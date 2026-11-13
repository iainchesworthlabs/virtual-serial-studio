//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The agent's server.  Three kinds of thread meet here and each has one job:
//
//   the hub's threads    run the engine.  The only code of this file they run
//                        is `event_queue::push`, on the dispatcher strand: a
//                        copy into a bounded queue under a short lock.
//   the client thread    runs `io_`: every read from and write to a client,
//                        every timer and all the state of a connection.
//   the request pool     runs the requests that call the studio, which may
//                        take seconds (a driver port), one client's requests
//                        in order and different clients side by side.
//
// A function below says in its comment which of them calls it.
//

#include <vss/agent/server.hpp>

#include "platform/process_platform.hpp"

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/agent/proto/message.hpp>
#include <vss/ports/virtual_port.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace vss::agent
{
namespace
{

constexpr std::size_t read_buffer_size = std::size_t{64} * 1024;
/// Frames of one client handled before the others get a turn.
constexpr int frames_per_turn = 64;
constexpr std::chrono::milliseconds accept_retry_delay{100};
constexpr unsigned request_threads = 2;
/// The one kind of port this agent makes so far.
constexpr const char* loopback_kind = "loopback";

/// The events one subscriber has not been sent yet.
///
/// The hub's sink holds it through a `shared_ptr`, so a delivery that is
/// already running when the client goes away writes into a queue nobody
/// reads, never into freed memory.
class event_queue
{
public:
    event_queue(bool want_data, std::size_t max_events, std::size_t max_bytes)
        : want_data_(want_data), max_events_(max_events), max_bytes_(max_bytes)
    {
    }

    /// The hub's dispatcher strand.  Never waits for a client.
    void push(const endpoint_event& event)
    {
        if (!accepting_.load())
            return;
        event_record record;
        record.kind = event.kind;
        record.endpoint = event.endpoint;
        record.name = event.name;
        record.direction = event.direction;
        record.lines = event.lines.bits;
        record.message = event.message;
        if (event.error)
            record.error = to_wire(event.error);
        const auto since = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               event.when.time_since_epoch())
                               .count();
        record.when_ns = since > 0 ? static_cast<std::uint64_t>(since) : 0;
        if (event.data)
        {
            record.size = event.data->size();
            if (want_data_ && event.data->size() <= max_bytes_)
                record.data = *event.data;
        }

        const std::lock_guard lock(mutex_);
        while (!events_.empty() && (events_.size() >= max_events_ ||
                                    bytes_ + record.data.size() > max_bytes_))
        {
            bytes_ -= events_.front().data.size();
            events_.pop_front();
            ++dropped_;
        }
        bytes_ += record.data.size();
        events_.push_back(std::move(record));
    }

    /// The client thread.  Empties the queue into `batch`.
    void take(event_batch& batch)
    {
        const std::lock_guard lock(mutex_);
        batch.events.assign(std::make_move_iterator(events_.begin()),
                            std::make_move_iterator(events_.end()));
        batch.dropped = dropped_;
        events_.clear();
        bytes_ = 0;
        dropped_ = 0;
    }

    void stop() noexcept { accepting_.store(false); }

private:
    const bool want_data_;
    const std::size_t max_events_;
    const std::size_t max_bytes_;
    std::atomic<bool> accepting_{true};
    std::mutex mutex_;
    std::deque<event_record> events_;
    std::size_t bytes_ = 0;
    std::uint64_t dropped_ = 0;
};

/// Bytes waiting for a client to read them.
struct outgoing
{
    byte_buffer bytes;
    /// Runs on the client thread once the bytes are written.
    std::function<void()> after;
    bool batch = false;
};

/// One client.  Everything here belongs to the client thread, except `work`,
/// which is where the request pool runs this client's requests.
struct session
{
    session(std::unique_ptr<local_stream> connection,
            boost::asio::io_context& io, boost::asio::thread_pool& pool)
        : stream(std::move(connection)), work(pool.get_executor()),
          handshake_timer(io), flush_timer(io), read_buffer(read_buffer_size)
    {
    }

    std::unique_ptr<local_stream> stream;
    boost::asio::strand<boost::asio::thread_pool::executor_type> work;
    boost::asio::steady_timer handshake_timer;
    boost::asio::steady_timer flush_timer;
    frame_reader reader;
    byte_buffer read_buffer;

    std::deque<outgoing> unsent;
    std::size_t unsent_bytes = 0;
    std::size_t write_offset = 0;
    bool writing = false;

    bool reading = false;
    bool closed = false;
    bool greeted = false;
    /// Said in `hello`: this connection may list and subscribe only.
    bool read_only = false;
    /// An error was sent: nothing more is read, and the connection closes
    /// when the error has been written.
    bool leaving = false;
    /// Requests handed to the pool whose replies are not queued yet.
    std::size_t pending = 0;

    std::shared_ptr<event_queue> queue;
    subscriber_id subscription = 0;
    std::chrono::milliseconds interval{default_event_interval_ms};
    bool batch_unsent = false;
    std::uint64_t sent_revision = 0;
};

using session_ptr = std::shared_ptr<session>;

reply error_reply_for(std::uint64_t id, const error_code& error)
{
    reply out;
    out.id = id;
    out.body = error_reply{to_wire(error)};
    return out;
}

port_summary summarise(const endpoint_info& info, std::string advertised)
{
    port_summary out;
    out.id = info.id;
    out.name = info.name;
    if (advertised.empty())
        out.path = info.path;
    else
        out.path = std::move(advertised);
    out.kind = info.kind;
    out.state = info.state;
    out.open = info.open;
    out.bytes_rx = info.stats.bytes_rx;
    out.bytes_tx = info.stats.bytes_tx;
    return out;
}

link_summary summarise(const link_info& info)
{
    link_summary out;
    out.id = info.id;
    out.mode = info.mode;
    out.sources = info.sources;
    out.targets = info.targets;
    out.bytes = info.stats.bytes;
    return out;
}

} // namespace

std::string agent_version()
{
    return VSS_AGENT_VERSION;
}

class server::impl
{
public:
    impl(studio& app, server_options options)
        : studio_(app), options_(std::move(options)), accept_retry_(io_)
    {
    }

    ~impl()
    {
        try
        {
            stop();
        }
        catch (...)
        {
            // A stop that failed part of the way leaves threads that still
            // use this object, so there is nothing to go on with.
            std::terminate();
        }
    }

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    // -- any thread ----------------------------------------------------------

    result<void> start()
    {
        const std::lock_guard lock(state_mutex_);
        if (started_)
            return make_error_code(errc::already_open);
        auto listening = listen_local(io_.get_executor(), options_.instance);
        if (!listening)
            return listening.error();
        listener_ = std::move(*listening);
        address_ = listener_->address();
        pool_ = std::make_unique<boost::asio::thread_pool>(request_threads);
        work_.emplace(io_.get_executor());
        accept_next();
        io_thread_ = std::thread([this] { run_client_thread(); });
        started_ = true;
        return {};
    }

    void stop()
    {
        {
            const std::lock_guard lock(state_mutex_);
            if (!started_ || stopped_)
                return;
            stopped_ = true;
        }
        // Close the door and every connection on the thread that owns them.
        std::promise<void> closed;
        boost::asio::post(io_,
                          [this, &closed]
                          {
                              stopping_ = true;
                              accept_retry_.cancel();
                              if (listener_)
                                  listener_->close();
                              const std::vector<session_ptr> all(
                                  sessions_.begin(), sessions_.end());
                              for (const session_ptr& client : all)
                                  close(client);
                              closed.set_value();
                          });
        closed.get_future().wait();
        // Requests still running finish; what they post back finds its
        // session closed.  Only then may the client thread run out of work.
        pool_->join();
        work_.reset();
        if (io_thread_.joinable())
            io_thread_.join();
        listener_.reset();
        request_stop();
    }

    std::string address() const
    {
        const std::lock_guard lock(state_mutex_);
        return address_;
    }

    void request_stop()
    {
        {
            const std::lock_guard lock(state_mutex_);
            stop_requested_ = true;
        }
        stop_signal_.notify_all();
    }

    void wait_for_stop_request()
    {
        std::unique_lock lock(state_mutex_);
        stop_signal_.wait(lock, [this] { return stop_requested_; });
    }

    bool wait_for_stop_request(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(state_mutex_);
        return stop_signal_.wait_for(lock, limit,
                                     [this] { return stop_requested_; });
    }

    server_counters counters() const
    {
        server_counters out;
        out.accepted = accepted_.load();
        out.refused_busy = refused_busy_.load();
        out.protocol_errors = protocol_errors_.load();
        out.handshake_timeouts = handshake_timeouts_.load();
        out.slow_clients_closed = slow_clients_closed_.load();
        out.dropped_events = dropped_events_.load();
        out.read_only_refused = read_only_refused_.load();
        out.clients = clients_.load();
        out.read_only_clients = read_only_clients_.load();
        return out;
    }

private:
    // -- the client thread ---------------------------------------------------

    void run_client_thread() noexcept
    {
        for (;;)
        {
            try
            {
                io_.run();
                return;
            }
            catch (...)
            {
                // Out of memory in a handler.  Ask the program to stop, which
                // closes the ports in order, and keep the thread running so
                // that `stop` can still reach it.
                request_stop();
            }
        }
    }

    void accept_next()
    {
        listener_->async_accept([this](boost::system::error_code failed,
                                       std::unique_ptr<local_stream> stream)
                                { on_accept(failed, std::move(stream)); });
    }

    void on_accept(const boost::system::error_code& failed,
                   std::unique_ptr<local_stream> stream)
    {
        if (stopping_ || failed == boost::asio::error::operation_aborted)
            return;
        if (failed || !stream)
        {
            // A client that came and went, or the system out of handles for
            // a moment.  Try again, but not in a tight loop.
            accept_retry_.expires_after(accept_retry_delay);
            accept_retry_.async_wait(
                [this](boost::system::error_code cancelled)
                {
                    if (!cancelled && !stopping_)
                        accept_next();
                });
            return;
        }

        accepted_.fetch_add(1);
        auto client = std::make_shared<session>(std::move(stream), io_, *pool_);
        const bool full = sessions_.size() >= options_.max_clients;
        sessions_.insert(client);
        clients_.store(sessions_.size());
        // Bounds the life of a connection that never speaks, and of one that
        // was refused and does not read why.
        client->handshake_timer.expires_after(options_.handshake_timeout);
        client->handshake_timer.async_wait(
            [this, client](boost::system::error_code cancelled)
            {
                if (cancelled || client->closed || client->greeted)
                    return;
                handshake_timeouts_.fetch_add(1);
                close(client);
            });
        if (full)
        {
            refused_busy_.fetch_add(1);
            fail(client, 0, make_error_code(protocol_errc::busy));
        }
        else
        {
            pump(client);
        }
        accept_next();
    }

    /// Handles the frames that have arrived, then reads more.  It stops while
    /// the client has `max_pending_requests` waiting, which is what slows a
    /// client that sends faster than its requests can run.
    void pump(const session_ptr& client)
    {
        int budget = frames_per_turn;
        while (!client->closed && !client->leaving &&
               client->pending < options_.max_pending_requests)
        {
            if (budget-- == 0)
            {
                // Let the other clients have the thread.
                boost::asio::post(io_, [this, client] { pump(client); });
                return;
            }
            auto next = client->reader.next();
            if (!next)
            {
                protocol_errors_.fetch_add(1);
                fail(client, 0, next.error());
                return;
            }
            const std::optional<frame> ready = std::move(*next);
            if (!ready)
                break;
            handle_frame(client, *ready);
        }
        if (!client->closed && !client->leaving && !client->reading &&
            client->pending < options_.max_pending_requests)
            start_read(client);
    }

    void start_read(const session_ptr& client)
    {
        client->reading = true;
        client->stream->async_read_some(
            boost::asio::buffer(client->read_buffer),
            [this, client](boost::system::error_code failed, std::size_t size)
            {
                client->reading = false;
                if (client->closed)
                    return;
                if (failed)
                {
                    close(client);
                    return;
                }
                client->reader.feed(
                    bytes_view(client->read_buffer.data(), size));
                pump(client);
            });
    }

    void handle_frame(const session_ptr& client, const frame& received)
    {
        auto decoded = decode_request(received);
        if (!decoded)
        {
            protocol_errors_.fetch_add(1);
            const std::uint64_t id = peek_request_id(received);
            // A well-formed request for something this agent does not have
            // costs the client an error, not its connection.
            if (client->greeted &&
                decoded.error() == protocol_errc::unknown_operation)
                send_reply(client, error_reply_for(id, decoded.error()));
            else
                fail(client, id, decoded.error());
            return;
        }

        request& asked = *decoded;
        if (const auto* hello = std::get_if<hello_request>(&asked.body))
        {
            greet(client, asked.id, *hello);
            return;
        }
        if (!client->greeted)
        {
            protocol_errors_.fetch_add(1);
            fail(client, asked.id,
                 make_error_code(protocol_errc::handshake_required));
            return;
        }
        // A connection that only looks may list and subscribe.  The two are
        // named here, so an operation that is added later is refused until
        // someone decides otherwise.
        if (client->read_only &&
            !std::holds_alternative<list_request>(asked.body) &&
            !std::holds_alternative<subscribe_request>(asked.body))
        {
            read_only_refused_.fetch_add(1);
            send_reply(client,
                       error_reply_for(
                           asked.id, make_error_code(errc::permission_denied)));
            return;
        }
        if (const auto* wanted = std::get_if<subscribe_request>(&asked.body))
        {
            subscribe(client, *wanted);
            send_reply(client, reply{asked.id, done_reply{}});
            return;
        }
        if (std::holds_alternative<shutdown_request>(asked.body))
        {
            // The agent stops once the client has its answer.
            send_reply(client, reply{asked.id, done_reply{}},
                       [this] { request_stop(); });
            return;
        }

        ++client->pending;
        boost::asio::post(client->work,
                          [this, client, asked = std::move(asked)]
                          {
                              reply answer = run(asked);
                              boost::asio::post(
                                  io_,
                                  [this, client, answer = std::move(answer)]
                                  {
                                      --client->pending;
                                      if (client->closed)
                                          return;
                                      send_reply(client, answer);
                                      pump(client);
                                  });
                          });
    }

    void greet(const session_ptr& client, std::uint64_t id,
               const hello_request& hello)
    {
        if (client->greeted)
        {
            protocol_errors_.fetch_add(1);
            fail(client, id, make_error_code(protocol_errc::malformed_message));
            return;
        }
        if (hello.protocol != protocol_version)
        {
            protocol_errors_.fetch_add(1);
            fail(client, id, make_error_code(protocol_errc::version_mismatch));
            return;
        }
        client->greeted = true;
        client->read_only = hello.read_only;
        if (client->read_only)
            read_only_clients_.fetch_add(1);
        client->handshake_timer.cancel();
        hello_reply welcome;
        welcome.read_only = hello.read_only;
        welcome.protocol = protocol_version;
        welcome.agent_version = agent_version();
        welcome.process_id = platform::process_id();
        welcome.port_kinds = {loopback_kind};
        send_reply(client, reply{id, std::move(welcome)});
    }

    void subscribe(const session_ptr& client, const subscribe_request& wanted)
    {
        unsubscribe(client);
        client->queue = std::make_shared<event_queue>(
            wanted.data, options_.max_queued_events,
            options_.max_queued_event_bytes);
        client->interval = std::chrono::milliseconds(std::clamp(
            wanted.interval_ms, min_event_interval_ms, max_event_interval_ms));
        client->sent_revision = revision_.load();
        client->subscription = studio_.engine().subscribe(
            {}, [queue = client->queue](const endpoint_event& event)
            { queue->push(event); });
        arm_flush(client);
    }

    void unsubscribe(const session_ptr& client)
    {
        if (!client->queue)
            return;
        client->queue->stop();
        studio_.engine().unsubscribe(client->subscription);
        client->queue.reset();
        client->subscription = 0;
        client->flush_timer.cancel();
    }

    void arm_flush(const session_ptr& client)
    {
        client->flush_timer.expires_after(client->interval);
        client->flush_timer.async_wait(
            [this, client,
             queue = client->queue](boost::system::error_code cancelled)
            {
                // A new subscription has a new queue and its own timer.
                if (cancelled || client->closed || client->queue != queue)
                    return;
                flush(client);
                arm_flush(client);
            });
    }

    /// Sends what the queue holds as one frame.  While the batch before is
    /// still unread nothing is sent: the queue goes on filling, and dropping
    /// when it is full, so a client that reads slowly costs the agent a
    /// bounded amount of memory and the hub nothing.
    void flush(const session_ptr& client)
    {
        if (client->batch_unsent)
            return;
        event_batch batch;
        client->queue->take(batch);
        batch.revision = revision_.load();
        if (batch.events.empty() && batch.dropped == 0 &&
            batch.revision == client->sent_revision)
            return;
        auto bytes = encode_frame(encode(batch));
        if (!bytes)
        {
            // Too much for one frame: say how much was lost instead.
            batch.dropped += batch.events.size();
            batch.events.clear();
            bytes = encode_frame(encode(batch));
            if (!bytes)
                return;
        }
        dropped_events_.fetch_add(batch.dropped);
        client->sent_revision = batch.revision;
        client->batch_unsent = true;
        enqueue(client, outgoing{std::move(*bytes), {}, true});
    }

    void send_reply(const session_ptr& client, const reply& answer,
                    std::function<void()> after = {})
    {
        auto bytes = encode_frame(encode(answer));
        if (!bytes)
            bytes =
                encode_frame(encode(error_reply_for(answer.id, bytes.error())));
        if (!bytes)
        {
            close(client);
            return;
        }
        enqueue(client, outgoing{std::move(*bytes), std::move(after), false});
    }

    /// Sends an error and closes the connection once it is written.
    void fail(const session_ptr& client, std::uint64_t id,
              const error_code& error)
    {
        client->leaving = true;
        // A client that does not read the error is not waited for for ever.
        client->handshake_timer.expires_after(options_.handshake_timeout);
        client->handshake_timer.async_wait(
            [this, client](boost::system::error_code cancelled)
            {
                if (!cancelled)
                    close(client);
            });
        send_reply(client, error_reply_for(id, error),
                   [this, client] { close(client); });
    }

    void enqueue(const session_ptr& client, outgoing item)
    {
        if (client->closed)
            return;
        client->unsent_bytes += item.bytes.size();
        client->unsent.push_back(std::move(item));
        if (client->unsent_bytes > options_.max_unsent_bytes)
        {
            slow_clients_closed_.fetch_add(1);
            close(client);
            return;
        }
        if (!client->writing)
            write_next(client);
    }

    void write_next(const session_ptr& client)
    {
        client->writing = true;
        const outgoing& front = client->unsent.front();
        client->stream->async_write_some(
            boost::asio::buffer(front.bytes.data() + client->write_offset,
                                front.bytes.size() - client->write_offset),
            [this, client](boost::system::error_code failed, std::size_t size)
            {
                client->writing = false;
                if (client->closed)
                    return;
                if (failed)
                {
                    close(client);
                    return;
                }
                client->write_offset += size;
                if (client->write_offset == client->unsent.front().bytes.size())
                {
                    outgoing sent = std::move(client->unsent.front());
                    client->unsent.pop_front();
                    client->unsent_bytes -= sent.bytes.size();
                    client->write_offset = 0;
                    if (sent.batch)
                        client->batch_unsent = false;
                    if (sent.after)
                        sent.after();
                }
                if (!client->closed && !client->unsent.empty())
                    write_next(client);
            });
    }

    void close(const session_ptr& client)
    {
        if (client->closed)
            return;
        client->closed = true;
        client->handshake_timer.cancel();
        unsubscribe(client);
        client->stream->close();
        // The bytes stay until the session goes: a write that is being
        // cancelled may still be looking at them.  What was to run after
        // them holds the session, so it is let go here.
        for (outgoing& item : client->unsent)
            item.after = nullptr;
        if (client->read_only)
            read_only_clients_.fetch_sub(1);
        sessions_.erase(client);
        clients_.store(sessions_.size());
    }

    // -- the request pool ----------------------------------------------------

    reply run(const request& asked) noexcept
    {
        reply answer;
        answer.id = asked.id;
        try
        {
            answer.body =
                std::visit([this](const auto& body)
                           { return this->answer_to(body); }, asked.body);
        }
        catch (const std::exception&)
        {
            answer.body = error_reply{to_wire(make_error_code(errc::internal))};
        }
        return answer;
    }

    static reply_body refused(const error_code& error)
    {
        return error_reply{to_wire(error)};
    }

    reply_body answer_to(const list_request&)
    {
        list_reply out;
        // Read first: a change that lands while the lists are read is then
        // announced by the next batch, never missed.
        out.revision = revision_.load();
        for (const endpoint_info& info : studio_.endpoint_infos())
        {
            out.ports.push_back(
                summarise(info, studio_.advertised_path(info.id)));
        }
        for (const link_info& info : studio_.link_infos())
            out.links.push_back(summarise(info));
        return out;
    }

    reply_body answer_to(const create_port_request& body)
    {
        if (body.kind != loopback_kind)
            return refused(make_error_code(errc::unsupported));
        const auto created =
            studio_.create_virtual_port(body.name, virtual_port_kind::loopback);
        if (!created)
            return refused(created.error());
        revision_.fetch_add(1);
        return create_port_reply{(*created)->id()};
    }

    reply_body answer_to(const remove_port_request& body)
    {
        const auto removed = studio_.remove_port(body.endpoint);
        if (!removed)
            return refused(removed.error());
        revision_.fetch_add(1);
        return done_reply{};
    }

    reply_body answer_to(const connect_request& body)
    {
        const auto linked =
            body.one_way ? studio_.engine().redirect(body.first, body.second)
                         : studio_.engine().connect(body.first, body.second);
        if (!linked)
            return refused(linked.error());
        revision_.fetch_add(1);
        return connect_reply{*linked};
    }

    /// As the window's Send pane: bytes for an open port, and for a mock
    /// fixture the device speaking.
    reply_body answer_to(const write_request& body)
    {
        const auto target = studio_.engine().find_endpoint(body.endpoint);
        if (!target)
            return refused(make_error_code(errc::not_found));
        if (!target->is_open())
            return refused(make_error_code(errc::not_open));
        if (target->kind() == endpoint_kind::mock)
        {
            const auto injected =
                studio_.inject_data(body.endpoint, bytes_view(body.data));
            if (!injected)
                return refused(injected.error());
            return done_reply{};
        }
        target->write(bytes_view(body.data));
        return done_reply{};
    }

    // The client thread answers these itself; they never reach the pool.
    static reply_body answer_to(const hello_request&)
    {
        return refused(make_error_code(errc::internal));
    }
    static reply_body answer_to(const subscribe_request&)
    {
        return refused(make_error_code(errc::internal));
    }
    static reply_body answer_to(const shutdown_request&)
    {
        return refused(make_error_code(errc::internal));
    }

    studio& studio_;
    const server_options options_;

    boost::asio::io_context io_;
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        work_;
    std::thread io_thread_;
    std::unique_ptr<boost::asio::thread_pool> pool_;

    // The client thread only.
    std::unique_ptr<local_listener> listener_;
    boost::asio::steady_timer accept_retry_;
    std::unordered_set<session_ptr> sessions_;
    bool stopping_ = false;

    /// Counts the changes to the ports and links.
    std::atomic<std::uint64_t> revision_{0};

    mutable std::mutex state_mutex_;
    std::condition_variable stop_signal_;
    std::string address_;
    bool started_ = false;
    bool stopped_ = false;
    bool stop_requested_ = false;

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> refused_busy_{0};
    std::atomic<std::uint64_t> protocol_errors_{0};
    std::atomic<std::uint64_t> handshake_timeouts_{0};
    std::atomic<std::uint64_t> slow_clients_closed_{0};
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> read_only_refused_{0};
    std::atomic<std::uint64_t> clients_{0};
    std::atomic<std::uint64_t> read_only_clients_{0};
};

server::server(studio& app, server_options options)
    : impl_(std::make_unique<impl>(app, std::move(options)))
{
}

server::~server() = default;

result<void> server::start()
{
    return impl_->start();
}

void server::stop()
{
    impl_->stop();
}

std::string server::address() const
{
    return impl_->address();
}

void server::request_stop()
{
    impl_->request_stop();
}

void server::wait_for_stop_request()
{
    impl_->wait_for_stop_request();
}

bool server::wait_for_stop_request(std::chrono::milliseconds limit)
{
    return impl_->wait_for_stop_request(limit);
}

server_counters server::counters() const
{
    return impl_->counters();
}

} // namespace vss::agent
