//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/control_relay.hpp>

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/proto/error.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace vss::plugin::host
{
namespace json = boost::json;

std::string_view to_string(control_close_code code) noexcept
{
    switch (code)
    {
    case control_close_code::revoked:
        return "revoked";
    case control_close_code::expired:
        return "expired";
    case control_close_code::gateway_closed:
        return "gateway_closed";
    case control_close_code::refused_frame:
        return "refused_frame";
    case control_close_code::host_stopped:
        return "host_stopped";
    }
    return "gateway_closed";
}

// -- text --------------------------------------------------------------------

namespace
{

/// One character read from UTF-8; `length` is 0 for bytes that are not one.
struct utf8_char
{
    std::uint32_t point = 0;
    std::size_t length = 0;
};

utf8_char read_utf8(std::string_view text, std::size_t index) noexcept
{
    const auto at = [&](std::size_t offset)
    { return static_cast<unsigned char>(text[index + offset]); };
    const unsigned char lead = at(0);
    if (lead < 0x80)
        return {lead, 1};
    std::size_t length = 0;
    std::uint32_t point = 0;
    std::uint32_t least = 0;
    if ((lead & 0xe0) == 0xc0)
    {
        length = 2;
        point = lead & 0x1fU;
        least = 0x80;
    }
    else if ((lead & 0xf0) == 0xe0)
    {
        length = 3;
        point = lead & 0x0fU;
        least = 0x800;
    }
    else if ((lead & 0xf8) == 0xf0)
    {
        length = 4;
        point = lead & 0x07U;
        least = 0x10000;
    }
    else
    {
        return {};
    }
    if (text.size() - index < length)
        return {};
    for (std::size_t offset = 1; offset < length; ++offset)
    {
        const unsigned char next = at(offset);
        if ((next & 0xc0) != 0x80)
            return {};
        point = (point << 6) | (next & 0x3fU);
    }
    if (point < least || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff))
        return {};
    return {point, length};
}

/// Characters that a person cannot see or that change how text around them
/// reads.
bool unwanted(std::uint32_t point) noexcept
{
    return point < 0x20 || (point >= 0x7f && point <= 0x9f) ||
           (point >= 0x200b && point <= 0x200f) || point == 0x2028 ||
           point == 0x2029 || (point >= 0x202a && point <= 0x202e) ||
           (point >= 0x2060 && point <= 0x206f) || point == 0xfeff ||
           (point >= 0xfff9 && point <= 0xfffb);
}

} // namespace

std::string clean_control_text(std::string_view text, std::size_t most_bytes)
{
    std::string out;
    std::size_t index = 0;
    while (index < text.size())
    {
        const utf8_char next = read_utf8(text, index);
        if (next.length == 0)
        {
            ++index;
            continue;
        }
        const std::string_view piece = text.substr(index, next.length);
        index += next.length;
        if (unwanted(next.point))
            continue;
        if (out.size() + piece.size() > most_bytes)
            break;
        out.append(piece);
    }
    const std::size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    const std::size_t last = out.find_last_not_of(' ');
    return out.substr(first, last - first + 1);
}

// -- one connection to the gateway -------------------------------------------

namespace
{

constexpr std::size_t read_size = std::size_t{64} * 1024;
/// Until the gateway has answered `hello` it sends one small frame.
constexpr std::size_t greeting_frame_limit = std::size_t{64} * 1024;
constexpr std::uint64_t hello_id = 1;
/// The most scope names a request may list; there are eight scopes.
constexpr std::size_t most_scope_names = 32;

using steady_timer = boost::asio::steady_timer;

/// How long an administration call waits before it tries again after the
/// gateway said it is full.
constexpr std::chrono::milliseconds busy_wait{100};

/// What the gateway answers to an administration request: the reply, or the
/// error it carried.
using admin_results = std::vector<result<control::reply_body>>;
using admin_done = std::function<void(error_code, admin_results)>;

/// Overwrites a string's bytes.
void wipe(std::string& text) noexcept
{
    control::secure_wipe(text.data(), text.size());
}

/// A connection to one of the gateway's local endpoints, as a stream of
/// frames.  Handlers run on the executor the stream was made with.
///
/// Reading stops while a frame that the handler did not take is held, and
/// goes on when `resume` is called; so what waits is that frame and what
/// one read brought.  What is written waits in a queue the owner can see
/// the size of.
class link : public std::enable_shared_from_this<link>
{
public:
    /// Returns true when it took the frame.  The frame may be changed.
    using frame_handler = std::function<bool(agent::frame&)>;
    using close_handler = std::function<void(const error_code&)>;

    /// `sensitive` is for a connection that may carry a token in what it
    /// reads: the read buffer is wiped after each read.
    link(std::unique_ptr<agent::local_stream> stream, bool sensitive)
        : stream_(std::move(stream)), buffer_(read_size), sensitive_(sensitive)
    {
    }

    ~link()
    {
        for (outgoing& item : queue_)
        {
            if (item.secret)
                control::secure_wipe(reinterpret_cast<char*>(item.bytes.data()),
                                     item.bytes.size());
        }
    }

    link(const link&) = delete;
    link& operator=(const link&) = delete;

    static result<std::shared_ptr<link>>
    open(const boost::asio::any_io_executor& executor,
         agent::local_endpoint endpoint, std::string_view instance,
         bool sensitive)
    {
        auto connected = agent::connect_local(executor, endpoint, instance);
        if (!connected)
            return connected.error();
        return std::make_shared<link>(std::move(*connected), sensitive);
    }

    void start(frame_handler on_frame, close_handler on_close)
    {
        auto handlers = std::make_shared<callbacks>();
        handlers->on_frame = std::move(on_frame);
        handlers->on_close = std::move(on_close);
        callbacks_ = std::move(handlers);
        read_more();
    }

    void set_limit(std::size_t limit) noexcept { reader_.set_limit(limit); }

    /// Offers the held frame again and goes on reading.
    void resume()
    {
        if (closed_ || !paused_)
            return;
        paused_ = false;
        pump();
    }

    /// Queues bytes to be written.  `secret` bytes are wiped once written.
    void send(byte_buffer bytes, bool secret)
    {
        if (closed_)
        {
            if (secret)
                control::secure_wipe(reinterpret_cast<char*>(bytes.data()),
                                     bytes.size());
            return;
        }
        waiting_ += bytes.size();
        queue_.push_back(outgoing{std::move(bytes), secret});
        write_next();
    }

    std::size_t waiting_bytes() const noexcept { return waiting_; }

    /// Closes the connection.  No handler is called afterwards.
    void close()
    {
        if (closed_)
            return;
        closed_ = true;
        callbacks_.reset();
        stream_->close();
    }

private:
    struct callbacks
    {
        frame_handler on_frame;
        close_handler on_close;
    };

    struct outgoing
    {
        byte_buffer bytes;
        bool secret = false;
    };

    void fail(const error_code& why)
    {
        if (closed_)
            return;
        closed_ = true;
        stream_->close();
        const std::shared_ptr<callbacks> handlers = std::move(callbacks_);
        callbacks_.reset();
        if (handlers && handlers->on_close)
            handlers->on_close(why);
    }

    void read_more()
    {
        if (closed_ || reading_ || paused_)
            return;
        reading_ = true;
        stream_->async_read_some(
            boost::asio::buffer(buffer_),
            [self = shared_from_this()](boost::system::error_code failed,
                                        std::size_t size)
            {
                self->reading_ = false;
                if (self->closed_)
                    return;
                if (failed)
                {
                    self->fail(failed);
                    return;
                }
                self->reader_.feed(bytes_view(self->buffer_.data(), size));
                if (self->sensitive_)
                    control::secure_wipe(
                        reinterpret_cast<char*>(self->buffer_.data()), size);
                self->pump();
            });
    }

    /// Hands out the frames that have arrived, one at a time, until the
    /// handler does not take one.
    void pump()
    {
        for (;;)
        {
            if (closed_)
                return;
            if (!held_)
            {
                auto next = reader_.next();
                if (!next)
                {
                    fail(next.error());
                    return;
                }
                if (!*next)
                    break;
                held_ = std::move(**next);
            }
            const std::shared_ptr<callbacks> handlers = callbacks_;
            const bool taken =
                !handlers || !handlers->on_frame || handlers->on_frame(*held_);
            if (closed_)
                return;
            if (!taken)
            {
                paused_ = true;
                return;
            }
            held_.reset();
        }
        read_more();
    }

    void write_next()
    {
        if (writing_ || queue_.empty() || closed_)
            return;
        writing_ = true;
        offset_ = 0;
        write_some();
    }

    void write_some()
    {
        const outgoing& head = queue_.front();
        stream_->async_write_some(
            boost::asio::buffer(head.bytes.data() + offset_,
                                head.bytes.size() - offset_),
            [self = shared_from_this()](boost::system::error_code failed,
                                        std::size_t size)
            {
                if (self->closed_)
                    return;
                if (failed)
                {
                    self->fail(failed);
                    return;
                }
                outgoing& item = self->queue_.front();
                self->offset_ += size;
                self->waiting_ -= std::min(size, self->waiting_);
                if (self->offset_ < item.bytes.size())
                {
                    self->write_some();
                    return;
                }
                if (item.secret)
                    control::secure_wipe(
                        reinterpret_cast<char*>(item.bytes.data()),
                        item.bytes.size());
                self->queue_.pop_front();
                self->writing_ = false;
                self->write_next();
            });
    }

    std::unique_ptr<agent::local_stream> stream_;
    byte_buffer buffer_;
    bool sensitive_ = false;
    agent::frame_reader reader_;
    std::shared_ptr<callbacks> callbacks_;
    std::optional<agent::frame> held_;
    std::deque<outgoing> queue_;
    std::size_t offset_ = 0;
    std::size_t waiting_ = 0;
    bool reading_ = false;
    bool writing_ = false;
    bool paused_ = false;
    bool closed_ = false;
};

// -- calls on the administration endpoint ------------------------------------

/// Connects to the administration endpoint as this user, says `hello`
/// without a token, makes each request in turn and waits for its reply, all
/// within one time limit.  The result of each request is its reply or the
/// error the gateway answered with.  The gateway has room for two
/// connections on this endpoint, so a `hello` that it answers with `busy` is
/// tried again, within the same limit.  The job keeps itself alive until it
/// has finished.
class admin_job : public std::enable_shared_from_this<admin_job>
{
public:
    admin_job(const boost::asio::any_io_executor& executor,
              std::string instance, std::vector<control::request_body> requests,
              admin_done done)
        : executor_(executor), instance_(std::move(instance)), timer_(executor),
          retry_(executor), requests_(std::move(requests)),
          done_(std::move(done))
    {
    }

    static void run(const boost::asio::any_io_executor& executor,
                    std::string_view instance,
                    std::vector<control::request_body> requests,
                    std::chrono::milliseconds limit, admin_done done)
    {
        const auto job =
            std::make_shared<admin_job>(executor, std::string(instance),
                                        std::move(requests), std::move(done));
        job->begin(limit);
    }

private:
    void begin(std::chrono::milliseconds limit)
    {
        timer_.expires_after(limit);
        timer_.async_wait(
            [self = shared_from_this()](boost::system::error_code cancelled)
            {
                if (!cancelled)
                    self->finish(make_error_code(errc::timeout));
            });
        connect();
    }

    void connect()
    {
        auto connected = link::open(
            executor_, agent::local_endpoint::control_admin, instance_, true);
        if (!connected)
        {
            finish(connected.error());
            return;
        }
        step_ = 0;
        link_ = std::move(*connected);
        link_->start(
            [self = shared_from_this()](agent::frame& received)
            {
                self->on_frame(received);
                return true;
            },
            [self = shared_from_this()](const error_code&)
            {
                self->finish(agent::make_error_code(
                    agent::protocol_errc::connection_closed));
            });
        send_step();
    }

    /// The gateway is full: another try after a moment.
    void try_again_later()
    {
        link_->close();
        link_.reset();
        retry_.expires_after(busy_wait);
        retry_.async_wait(
            [self = shared_from_this()](boost::system::error_code cancelled)
            {
                if (!cancelled)
                    self->connect();
            });
    }

    /// Step 0 is the `hello`, step `n` the request `n - 1`.  The request id
    /// is the step plus one.
    void send_step()
    {
        control::request asked;
        asked.id = step_ + 1;
        if (step_ == 0)
            asked.body = control::hello_request{
                control::api_version, "plugin control relay", {}};
        else
            asked.body = requests_[step_ - 1];
        auto bytes = agent::encode_frame(control::encode(asked));
        if (!bytes)
        {
            finish(bytes.error());
            return;
        }
        link_->send(std::move(*bytes), false);
    }

    void on_frame(agent::frame& received)
    {
        auto decoded = control::decode_server_message(received);
        // The reply to a request that made a token holds the token.
        wipe(received.header);
        if (!decoded)
        {
            finish(decoded.error());
            return;
        }
        auto* answer = std::get_if<control::reply>(&*decoded);
        if (answer == nullptr)
            return;
        const auto* refused = std::get_if<control::error_reply>(&answer->body);
        // Id 0 is an error about the connection itself.
        if (answer->id != step_ + 1 && !(answer->id == 0 && refused))
            return;
        if (step_ == 0)
        {
            if (refused != nullptr)
            {
                const error_code why = control::from_api_error(refused->error);
                if (why == control::api_errc::busy)
                    try_again_later();
                else
                    finish(why);
                return;
            }
            const auto* welcome =
                std::get_if<control::hello_reply>(&answer->body);
            if (welcome == nullptr || welcome->api != control::api_version)
            {
                finish(make_error_code(control::api_errc::unsupported));
                return;
            }
        }
        else if (refused != nullptr)
        {
            results_.emplace_back(control::from_api_error(refused->error));
        }
        else
        {
            results_.emplace_back(std::move(answer->body));
        }
        ++step_;
        if (step_ > requests_.size())
            finish({});
        else
            send_step();
    }

    void finish(const error_code& why)
    {
        if (!done_)
            return;
        admin_done done = std::move(done_);
        done_ = nullptr;
        timer_.cancel();
        retry_.cancel();
        if (link_)
            link_->close();
        link_.reset();
        done(why, std::move(results_));
    }

    boost::asio::any_io_executor executor_;
    std::string instance_;
    steady_timer timer_;
    steady_timer retry_;
    std::vector<control::request_body> requests_;
    admin_done done_;
    std::shared_ptr<link> link_;
    std::size_t step_ = 0;
    admin_results results_;
};

// -- what a plugin sends -----------------------------------------------------

/// What is wrong with the header of a plugin's frame, or nothing.
std::string header_problem(const std::string& header)
{
    json::parse_options options;
    options.max_depth = control::max_json_depth;
    boost::system::error_code failed;
    const json::value parsed = json::parse(header, failed, {}, options);
    if (failed)
        return "The frame's header is not JSON that the gateway reads.";
    if (!parsed.is_object())
        return "The frame's header is not a JSON object.";
    const json::value* operation = parsed.as_object().if_contains("op");
    if (operation != nullptr && operation->is_string() &&
        operation->as_string() == "hello")
        return "A plugin may not say hello: the host does that.";
    return {};
}

/// True for an error reply of the gateway that says the session's token is
/// no longer good.
bool says_unauthenticated(const agent::frame& received)
{
    if (received.header.find("unauthenticated") == std::string::npos)
        return false;
    const auto decoded = control::decode_reply(received);
    if (!decoded)
        return false;
    const auto* refused = std::get_if<control::error_reply>(&decoded->body);
    return refused != nullptr && control::from_api_error(refused->error) ==
                                     control::api_errc::unauthenticated;
}

/// What to tell a person when the gateway could not be used.
std::string unreachable_text(const error_code& why)
{
    if (why == agent::protocol_errc::not_running)
        return "No control gateway is running.";
    if (why == errc::timeout)
        return "The control gateway did not answer in time.";
    if (why == agent::protocol_errc::not_same_user)
        return "The control gateway belongs to another user.";
    return "The control gateway could not be used.";
}

control_session_refused refused_with(control_refusal why, std::string text)
{
    control_session_refused out;
    out.why = why;
    out.text = std::move(text);
    return out;
}

} // namespace

// -- the relay ---------------------------------------------------------------

class control_relay::impl : public std::enable_shared_from_this<impl>
{
public:
    impl(boost::asio::any_io_executor executor, control_relay_options options,
         control_relay_handlers handlers)
        : executor_(std::move(executor)), options_(std::move(options)),
          handlers_(std::move(handlers))
    {
        options_.max_sessions =
            std::min(options_.max_sessions, max_control_sessions);
    }

    void request_session(const control_session_request& request,
                         answer_handler done)
    {
        std::vector<std::string> scopes;
        if (auto refusal = check(request, scopes))
        {
            answer_later(std::move(done), std::move(*refusal));
            return;
        }
        if (pending_)
        {
            answer_later(std::move(done),
                         refused_with(control_refusal::busy,
                                      "A session is being asked for "
                                      "already."));
            return;
        }
        if (sessions_.size() >= options_.max_sessions)
        {
            answer_later(std::move(done),
                         refused_with(control_refusal::busy,
                                      "The plugin has as many sessions as it "
                                      "may."));
            return;
        }
        if (!handlers_.approve)
        {
            answer_later(std::move(done),
                         refused_with(control_refusal::permission_denied,
                                      "No one can be asked to approve the "
                                      "session."));
            return;
        }

        control_approval_request ask;
        ask.plugin_id = options_.plugin_id;
        ask.plugin_name = options_.plugin_name;
        ask.peer = clean_control_text(request.peer, max_control_peer_size);
        ask.reason =
            clean_control_text(request.reason, max_control_reason_size);
        ask.scopes = scopes;
        ask.token_life = options_.token_life;

        pending_.emplace();
        pending_->done = std::move(done);
        pending_->scopes = std::move(scopes);
        pending_->label = "plugin:";
        pending_->label.append(clean_control_text(options_.plugin_id, 128));
        if (!ask.peer.empty())
        {
            pending_->label.push_back(' ');
            pending_->label.append(ask.peer);
        }

        // The owner may answer from any thread; the answer is posted to the
        // strand, and only the first one counts.
        const auto answered = std::make_shared<std::atomic<bool>>(false);
        std::function<void(bool)> reply =
            [weak = weak_from_this(), answered, epoch = epoch_](bool approved)
        {
            if (answered->exchange(true))
                return;
            if (const auto self = weak.lock())
            {
                boost::asio::post(
                    self->executor_, [self, epoch, approved]
                    { self->approval_answered(epoch, approved); });
            }
        };
        handlers_.approve(ask, std::move(reply));
    }

    void frame_from_plugin(std::uint64_t number, std::string header,
                           byte_buffer blob)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end() || found->second.state != stage::open)
        {
            if (handlers_.closed)
                handlers_.closed(number, control_close_code::refused_frame,
                                 "The session is not open.");
            return;
        }
        std::string problem = header_problem(header);
        std::optional<byte_buffer> bytes;
        if (problem.empty())
        {
            agent::frame sending;
            sending.header = std::move(header);
            sending.blob = std::move(blob);
            auto encoded = agent::encode_frame(sending);
            if (encoded)
                bytes = std::move(*encoded);
            else
                problem =
                    "The frame is empty or longer than the gateway reads.";
        }
        if (!bytes)
        {
            // `problem` is set whenever there are no bytes.
            say("session " + std::to_string(number) + " closed: " + problem);
            end_session(number, control_close_code::refused_frame, problem);
            return;
        }
        link& connection = *found->second.connection;
        if (connection.waiting_bytes() + bytes->size() >
            options_.max_waiting_bytes)
        {
            const std::string text =
                "The gateway did not take frames as fast as the plugin sent "
                "them.";
            say("session " + std::to_string(number) + " closed: " + text);
            end_session(number, control_close_code::refused_frame, text);
            return;
        }
        connection.send(std::move(*bytes), false);
    }

    void plugin_has_room(std::uint64_t number)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end() || found->second.state != stage::open)
            return;
        // The link may end the session while it offers the frame again.
        const std::shared_ptr<link> connection = found->second.connection;
        connection->resume();
    }

    void close_session(std::uint64_t number)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end() || found->second.state != stage::open)
            return;
        say("session " + std::to_string(number) + " closed by the host");
        end_session(number, control_close_code::host_stopped,
                    "The host closed the session.");
    }

    void close_all(std::function<void()> done)
    {
        ++epoch_;
        std::vector<std::string> tokens;
        std::vector<std::uint64_t> open_sessions;
        for (auto& [number, current] : sessions_)
        {
            tokens.push_back(current.token_id);
            if (current.state == stage::open)
                open_sessions.push_back(number);
            if (current.timer)
                current.timer->cancel();
            if (current.connection)
                current.connection->close();
        }
        sessions_.clear();
        if (pending_)
        {
            answer_later(std::move(pending_->done),
                         refused_with(control_refusal::plugin_unavailable,
                                      "The relay was closed."));
            pending_.reset();
        }
        revoke(std::move(tokens), std::move(done));
        if (handlers_.closed)
        {
            for (const std::uint64_t number : open_sessions)
                handlers_.closed(number, control_close_code::host_stopped,
                                 "The host closed the session.");
        }
    }

    /// For the relay's destructor: no more calls to the owner.
    void shut_down()
    {
        pending_.reset();
        handlers_ = {};
        close_all({});
    }

    std::size_t session_count() const noexcept { return sessions_.size(); }

private:
    enum class stage
    {
        /// `hello` is sent; the gateway has not answered.
        greeting,
        open
    };

    struct session
    {
        std::string token_id;
        stage state = stage::greeting;
        std::uint64_t expires_ms = 0;
        std::shared_ptr<link> connection;
        std::shared_ptr<steady_timer> timer;
    };

    struct admin_task
    {
        std::vector<control::request_body> requests;
        std::chrono::milliseconds limit{0};
        admin_done done;
    };

    /// The request that is being worked on, from the owner's approval to
    /// the gateway's `hello` reply.
    struct pending_request
    {
        answer_handler done;
        std::vector<std::string> scopes;
        std::string label;
    };

    void say(std::string_view line) const
    {
        if (!handlers_.log)
            return;
        std::string text = "control relay ";
        text.append(clean_control_text(options_.plugin_id, 128));
        text.append(": ");
        text.append(line);
        handlers_.log(text);
    }

    /// Calls `done` on the strand, after the caller has returned.
    void answer_later(answer_handler done, control_session_refused answer)
    {
        if (!done)
            return;
        boost::asio::post(executor_, [done = std::move(done),
                                      answer = std::move(answer)]() mutable
                          { done(control_session_answer(std::move(answer))); });
    }

    /// The reason a request is refused before anyone is asked, or nothing.
    /// `scopes` is the list that will be asked of the gateway.
    std::optional<control_session_refused>
    check(const control_session_request& request,
          std::vector<std::string>& scopes) const
    {
        if (request.peer.size() > max_control_peer_size)
            return refused_with(control_refusal::invalid_request,
                                "The peer text is longer than 64 bytes.");
        if (request.reason.size() > max_control_reason_size)
            return refused_with(control_refusal::invalid_request,
                                "The reason is longer than 256 bytes.");
        if (request.scopes.empty() || request.scopes.size() > most_scope_names)
            return refused_with(control_refusal::invalid_request,
                                "The request names no scope, or too many.");
        control::scope_set wanted;
        bool unknown = false;
        for (const std::string& name : request.scopes)
        {
            const auto parsed = control::parse_scope(name);
            if (!parsed)
                unknown = true;
            else if (*parsed == control::scope::admin)
                return refused_with(control_refusal::permission_denied,
                                    "A plugin cannot have the administration "
                                    "scope.");
            else
                wanted = wanted | control::scope_set{*parsed};
        }
        if (unknown)
            return refused_with(control_refusal::invalid_request,
                                "The request names a scope that does not "
                                "exist.");
        scopes = control::scope_names(wanted);
        return std::nullopt;
    }

    /// Answers the open request, once.
    void finish_request(control_session_answer answer)
    {
        if (!pending_)
            return;
        answer_handler done = std::move(pending_->done);
        pending_.reset();
        if (done)
            done(std::move(answer));
    }

    void refuse_request(control_refusal why, std::string text)
    {
        say("request refused: " + text);
        finish_request(refused_with(why, std::move(text)));
    }

    void approval_answered(std::uint64_t epoch, bool approved)
    {
        if (epoch != epoch_ || !pending_)
            return;
        if (!approved)
        {
            refuse_request(control_refusal::permission_denied,
                           "The request was not approved.");
            return;
        }
        control::tokens_create_request create;
        create.label = pending_->label;
        create.scopes = pending_->scopes;
        create.life_s =
            static_cast<std::uint64_t>(std::max<std::chrono::seconds::rep>(
                options_.token_life.count(), 1));
        create.transports = {"local"};
        run_admin({std::move(create)}, options_.step_timeout,
                  [self = shared_from_this(), epoch](error_code failed,
                                                     admin_results results)
                  { self->token_made(epoch, failed, std::move(results)); });
    }

    void token_made(std::uint64_t epoch, const error_code& failed,
                    admin_results results)
    {
        std::optional<control::token_created_reply> created;
        std::string problem;
        if (failed)
        {
            problem = failed.category() == control::api_category()
                          ? "The control gateway did not make a token: " +
                                failed.message()
                          : unreachable_text(failed);
        }
        else if (results.size() != 1 || !results.front())
        {
            problem = results.size() == 1
                          ? "The control gateway did not make a token: " +
                                results.front().error().message()
                          : "The control gateway did not make a token.";
        }
        else if (auto* reply = std::get_if<control::token_created_reply>(
                     &*results.front()))
        {
            created = std::move(*reply);
        }
        else
        {
            problem = "The control gateway did not make a token.";
        }

        if (epoch != epoch_)
        {
            // The relay was closed meanwhile and the request answered.  A
            // token that exists anyway is not left behind.
            if (created)
                revoke({created->entry.id}, {});
            return;
        }
        if (!created)
        {
            refuse_request(control_refusal::plugin_unavailable,
                           std::move(problem));
            return;
        }
        open_connection(std::move(*created));
    }

    void open_connection(control::token_created_reply created)
    {
        const std::string token_id = created.entry.id;
        auto connected = link::open(executor_, agent::local_endpoint::control,
                                    options_.instance, false);
        if (!connected)
        {
            revoke({token_id}, {});
            refuse_request(control_refusal::plugin_unavailable,
                           unreachable_text(connected.error()));
            return;
        }
        const std::uint64_t number = next_session_++;
        session& current = sessions_[number];
        current.token_id = token_id;
        current.expires_ms = created.entry.expires_ms;
        current.connection = std::move(*connected);
        current.timer = std::make_shared<steady_timer>(executor_);
        current.timer->expires_after(options_.step_timeout);
        current.timer->async_wait(
            [weak = weak_from_this(),
             number](boost::system::error_code cancelled)
            {
                if (cancelled)
                    return;
                if (const auto self = weak.lock())
                    self->greeting_timed_out(number);
            });
        current.connection->set_limit(greeting_frame_limit);
        current.connection->start(
            [weak = weak_from_this(), number](agent::frame& received)
            {
                const auto self = weak.lock();
                return !self || self->frame_from_gateway(number, received);
            },
            [weak = weak_from_this(), number](const error_code&)
            {
                if (const auto self = weak.lock())
                    self->link_closed(number);
            });

        // The first frame of the connection, and the only place the token
        // is used.  Everything that holds it is wiped once it is written.
        control::request asked;
        asked.id = hello_id;
        control::hello_request greeting;
        greeting.api = control::api_version;
        greeting.client = "plugin:";
        greeting.client.append(clean_control_text(options_.plugin_id, 128));
        greeting.token = std::move(created.token);
        asked.body = std::move(greeting);
        agent::frame encoded = control::encode(asked);
        auto bytes = agent::encode_frame(encoded);
        wipe(encoded.header);
        if (!bytes)
        {
            drop_session(number);
            refuse_request(control_refusal::plugin_unavailable,
                           "The hello for the gateway could not be made.");
            return;
        }
        current.connection->send(std::move(*bytes), true);
    }

    void greeting_timed_out(std::uint64_t number)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end() || found->second.state != stage::greeting)
            return;
        drop_session(number);
        refuse_request(control_refusal::plugin_unavailable,
                       "The control gateway did not answer hello in time.");
    }

    void link_closed(std::uint64_t number)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end())
            return;
        if (found->second.state == stage::greeting)
        {
            drop_session(number);
            refuse_request(control_refusal::plugin_unavailable,
                           "The control gateway closed the connection before "
                           "it answered hello.");
            return;
        }
        if (found->second.expires_ms <= now_ms())
        {
            const std::string text = "The session's token has ended.";
            say("session " + std::to_string(number) + " closed: " + text);
            end_session(number, control_close_code::expired, text);
            return;
        }
        const std::string text = "The control gateway closed the connection.";
        say("session " + std::to_string(number) + " closed: " + text);
        end_session(number, control_close_code::gateway_closed, text);
    }

    /// Returns true when the frame was taken.
    bool frame_from_gateway(std::uint64_t number, agent::frame& received)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end())
            return true;
        if (found->second.state == stage::greeting)
        {
            greeted(number, received);
            return true;
        }
        const bool ends = says_unauthenticated(received);
        const std::uint64_t expires_ms = found->second.expires_ms;
        bool taken = true;
        if (handlers_.to_plugin)
            taken = handlers_.to_plugin(number, received.header,
                                        bytes_view(received.blob));
        if (ends)
        {
            // The gateway says the same when a token ends as when it is
            // taken back; the time tells which.
            const bool ended = expires_ms <= now_ms();
            const std::string text =
                ended ? "The session's token has ended."
                      : "The control gateway no longer accepts the session's "
                        "token.";
            say("session " + std::to_string(number) + " closed: " + text);
            end_session(number,
                        ended ? control_close_code::expired
                              : control_close_code::revoked,
                        text);
            return true;
        }
        return taken;
    }

    /// The gateway's answer to `hello`.  The session opens only when the
    /// gateway says it is the session of this token.
    void greeted(std::uint64_t number, const agent::frame& received)
    {
        const auto decoded = control::decode_server_message(received);
        if (!decoded)
        {
            greeting_failed(number,
                            "The control gateway did not answer hello as "
                            "expected.");
            return;
        }
        const auto* answer = std::get_if<control::reply>(&*decoded);
        if (answer == nullptr)
            return;
        const auto* refused = std::get_if<control::error_reply>(&answer->body);
        if (answer->id != hello_id && !(answer->id == 0 && refused))
            return;
        if (refused != nullptr)
        {
            greeting_failed(
                number, "The control gateway did not take the session's "
                        "token: " +
                            control::from_api_error(refused->error).message());
            return;
        }
        const auto* welcome = std::get_if<control::hello_reply>(&answer->body);
        const auto found = sessions_.find(number);
        if (welcome == nullptr || found == sessions_.end() ||
            welcome->api != control::api_version ||
            welcome->principal != "token:" + found->second.token_id)
        {
            // Whatever else this is, it is not a connection that is the
            // token's, and no frame of the plugin goes on it.
            greeting_failed(number,
                            "The control gateway did not take the connection "
                            "as the session's token.");
            return;
        }
        session& current = found->second;
        current.state = stage::open;
        current.timer->cancel();
        current.connection->set_limit(agent::max_frame_size);

        control_session_opened opened;
        opened.session = number;
        opened.scopes = welcome->scopes;
        const std::string scope_text = [&]
        {
            std::string joined;
            for (const std::string& name : opened.scopes)
            {
                if (!joined.empty())
                    joined.push_back(' ');
                joined.append(name);
            }
            return joined;
        }();
        say("session " + std::to_string(number) + " open with " + scope_text);
        opened.expires_ms = current.expires_ms;
        finish_request(control_session_answer(std::move(opened)));
    }

    void greeting_failed(std::uint64_t number, std::string text)
    {
        drop_session(number);
        refuse_request(control_refusal::plugin_unavailable, std::move(text));
    }

    /// Takes a session out, closes its connection and revokes its token.
    void drop_session(std::uint64_t number)
    {
        const auto found = sessions_.find(number);
        if (found == sessions_.end())
            return;
        session gone = std::move(found->second);
        sessions_.erase(found);
        if (gone.timer)
            gone.timer->cancel();
        if (gone.connection)
            gone.connection->close();
        revoke({gone.token_id}, {});
    }

    /// Takes an open session out as `drop_session` does, and tells the
    /// owner why.
    void end_session(std::uint64_t number, control_close_code code,
                     const std::string& reason)
    {
        if (sessions_.find(number) == sessions_.end())
            return;
        drop_session(number);
        if (handlers_.closed)
            handlers_.closed(number, code, reason);
    }

    static std::uint64_t now_ms()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    /// Makes calls on the administration endpoint one after the other: the
    /// gateway has room for two connections there, and Studio may hold one.
    void run_admin(std::vector<control::request_body> requests,
                   std::chrono::milliseconds limit, admin_done done)
    {
        admin_queue_.push_back(
            admin_task{std::move(requests), limit, std::move(done)});
        start_next_admin();
    }

    void start_next_admin()
    {
        while (!admin_busy_ && !admin_queue_.empty())
        {
            admin_task task = std::move(admin_queue_.front());
            admin_queue_.pop_front();
            if (task.requests.empty())
            {
                // Nothing to ask: a mark in the line, so that what was asked
                // before it has been answered when its `done` runs.
                boost::asio::post(executor_, [done = std::move(task.done)]
                                  { done(error_code(), admin_results()); });
                continue;
            }
            admin_busy_ = true;
            admin_job::run(executor_, options_.instance,
                           std::move(task.requests), task.limit,
                           [self = shared_from_this(),
                            done = std::move(task.done)](error_code failed,
                                                         admin_results results)
                           {
                               self->admin_busy_ = false;
                               done(failed, std::move(results));
                               self->start_next_admin();
                           });
        }
    }

    /// Revokes tokens on the administration endpoint, as one try within the
    /// time limit.  Nothing is retried; a token the gateway could not be
    /// asked about ends when its life does.
    void revoke(std::vector<std::string> tokens, std::function<void()> done)
    {
        if (tokens.empty())
        {
            // Done when the revoking that is under way has been done.
            if (done)
                run_admin({}, {},
                          [done = std::move(done)](
                              error_code, const admin_results&) { done(); });
            return;
        }
        std::vector<control::request_body> requests;
        requests.reserve(tokens.size());
        for (const std::string& id : tokens)
            requests.emplace_back(control::tokens_revoke_request{id});
        run_admin(std::move(requests), options_.revoke_timeout,
                  [self = shared_from_this(), tokens = std::move(tokens),
                   done = std::move(done)](error_code failed,
                                           const admin_results& results)
                  {
                      self->revoked(tokens, failed, results);
                      if (done)
                          done();
                  });
    }

    void revoked(const std::vector<std::string>& tokens,
                 const error_code& failed, const admin_results& results)
    {
        for (std::size_t index = 0; index < tokens.size(); ++index)
        {
            std::string line = "token ";
            line.append(tokens[index]);
            if (index < results.size() && results[index])
            {
                line.append(" revoked");
                say(line);
                continue;
            }
            line.append(" not revoked: ");
            if (failed)
                line.append(unreachable_text(failed));
            else if (index < results.size())
                line.append(results[index].error().message());
            else
                line.append("no answer");
            say(line);
        }
    }

    boost::asio::any_io_executor executor_;
    control_relay_options options_;
    control_relay_handlers handlers_;
    std::map<std::uint64_t, session> sessions_;
    std::deque<admin_task> admin_queue_;
    bool admin_busy_ = false;
    std::optional<pending_request> pending_;
    std::uint64_t next_session_ = 1;
    /// Changes when the relay is closed, so that what was started before
    /// does not go on after.
    std::uint64_t epoch_ = 0;
};

control_relay::control_relay(std::shared_ptr<impl> state)
    : impl_(std::move(state))
{
}

control_relay::~control_relay()
{
    try
    {
        impl_->shut_down();
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        // Nothing can be told to anyone from here, and the connections are
        // closed by their owners as the relay goes.
    }
}

std::shared_ptr<control_relay>
control_relay::create(const boost::asio::any_io_executor& executor,
                      control_relay_options options,
                      control_relay_handlers handlers)
{
    return std::shared_ptr<control_relay>(
        new control_relay(std::make_shared<impl>(executor, std::move(options),
                                                 std::move(handlers))));
}

void control_relay::request_session(const control_session_request& request,
                                    answer_handler done)
{
    impl_->request_session(request, std::move(done));
}

void control_relay::frame_from_plugin(std::uint64_t session, std::string header,
                                      byte_buffer blob)
{
    impl_->frame_from_plugin(session, std::move(header), std::move(blob));
}

void control_relay::plugin_has_room(std::uint64_t session)
{
    impl_->plugin_has_room(session);
}

void control_relay::close_session(std::uint64_t session)
{
    impl_->close_session(session);
}

void control_relay::close_all(std::function<void()> done)
{
    impl_->close_all(std::move(done));
}

std::size_t control_relay::session_count() const noexcept
{
    return impl_->session_count();
}

} // namespace vss::plugin::host
