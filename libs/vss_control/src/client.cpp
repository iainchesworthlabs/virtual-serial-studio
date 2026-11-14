//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/client.hpp>
#include <vss/control/loopback.hpp>

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>

#include <boost/asio/io_context.hpp>

#include <deque>
#include <functional>
#include <utility>
#include <variant>

namespace vss::control
{
namespace
{

using steady = std::chrono::steady_clock;

constexpr std::size_t read_buffer_size = std::size_t{64} * 1024;
/// Batches kept while the caller is busy with requests.  Older ones go.
constexpr std::size_t max_kept_batches = 256;

/// What a write in flight reports to.  Shared with its handler, so a call
/// that gave up waiting leaves nothing for the handler to dangle on.
struct write_progress
{
    bool done = false;
    boost::system::error_code failed;
    std::size_t size = 0;
};

} // namespace

class client::impl
{
public:
    explicit impl(client_options options)
        : options_(std::move(options)), buffer_(read_buffer_size)
    {
    }

    error_code open()
    {
        if (options_.loopback_port)
        {
            // Never without a token: over TCP nobody is the local user.
            if (options_.admin || options_.token.empty())
                return make_error_code(errc::invalid_argument);
            auto reached =
                connect_loopback(io_.get_executor(), *options_.loopback_port);
            if (!reached)
                return agent::make_error_code(
                    agent::protocol_errc::not_running);
            stream_ = std::move(*reached);
            return send_raw(bytes_view(reinterpret_cast<const byte*>(
                                           control_preface.data()),
                                       control_preface.size()),
                            steady::now() + options_.timeout);
        }
        auto connected = agent::connect_local(
            io_.get_executor(),
            options_.admin ? agent::local_endpoint::control_admin
                           : agent::local_endpoint::control,
            options_.instance);
        if (!connected)
            return connected.error();
        stream_ = std::move(*connected);
        return {};
    }

    /// Sends a request and waits for its reply.  An error reply comes back
    /// as the error it carries.
    result<reply_body> call(request_body body,
                            std::chrono::milliseconds longer = {})
    {
        if (broken_)
            return agent::make_error_code(
                agent::protocol_errc::connection_closed);
        request asked;
        asked.id = next_id_++;
        asked.body = std::move(body);
        const auto deadline = steady::now() + options_.timeout + longer;

        const error_code unsent = send(encode(asked), deadline);
        if (unsent == errc::timeout)
        {
            drop();
            return unsent;
        }
        // When the write failed the gateway may have said why before it
        // closed the connection (`busy`), so what it sent is still read.
        for (;;)
        {
            auto message = receive(deadline);
            if (!message)
            {
                drop();
                return unsent ? agent::make_error_code(
                                    agent::protocol_errc::connection_closed)
                              : message.error();
            }
            if (auto* batch = std::get_if<event_batch>(&*message))
            {
                keep(std::move(*batch));
                continue;
            }
            reply& answer = std::get<reply>(*message);
            const auto* refused = std::get_if<error_reply>(&answer.body);
            // Id 0 is an error about the connection itself.
            if (answer.id != asked.id && !(answer.id == 0 && refused))
                continue;
            if (refused != nullptr)
            {
                last_message = refused->error.message;
                return from_api_error(refused->error);
            }
            return std::move(answer.body);
        }
    }

    result<event_batch> next_events(std::chrono::milliseconds limit)
    {
        const auto deadline = steady::now() + limit;
        for (;;)
        {
            if (!events_.empty())
            {
                event_batch batch = std::move(events_.front());
                events_.pop_front();
                return batch;
            }
            if (broken_)
                return agent::make_error_code(
                    agent::protocol_errc::connection_closed);
            auto message = receive(deadline);
            if (!message)
            {
                // Nothing came in time: not a fault of the connection.
                if (message.error() != errc::timeout)
                    drop();
                return message.error();
            }
            if (auto* batch = std::get_if<event_batch>(&*message))
            {
                keep(std::move(*batch));
                continue;
            }
            // No request is waiting, so an error with no id is the gateway
            // saying why it ends the session.  Anything else is the late
            // answer to a request that was given up on.
            const reply& answer = std::get<reply>(*message);
            const auto* refused = std::get_if<error_reply>(&answer.body);
            if (answer.id == 0 && refused != nullptr)
            {
                last_message = refused->error.message;
                const error_code why = from_api_error(refused->error);
                drop();
                return why;
            }
        }
    }

    /// Wipes the token the options came with.  Called once the hello is
    /// answered: after that the client has no use for it and keeps none.
    void forget_token() { options_.token = secret_string(); }

    hello_reply hello;
    std::string last_message;

private:
    void keep(event_batch batch)
    {
        if (events_.size() >= max_kept_batches)
            events_.pop_front();
        events_.push_back(std::move(batch));
    }

    void drop()
    {
        broken_ = true;
        if (stream_)
            stream_->close();
    }

    /// Runs the connection's handlers until `finished` or the deadline.
    bool wait(const std::function<bool()>& finished,
              steady::time_point deadline)
    {
        while (!finished())
        {
            const auto now = steady::now();
            if (now >= deadline)
            {
                // What has already happened is still taken, so a caller with
                // no time to wait gets what has arrived.
                io_.restart();
                io_.poll();
                return finished();
            }
            io_.restart();
            io_.run_one_for(deadline - now);
        }
        return true;
    }

    error_code send(const frame& value, steady::time_point deadline)
    {
        auto encoded = agent::encode_frame(value);
        if (!encoded)
            return encoded.error();
        return send_raw(bytes_view(*encoded), deadline);
    }

    error_code send_raw(bytes_view raw, steady::time_point deadline)
    {
        // Shared with the handler: a write that is given up on may still be
        // looking at the bytes when this function has returned.
        const auto bytes =
            std::make_shared<const byte_buffer>(raw.begin(), raw.end());
        std::size_t offset = 0;
        while (offset < bytes->size())
        {
            const auto progress = std::make_shared<write_progress>();
            stream_->async_write_some(
                boost::asio::buffer(bytes->data() + offset,
                                    bytes->size() - offset),
                [progress, bytes](boost::system::error_code failed,
                                  std::size_t size)
                {
                    progress->done = true;
                    progress->failed = failed;
                    progress->size = size;
                });
            if (!wait([&progress] { return progress->done; }, deadline))
                return make_error_code(errc::timeout);
            if (progress->failed)
                return agent::make_error_code(
                    agent::protocol_errc::connection_closed);
            offset += progress->size;
        }
        return {};
    }

    void start_read()
    {
        if (reading_)
            return;
        reading_ = true;
        // `this` is safe: the handler only ever runs from `wait`, and the
        // io_context that holds it is a member that is destroyed first.
        stream_->async_read_some(
            boost::asio::buffer(buffer_),
            [this](boost::system::error_code failed, std::size_t size)
            {
                reading_ = false;
                if (failed)
                    read_failed_ = true;
                else
                    reader_.feed(bytes_view(buffer_.data(), size));
            });
    }

    /// The next message from the gateway.  `errc::timeout` leaves the read in
    /// flight, so a later call goes on where this one stopped.
    result<server_message> receive(steady::time_point deadline)
    {
        for (;;)
        {
            auto next = reader_.next();
            if (!next)
                return next.error();
            const std::optional<frame> ready = std::move(*next);
            if (ready)
                return decode_server_message(*ready);
            if (read_failed_)
                return agent::make_error_code(
                    agent::protocol_errc::connection_closed);
            start_read();
            if (!wait([this] { return !reading_; }, deadline))
                return make_error_code(errc::timeout);
        }
    }

    client_options options_;
    // The order matters when the client is destroyed.  The stream goes
    // first, which ends a read that is still waiting; then the io_context,
    // which destroys that read's handler without running it; and the buffer
    // the read was given goes last.
    byte_buffer buffer_;
    boost::asio::io_context io_;
    std::unique_ptr<agent::local_stream> stream_;
    agent::frame_reader reader_;
    std::deque<event_batch> events_;
    std::uint64_t next_id_ = 1;
    bool reading_ = false;
    bool read_failed_ = false;
    bool broken_ = false;
};

client::client(std::unique_ptr<impl> state) : impl_(std::move(state)) {}

client::~client() = default;

result<std::unique_ptr<client>> client::connect(const client_options& options)
{
    auto state = std::make_unique<impl>(options);
    if (const error_code failed = state->open())
        return failed;
    hello_request greeting;
    greeting.api = api_version;
    greeting.client = options.name;
    greeting.token = options.token;
    auto answer = state->call(std::move(greeting));
    state->forget_token();
    if (!answer)
        return answer.error();
    auto* welcome = std::get_if<hello_reply>(&*answer);
    if (welcome == nullptr)
        return make_error_code(api_errc::invalid_argument);
    if (welcome->api != api_version)
        return make_error_code(api_errc::unsupported);
    state->hello = std::move(*welcome);
    return std::unique_ptr<client>(new client(std::move(state)));
}

const hello_reply& client::gateway() const noexcept
{
    return impl_->hello;
}

result<status_reply> client::status()
{
    auto answer = impl_->call(status_request{});
    if (!answer)
        return answer.error();
    auto* state = std::get_if<status_reply>(&*answer);
    if (state == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(*state);
}

result<ports_reply> client::list_ports()
{
    auto answer = impl_->call(ports_list_request{});
    if (!answer)
        return answer.error();
    auto* listed = std::get_if<ports_reply>(&*answer);
    if (listed == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(*listed);
}

namespace
{

result<std::string> made(result<reply_body> answer)
{
    if (!answer)
        return answer.error();
    auto* created = std::get_if<created_reply>(&*answer);
    if (created == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(created->id);
}

result<void> carried_out(const result<reply_body>& answer)
{
    if (!answer)
        return answer.error();
    if (!std::holds_alternative<done_reply>(*answer))
        return make_error_code(api_errc::invalid_argument);
    return {};
}

} // namespace

result<std::string> client::create_port(const std::string& name,
                                        const std::string& kind)
{
    return made(impl_->call(ports_create_request{name, kind}));
}

result<void> client::remove_port(const std::string& port)
{
    return carried_out(impl_->call(ports_remove_request{port}));
}

result<std::string> client::connect_ports(const std::string& first,
                                          const std::string& second,
                                          bool one_way)
{
    return made(impl_->call(links_connect_request{first, second, one_way}));
}

result<void> client::send(const std::string& port, bytes_view data)
{
    ports_send_request asking;
    asking.port = port;
    asking.data.assign(data.begin(), data.end());
    return carried_out(impl_->call(std::move(asking)));
}

result<matched_reply> client::expect_bytes(const expect_bytes_request& wanted)
{
    auto answer =
        impl_->call(wanted, std::chrono::milliseconds(wanted.timeout_ms));
    if (!answer)
        return answer.error();
    const auto* found = std::get_if<matched_reply>(&*answer);
    if (found == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return *found;
}

namespace
{

result<run_reply> run_of(result<reply_body> answer)
{
    if (!answer)
        return answer.error();
    auto* run = std::get_if<run_reply>(&*answer);
    if (run == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(*run);
}

} // namespace

result<run_reply> client::begin_run(const std::string& name)
{
    return run_of(impl_->call(runs_begin_request{name}));
}

result<run_reply> client::end_run()
{
    return run_of(impl_->call(runs_end_request{}));
}

result<void> client::record_in_run(const runs_record_request& noted)
{
    return carried_out(impl_->call(noted));
}

result<std::string> client::report_junit(const std::string& run)
{
    auto answer = impl_->call(report_junit_request{run});
    if (!answer)
        return answer.error();
    auto* report = std::get_if<report_reply>(&*answer);
    if (report == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(report->xml);
}

result<void> client::stop()
{
    auto answer = impl_->call(endpoint_stop_request{});
    if (!answer)
        return answer.error();
    if (!std::holds_alternative<done_reply>(*answer))
        return make_error_code(api_errc::invalid_argument);
    return {};
}

result<token_created_reply> client::create_token(
    const std::string& label, const std::vector<std::string>& scopes,
    std::uint64_t life_s, const std::vector<std::string>& transports)
{
    tokens_create_request asking;
    asking.label = label;
    asking.scopes = scopes;
    asking.life_s = life_s;
    asking.transports = transports;
    auto answer = impl_->call(std::move(asking));
    if (!answer)
        return answer.error();
    auto* created = std::get_if<token_created_reply>(&*answer);
    if (created == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(*created);
}

result<tokens_reply> client::list_tokens()
{
    auto answer = impl_->call(tokens_list_request{});
    if (!answer)
        return answer.error();
    auto* listed = std::get_if<tokens_reply>(&*answer);
    if (listed == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(*listed);
}

result<void> client::revoke_token(const std::string& id)
{
    tokens_revoke_request asking;
    asking.id = id;
    auto answer = impl_->call(std::move(asking));
    if (!answer)
        return answer.error();
    if (!std::holds_alternative<done_reply>(*answer))
        return make_error_code(api_errc::invalid_argument);
    return {};
}

namespace
{

/// The answer to a subscription, or the error it was refused with.
result<subscribed_reply> subscribed(result<reply_body> answer)
{
    if (!answer)
        return answer.error();
    const auto* started = std::get_if<subscribed_reply>(&*answer);
    if (started == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return *started;
}

result<void> done(const result<reply_body>& answer)
{
    if (!answer)
        return answer.error();
    if (!std::holds_alternative<done_reply>(*answer))
        return make_error_code(api_errc::invalid_argument);
    return {};
}

} // namespace

result<subscribed_reply> client::subscribe_events(const stream_options& options)
{
    return subscribed(impl_->call(events_subscribe_request{options}));
}

result<subscribed_reply>
client::subscribe_traffic(const stream_options& options)
{
    return subscribed(impl_->call(traffic_subscribe_request{options}));
}

result<void> client::unsubscribe_events()
{
    return done(impl_->call(events_unsubscribe_request{}));
}

result<void> client::unsubscribe_traffic()
{
    return done(impl_->call(traffic_unsubscribe_request{}));
}

result<event_batch> client::next_events(std::chrono::milliseconds limit)
{
    return impl_->next_events(limit);
}

result<sessions_reply> client::list_sessions()
{
    auto answer = impl_->call(sessions_list_request{});
    if (!answer)
        return answer.error();
    auto* listed = std::get_if<sessions_reply>(&*answer);
    if (listed == nullptr)
        return make_error_code(api_errc::invalid_argument);
    return std::move(*listed);
}

result<void> client::kick_session(std::uint64_t number)
{
    return done(impl_->call(sessions_kick_request{number}));
}

const std::string& client::last_error_message() const noexcept
{
    return impl_->last_message;
}

} // namespace vss::control
