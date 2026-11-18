//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/server.hpp>

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/audit_log.hpp>
#include <vss/control/private_file.hpp>

#include "json_support.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace vss::debug
{
namespace
{

namespace json = boost::json;
namespace agent = vss::agent;

/// Before a client has said hello a frame may have this many bytes.
constexpr std::size_t hello_frame_limit = 4096;
/// After it, a request is a small header.
constexpr std::size_t request_frame_limit = std::size_t{64} * 1024;
/// A client that does not read is closed when this much waits for it.
constexpr std::size_t write_backlog_limit = std::size_t{4} * 1024 * 1024;
constexpr std::size_t audit_text_limit = 200;
constexpr auto trace_poll = std::chrono::milliseconds(100);
constexpr std::size_t trace_event_limit = 500;
constexpr std::size_t trace_read_default = 200;
constexpr std::size_t trace_read_most = 2000;

std::string clean_name(std::string_view text)
{
    std::string out;
    for (const char letter : text)
    {
        if (out.size() >= 64)
            break;
        const auto code = static_cast<unsigned char>(letter);
        out += (code < 0x20 || code == 0x7f) ? '?' : letter;
    }
    return out;
}

std::string cut(std::string text, std::size_t most)
{
    if (text.size() > most)
        text.resize(most);
    return text;
}

std::uint64_t current_process_id() noexcept
{
    return agent::current_process_id();
}

} // namespace

class server::impl : public std::enable_shared_from_this<impl>
{
public:
    class connection;

    explicit impl(server_options options)
        : options_(std::move(options)), pool_(2), poll_(io_)
    {
        if (options_.recorder == nullptr)
            options_.recorder = &trace::recorder::global();
        if (options_.instance.empty())
            options_.instance = std::to_string(current_process_id());
    }

    ~impl()
    {
        try
        {
            stop();
        }
        catch (...)
        {
            // best effort: nothing can be done about a failure to stop
            // while the object is being destroyed.
        }
    }

    result<void> begin()
    {
        auto listener = agent::listen_local(io_.get_executor(),
                                            agent::local_endpoint::debug,
                                            options_.instance);
        if (!listener)
            return listener.error();
        listener_ = std::move(listener).value();

        if (!options_.audit_directory.empty())
        {
            auto opened = control::audit_log::open(
                {.directory = options_.audit_directory});
            if (!opened)
            {
                listener_->close();
                return opened.error();
            }
            audit_ = std::move(opened).value();
        }
        publish_instance();
        audit("debug.server.started", {{"instance", options_.instance}});

        accept_next();
        schedule_poll();
        thread_ = std::thread([this] { io_.run(); });
        return {};
    }

    void stop();

    // -- the clients -------------------------------------------------------

    std::vector<client_view> clients() const;

    void disconnect_all();

    void set_traffic(bool on)
    {
        {
            const std::lock_guard lock(mutex_);
            traffic_until_ =
                on ? std::chrono::steady_clock::now() + options_.traffic_window
                   : std::chrono::steady_clock::time_point{};
            traffic_on_ = on;
        }
        audit(on ? "debug.traffic.on" : "debug.traffic.off", {});
        changed();
    }

    bool traffic_on()
    {
        bool expired = false;
        bool on = false;
        {
            const std::lock_guard lock(mutex_);
            if (traffic_on_ &&
                std::chrono::steady_clock::now() >= traffic_until_)
            {
                traffic_on_ = false;
                expired = true;
            }
            on = traffic_on_;
        }
        if (expired)
        {
            audit("debug.traffic.off", {{"reason", "timed_out"}});
            changed();
        }
        return on;
    }

    // -- helpers the connections use ---------------------------------------

    const server_options& options() const noexcept { return options_; }
    boost::asio::io_context& io() noexcept { return io_; }
    boost::asio::thread_pool& pool() noexcept { return pool_; }
    bool stopping() const noexcept { return stopped_.load(); }
    const std::string& instance() const { return options_.instance; }
    std::string address() const
    {
        auto where = agent::local_address(agent::local_endpoint::debug,
                                          options_.instance);
        return where ? *where : std::string{};
    }

    void audit(std::string_view event,
               const std::vector<control::audit_field>& fields)
    {
        if (audit_)
            audit_->record(event, fields);
    }

    void changed()
    {
        if (options_.on_change)
            options_.on_change();
    }

    std::optional<scope_set> remembered(const std::string& program)
    {
        if (program.empty())
            return std::nullopt;
        const std::lock_guard lock(mutex_);
        const auto found = remembered_.find(program);
        if (found == remembered_.end())
            return std::nullopt;
        return found->second;
    }

    void remember(const std::string& program, scope_set granted)
    {
        if (program.empty())
            return;
        const std::lock_guard lock(mutex_);
        remembered_[program] |= granted;
    }

    std::uint64_t next_id() { return ++last_id_; }

    void add(const std::shared_ptr<connection>& item, std::uint64_t id)
    {
        const std::lock_guard lock(mutex_);
        connections_[id] = item;
    }

    void remove(std::uint64_t id)
    {
        {
            const std::lock_guard lock(mutex_);
            connections_.erase(id);
            views_.erase(id);
        }
        changed();
    }

    /// What `clients()` says of a connection that was let in.  Kept apart from
    /// the connection's own fields, which the io thread owns.
    void publish_view(const client_view& view)
    {
        const std::lock_guard lock(mutex_);
        if (connections_.contains(view.id))
            views_[view.id] = view;
    }

    std::size_t count() const
    {
        const std::lock_guard lock(mutex_);
        return connections_.size();
    }

    op_result about();
    op_result audit_verify();

private:
    void accept_next();
    void schedule_poll();
    void poll_traces();
    void publish_instance();
    void unpublish_instance();

    server_options options_;
    boost::asio::io_context io_;
    boost::asio::thread_pool pool_;
    boost::asio::steady_timer poll_;
    std::thread thread_;
    std::unique_ptr<agent::local_listener> listener_;
    std::unique_ptr<control::audit_log> audit_;
    std::atomic<bool> stopped_{false};
    std::atomic<std::uint64_t> last_id_{0};
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::shared_ptr<connection>> connections_;
    std::map<std::uint64_t, client_view> views_;
    std::map<std::string, scope_set> remembered_;
    bool traffic_on_ = false;
    std::chrono::steady_clock::time_point traffic_until_{};
};

class server::impl::connection : public std::enable_shared_from_this<connection>
{
public:
    connection(std::shared_ptr<impl> owner,
               std::unique_ptr<agent::local_stream> stream, std::uint64_t id)
        : owner_(std::move(owner)), stream_(std::move(stream)), id_(id),
          reader_(hello_frame_limit), deadline_(owner_->io()),
          since_(std::chrono::system_clock::now())
    {
    }

    void start()
    {
        auto self = shared_from_this();
        const auto who = stream_->peer();
        if (!who || !who->same_user)
        {
            // Not this user: closed with no reply.  The operating system has
            // already refused anyone else; this is the second line.
            close("not_same_user");
            return;
        }
        program_ = who->program;
        process_id_ = who->process_id;
        owner_->add(self, id_);
        deadline_.expires_after(owner_->options().hello_deadline);
        deadline_.async_wait(
            [self](const boost::system::error_code& ec)
            {
                if (!ec && !self->hello_done_)
                    self->close("no_hello");
            });
        read_more();
    }

    void close(std::string_view reason)
    {
        if (closed_)
            return;
        closed_ = true;
        deadline_.cancel();
        if (stream_)
            stream_->close();
        if (opened_)
            owner_->audit("debug.session.closed",
                          {{"client", name_}, {"id", id_}, {"reason", reason}});
        owner_->remove(id_);
    }

    client_view view() const
    {
        client_view out;
        out.id = id_;
        out.name = name_;
        out.program = program_;
        out.process_id = process_id_;
        out.granted = granted_;
        out.watching_trace = watching;
        out.since = since_;
        return out;
    }

    /// Called on the io thread by the poller.
    void send_trace(trace::recorder& recorder)
    {
        if (!watching || closed_)
            return;
        trace::filter what;
        what.after_seq = cursor;
        what.endpoint = watch_endpoint;
        what.failures_only = watch_failures;
        what.limit = trace_event_limit;
        const auto found = recorder.read(what);
        if (found.empty())
            return;
        cursor = found.back().seq;

        json::array entries;
        for (const auto& item : found)
            entries.push_back(entry_object(item));
        json::object event;
        event["event"] = "trace";
        event["entries"] = std::move(entries);
        json::value wrapped = std::move(event);
        redact_value(wrapped, owner_->options().redact);
        send(json::serialize(wrapped), {});
    }

private:
    void read_more()
    {
        if (closed_)
            return;
        auto self = shared_from_this();
        stream_->async_read_some(
            boost::asio::buffer(chunk_),
            [self](const boost::system::error_code& ec, std::size_t size)
            {
                if (ec)
                {
                    self->close("closed_by_client");
                    return;
                }
                self->reader_.feed(bytes_view(self->chunk_.data(), size));
                self->take_frames();
                self->read_more();
            });
    }

    void take_frames()
    {
        while (!closed_)
        {
            auto next = reader_.next();
            if (!next)
            {
                close("bad_frame");
                return;
            }
            const auto& request = *next;
            if (!request)
                return;
            handle(*request);
        }
    }

    void handle(const agent::frame& request)
    {
        json::value parsed;
        {
            boost::system::error_code ec;
            json::parse_options parse;
            parse.max_depth = 8;
            parsed = json::parse(request.header, ec, {}, parse);
            if (ec || !parsed.is_object())
            {
                close("bad_message");
                return;
            }
        }
        const json::object& object = parsed.as_object();
        const std::int64_t id = number_of(object, "id");
        const std::string op = text_of(object, "op");

        if (!hello_done_)
        {
            if (op != "hello")
            {
                close("hello_required");
                return;
            }
            handle_hello(object, id);
            return;
        }
        if (op == "ping")
        {
            reply(id, op_result::done());
            return;
        }
        if (op == "hello")
        {
            reply(id,
                  op_result::fail("invalid_argument", "Already said hello."));
            return;
        }
        if (!within_rate())
        {
            reply(id, op_result::fail("limit_exceeded",
                                      "Too many requests this second."));
            return;
        }
        const auto needed = scope_of_operation(op);
        if (!needed)
        {
            reply(id, op_result::fail("unsupported",
                                      "There is no such operation."));
            return;
        }
        if (!has(granted_, *needed))
        {
            audit_request(op, {}, false, "forbidden");
            reply(id,
                  op_result::fail("forbidden",
                                  "This client was not given " +
                                      std::string(scope_name(*needed)) + "."));
            return;
        }
        std::string args = "{}";
        if (const auto* given = object.if_contains("args"))
            args = json::serialize(*given);
        if (op == "debug.trace.subscribe" || op == "debug.trace.unsubscribe")
        {
            reply(id, change_subscription(op, object));
            return;
        }
        dispatch(id, op, std::move(args));
    }

    void handle_hello(const json::object& object, std::int64_t id)
    {
        if (number_of(object, "version") !=
            static_cast<std::int64_t>(protocol_version))
        {
            reply(id, op_result::fail("unsupported",
                                      "This studio speaks another version "
                                      "of the debug protocol."));
            close("version_mismatch");
            return;
        }
        if (owner_->count() > owner_->options().max_clients)
        {
            reply(id, op_result::fail("busy", "Too many debug clients."));
            close("busy");
            return;
        }
        name_ = clean_name(text_of(object, "client"));
        if (name_.empty())
            name_ = "client";
        scope_set wanted = 0;
        if (const auto* list = object.if_contains("scopes");
            list != nullptr && list->is_array())
        {
            for (const auto& item : list->as_array())
            {
                if (!item.is_string())
                    continue;
                if (const auto known = scope_from_name(item.as_string()))
                    wanted |= bit(*known);
            }
        }
        if (wanted == 0)
            wanted = bit(scope::read);
        hello_done_ = true;
        reader_.set_limit(request_frame_limit);
        deadline_.expires_after(owner_->options().approval_timeout);
        auto self = shared_from_this();
        deadline_.async_wait(
            [self](const boost::system::error_code& ec)
            {
                if (!ec && !self->opened_)
                    self->decide(0, 0, false, "approval_timed_out");
            });

        // A program the person has already let in, for these scopes, is not
        // asked again.
        if (const auto known = owner_->remembered(program_);
            known && (wanted & ~*known) == 0)
        {
            hello_id_ = id;
            decide(id, wanted, false, "remembered");
            return;
        }
        hello_id_ = id;
        if (!owner_->options().approve)
        {
            decide(id, 0, false, "no_approver");
            return;
        }
        client_request ask;
        ask.name = name_;
        ask.program = program_;
        ask.process_id = process_id_;
        ask.wanted = wanted;
        std::weak_ptr<connection> weak = self;
        auto owner = owner_;
        owner_->options().approve(
            ask,
            [weak, owner, wanted](approval answer)
            {
                boost::asio::post(owner->io(),
                                  [weak, wanted, answer]
                                  {
                                      if (const auto item = weak.lock())
                                          item->decide(item->hello_id_,
                                                       answer.granted & wanted,
                                                       answer.remember,
                                                       "person");
                                  });
            });
    }

    void decide(std::int64_t id, scope_set granted, bool remember,
                std::string_view how)
    {
        if (closed_ || opened_)
            return;
        if (granted == 0)
        {
            owner_->audit(
                "debug.session.denied",
                {{"client", name_},
                 {"program", cut(owner_->options().redact.apply(program_),
                                 audit_text_limit)},
                 {"process", process_id_},
                 {"how", how}});
            reply(hello_id_,
                  op_result::fail("forbidden",
                                  "The person at this computer did not "
                                  "allow this client."));
            close("denied");
            return;
        }
        (void)id;
        granted_ = granted;
        opened_ = true;
        owner_->publish_view(view());
        deadline_.cancel();
        if (remember)
            owner_->remember(program_, granted);
        owner_->audit("debug.session.opened",
                      {{"client", name_},
                       {"program", cut(owner_->options().redact.apply(program_),
                                       audit_text_limit)},
                       {"process", process_id_},
                       {"id", id_},
                       {"scopes", scope_text(granted)},
                       {"how", how}});

        json::object result;
        json::array scopes;
        for (const scope item :
             {scope::read, scope::act, scope::gui, scope::data})
        {
            if (has(granted, item))
                scopes.push_back(json::string(scope_name(item)));
        }
        result["granted"] = std::move(scopes);
        result["version"] = protocol_version;
        result["process"] = current_process_id();
        reply(hello_id_, op_result::done(json::serialize(result)));
        owner_->changed();
    }

    void dispatch(std::int64_t id, const std::string& op, std::string args)
    {
        auto self = shared_from_this();
        auto owner = owner_;
        boost::asio::post(
            owner_->pool(),
            [self, owner, id, op, args = std::move(args)]
            {
                op_result done = self->execute(op, args);
                boost::asio::post(
                    owner->io(),
                    [self, id, op, args, done = std::move(done)]() mutable
                    {
                        self->audit_request(op, args, done.ok, done.code);
                        self->reply(id, std::move(done));
                    });
            });
    }

    /// Runs on a worker thread.
    op_result execute(const std::string& op, const std::string& args)
    {
        const auto& source = owner_->options().source;
        if (op == "debug.status")
            return owner_->about();
        if (op == "debug.audit.verify")
            return owner_->audit_verify();
        if (op == "debug.trace")
            return read_trace(args);
        if (op == "debug.snapshot")
            return source->snapshot();
        if (op == "debug.com_numbers")
            return source->com_numbers();
        if (op == "debug.driver")
            return source->driver();
        if (op == "debug.plugins")
            return source->plugins();
        if (op == "debug.run")
            return source->run(args);
        if (op == "debug.gui.state")
            return source->gui_state();
        if (op == "debug.gui.screenshot")
            return source->screenshot(args);
        if (op == "debug.traffic")
        {
            if (!owner_->traffic_on())
                return op_result::fail(
                    "forbidden",
                    "The person at this computer has not turned on the "
                    "traffic switch.");
            return source->traffic(args);
        }
        return op_result::fail("unsupported", "There is no such operation.");
    }

    /// Starts or stops the live trace on this connection.  Runs on the io
    /// thread, which is where the poller sends from.
    op_result change_subscription(const std::string& op,
                                  const json::object& request)
    {
        auto& recorder = *owner_->options().recorder;
        if (op == "debug.trace.unsubscribe")
        {
            watching = false;
            owner_->publish_view(view());
            owner_->changed();
            return op_result::done();
        }
        watching = true;
        watch_endpoint = 0;
        watch_failures = false;
        std::int64_t backlog = 0;
        if (const auto* given = request.if_contains("args");
            given != nullptr && given->is_object())
        {
            const auto& object = given->as_object();
            watch_endpoint = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, number_of(object, "endpoint")));
            if (const auto* flag = object.if_contains("failures_only");
                flag != nullptr && flag->is_bool())
                watch_failures = flag->as_bool();
            backlog =
                std::clamp<std::int64_t>(number_of(object, "backlog"), 0, 200);
        }
        const std::uint64_t last = recorder.last_seq();
        cursor = last;
        if (backlog > 0)
        {
            trace::filter what;
            what.endpoint = watch_endpoint;
            what.failures_only = watch_failures;
            what.limit = static_cast<std::size_t>(backlog);
            const auto recent = recorder.read(what);
            if (!recent.empty())
                cursor = recent.front().seq - 1;
        }
        owner_->publish_view(view());
        owner_->changed();
        json::object out;
        out["last_seq"] = last;
        return op_result::done(json::serialize(out));
    }

    op_result read_trace(const std::string& args)
    {
        trace::filter what;
        what.limit = trace_read_default;
        boost::system::error_code ec;
        const json::value given = json::parse(args, ec);
        if (!ec && given.is_object())
        {
            const auto& object = given.as_object();
            what.after_seq = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, number_of(object, "after_seq")));
            what.endpoint = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, number_of(object, "endpoint")));
            if (const auto* flag = object.if_contains("failures_only");
                flag != nullptr && flag->is_bool())
                what.failures_only = flag->as_bool();
            if (const auto limit = number_of(object, "limit"); limit > 0)
                what.limit = std::min<std::size_t>(
                    static_cast<std::size_t>(limit), trace_read_most);
        }
        auto& recorder = *owner_->options().recorder;
        json::array entries;
        for (const auto& item : recorder.read(what))
            entries.push_back(entry_object(item));
        json::object out;
        out["entries"] = std::move(entries);
        out["last_seq"] = recorder.last_seq();
        out["overwritten"] = recorder.overwritten();
        return op_result::done(json::serialize(out));
    }

    void reply(std::int64_t id, op_result done)
    {
        if (closed_)
            return;
        json::object header;
        header["id"] = id;
        header["ok"] = done.ok;
        if (done.ok)
        {
            boost::system::error_code ec;
            json::value body = json::parse(done.json, ec);
            if (ec)
                body = json::object{};
            redact_value(body, owner_->options().redact);
            header["result"] = std::move(body);
        }
        else
        {
            json::object error;
            error["code"] = done.code;
            error["message"] = owner_->options().redact.apply(done.message);
            header["error"] = std::move(error);
        }
        if (!done.blob.empty())
            header["blob"] = done.blob.size();
        send(json::serialize(header), std::move(done.blob));
    }

    void send(std::string header, byte_buffer blob)
    {
        if (closed_)
            return;
        auto bytes = agent::encode_frame({std::move(header), std::move(blob)});
        if (!bytes)
        {
            // Too large for one frame: say so instead of staying silent.
            json::object error;
            error["ok"] = false;
            error["error"] = json::object{
                {"code", "limit_exceeded"},
                {"message", "The reply does not fit in one frame."}};
            bytes = agent::encode_frame({json::serialize(error), {}});
            if (!bytes)
                return;
        }
        queued_bytes_ += bytes->size();
        writes_.push_back(std::move(*bytes));
        if (queued_bytes_ > write_backlog_limit)
        {
            close("client_not_reading");
            return;
        }
        if (!writing_)
            write_next();
    }

    void write_next()
    {
        if (closed_ || writes_.empty())
        {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto self = shared_from_this();
        const auto& front = writes_.front();
        stream_->async_write_some(
            boost::asio::buffer(front.data() + written_,
                                front.size() - written_),
            [self](const boost::system::error_code& ec, std::size_t size)
            {
                if (ec)
                {
                    self->close("write_failed");
                    return;
                }
                self->written_ += size;
                if (self->written_ == self->writes_.front().size())
                {
                    self->queued_bytes_ -= self->writes_.front().size();
                    self->writes_.pop_front();
                    self->written_ = 0;
                }
                self->write_next();
            });
    }

    bool within_rate()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start_ >= std::chrono::seconds(1))
        {
            window_start_ = now;
            window_count_ = 0;
        }
        return ++window_count_ <= owner_->options().max_requests_per_second;
    }

    void audit_request(const std::string& op, const std::string& args, bool ok,
                       const std::string& code)
    {
        std::vector<control::audit_field> fields{
            {"client", name_}, {"id", id_}, {"op", op}, {"ok", ok}};
        if (!code.empty())
            fields.emplace_back("code", code);
        if (!args.empty() && args != "{}")
            fields.emplace_back(
                "args",
                cut(owner_->options().redact.apply(args), audit_text_limit));
        owner_->audit("debug.request", fields);
    }

    static std::string scope_text(scope_set set)
    {
        std::string out;
        for (const scope item :
             {scope::read, scope::act, scope::gui, scope::data})
        {
            if (!has(set, item))
                continue;
            if (!out.empty())
                out += ' ';
            out += scope_name(item);
        }
        return out;
    }

    static std::int64_t number_of(const json::object& object,
                                  std::string_view key)
    {
        if (const auto* found = object.if_contains(key))
        {
            if (found->is_int64())
                return found->as_int64();
            if (found->is_uint64())
                return static_cast<std::int64_t>(found->as_uint64());
        }
        return 0;
    }

    static std::string text_of(const json::object& object, std::string_view key)
    {
        if (const auto* found = object.if_contains(key);
            found != nullptr && found->is_string())
            return std::string(found->as_string());
        return {};
    }

    friend class server::impl;

    std::shared_ptr<impl> owner_;
    std::unique_ptr<agent::local_stream> stream_;
    std::uint64_t id_;
    agent::frame_reader reader_;
    boost::asio::steady_timer deadline_;
    std::chrono::system_clock::time_point since_;
    std::array<byte, std::size_t{16} * 1024> chunk_{};
    std::deque<byte_buffer> writes_;
    std::size_t written_ = 0;
    std::size_t queued_bytes_ = 0;
    bool writing_ = false;
    bool closed_ = false;
    bool hello_done_ = false;
    bool opened_ = false;
    std::int64_t hello_id_ = 0;
    scope_set granted_ = 0;
    std::string name_;
    std::string program_;
    std::uint64_t process_id_ = 0;
    std::chrono::steady_clock::time_point window_start_{};
    std::size_t window_count_ = 0;

public:
    bool watching = false;
    std::uint64_t cursor = 0;
    std::uint64_t watch_endpoint = 0;
    bool watch_failures = false;
};

void server::impl::stop()
{
    bool expected = false;
    if (!stopped_.compare_exchange_strong(expected, true))
        return;
    boost::asio::post(io_,
                      [this]
                      {
                          if (listener_)
                              listener_->close();
                          poll_.cancel();
                          // The connections are read here, on the io thread,
                          // so one that was accepted just before is closed
                          // too.
                          std::vector<std::shared_ptr<connection>> open;
                          {
                              const std::lock_guard lock(mutex_);
                              for (auto& item : connections_)
                                  open.push_back(item.second);
                          }
                          for (const auto& item : open)
                              item->close("server_stopped");
                      });
    // Let the posted closes run, then stop the loop.
    boost::asio::post(io_, [this] { io_.stop(); });
    if (thread_.joinable())
        thread_.join();
    pool_.join();
    // A connection holds the server, and the loop holds the handlers that
    // hold the connection.  Stopping the loop left those handlers queued, so
    // run them now: the closed sockets and cancelled timers complete, and each
    // handler lets its connection go.  Without this the loop, the connections
    // and the server keep each other alive for ever.
    io_.restart();
    io_.poll();
    {
        const std::lock_guard lock(mutex_);
        connections_.clear();
    }
    listener_.reset();
    audit("debug.server.stopped", {});
    // Let go of the log's file, so a server that starts next can open it.
    audit_.reset();
    unpublish_instance();
}

void server::impl::disconnect_all()
{
    std::vector<std::shared_ptr<connection>> open;
    {
        const std::lock_guard lock(mutex_);
        for (auto& item : connections_)
            open.push_back(item.second);
        remembered_.clear();
    }
    boost::asio::post(io_,
                      [open]
                      {
                          for (const auto& item : open)
                              item->close("disconnected_by_user");
                      });
}

std::vector<client_view> server::impl::clients() const
{
    std::vector<client_view> out;
    const std::lock_guard lock(mutex_);
    out.reserve(views_.size());
    for (const auto& item : views_)
        out.push_back(item.second);
    return out;
}

void server::impl::accept_next()
{
    if (stopped_.load() || !listener_)
        return;
    listener_->async_accept(
        [this](const boost::system::error_code& ec,
               std::unique_ptr<agent::local_stream> stream)
        {
            if (ec)
            {
                if (ec != boost::asio::error::operation_aborted &&
                    !stopped_.load())
                    accept_next();
                return;
            }
            auto item = std::make_shared<connection>(
                shared_from_this(), std::move(stream), next_id());
            item->start();
            accept_next();
        });
}

void server::impl::schedule_poll()
{
    if (stopped_.load())
        return;
    poll_.expires_after(trace_poll);
    poll_.async_wait(
        [this](const boost::system::error_code& ec)
        {
            if (ec || stopped_.load())
                return;
            poll_traces();
            traffic_on();
            schedule_poll();
        });
}

void server::impl::poll_traces()
{
    std::vector<std::shared_ptr<connection>> open;
    {
        const std::lock_guard lock(mutex_);
        for (auto& item : connections_)
        {
            if (item.second->watching)
                open.push_back(item.second);
        }
    }
    for (const auto& item : open)
        item->send_trace(*options_.recorder);
}

op_result server::impl::about()
{
    boost::system::error_code ec;
    json::value about = json::parse(options_.about_json, ec);
    if (ec || !about.is_object())
        about = json::object{};
    json::object out = about.as_object();
    out["protocol"] = protocol_version;
    out["process"] = current_process_id();
    out["instance"] = options_.instance;
    out["clients"] = count();
    out["traffic_on"] = traffic_on();
    out["recorder_last_seq"] = options_.recorder->last_seq();
    out["audit"] = audit_ != nullptr;
    return op_result::done(json::serialize(out));
}

op_result server::impl::audit_verify()
{
    if (!audit_)
        return op_result::fail("unsupported", "There is no audit log.");
    const auto verdict = audit_->verify();
    if (!verdict)
        return op_result::fail("internal", verdict.error().message());
    json::object out;
    out["intact"] = verdict->intact;
    out["records"] = verdict->records;
    if (!verdict->intact)
    {
        out["line"] = verdict->line;
        out["problem"] = verdict->problem;
    }
    return op_result::done(json::serialize(out));
}

void server::impl::publish_instance()
{
    if (options_.instance_directory.empty())
        return;
    std::error_code ignored;
    std::filesystem::create_directories(options_.instance_directory, ignored);
    const auto file =
        options_.instance_directory / (options_.instance + ".json");
    std::filesystem::remove(file, ignored);
    json::object body;
    body["instance"] = options_.instance;
    body["process"] = current_process_id();
    body["protocol"] = protocol_version;
    body["address"] = address();
    (void)control::create_private_file(file, json::serialize(body));
}

void server::impl::unpublish_instance()
{
    if (options_.instance_directory.empty())
        return;
    std::error_code ignored;
    std::filesystem::remove(
        options_.instance_directory / (options_.instance + ".json"), ignored);
}

result<std::unique_ptr<server>> server::start(server_options options)
{
    if (!options.source)
        return make_error_code(errc::invalid_argument);
    if (!agent::valid_instance_name(options.instance))
        return make_error_code(errc::invalid_argument);
    auto state = std::make_shared<impl>(std::move(options));
    if (const auto begun = state->begin(); !begun)
        return begun.error();
    return std::unique_ptr<server>(new server(std::move(state)));
}

server::server(std::shared_ptr<impl> state) : impl_(std::move(state)) {}

server::~server()
{
    if (!impl_)
        return;
    try
    {
        impl_->stop();
    }
    catch (...)
    {
        // best effort: nothing can be done about a failure to stop while
        // the object is being destroyed.
    }
}

void server::stop()
{
    impl_->stop();
}

const std::string& server::instance() const noexcept
{
    return impl_->instance();
}

std::string server::address() const
{
    return impl_->address();
}

std::vector<client_view> server::clients() const
{
    return impl_->clients();
}

void server::disconnect_all()
{
    impl_->disconnect_all();
}

void server::set_traffic(bool on)
{
    impl_->set_traffic(on);
}

bool server::traffic_on() const
{
    return impl_->traffic_on();
}

} // namespace vss::debug
