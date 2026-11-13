//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control gateway.  Two kinds of thread meet here and each has one job:
//
//   the client thread    runs `io_`: every accept, every read from and write
//                        to a client, every timer, every decision about a
//                        request and all the state of a connection.
//   the request pool     runs the requests that wait for the agent, one
//                        session's requests in order and different sessions
//                        side by side.  The only state of a session it
//                        touches is the session's connection to the agent.
//
// A function below says in its comment which of them calls it.
//
// The order of the checks on a connection is the design's
// (docs/remote-control.md, sections 3 to 5) and none may be skipped:
//
//   1. the operating system lets only this user open the endpoint;
//   2. `admit` asks the kernel who connected and closes anyone else unseen;
//   3. until `hello` a frame may have 4 KiB, the only request is `hello`,
//      and the connection has five seconds;
//   4. `hello` makes the principal, once; nothing changes it afterwards.
//      With a token the principal is the token, and every way a token can
//      be wrong gets the same answer;
//   5. every later request of a token session first asks whether the token
//      still stands, then passes the rate limit and then `authorise`.
//
// A token is in this file only as a `secret_string` on its way to the token
// store, and as the bytes of the one reply that hands a new token to the
// person who made it.  It is never given to the audit log.
//

#include <vss/control/gateway.hpp>

#include <vss/agent/client.hpp>
#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/audit_log.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/core/types.hpp>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>

#include <algorithm>
#include <array>
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

namespace vss::control
{
namespace
{

using steady = std::chrono::steady_clock;

constexpr std::size_t read_buffer_size = std::size_t{64} * 1024;
/// Frames of one session handled before the others get a turn.
constexpr int frames_per_turn = 64;
constexpr std::chrono::milliseconds accept_retry_delay{100};
constexpr unsigned request_threads = 2;

/// Bytes waiting for a client to read them.
struct outgoing
{
    byte_buffer bytes;
    /// Runs on the client thread once the bytes are written.
    std::function<void()> after;
    /// The bytes hold a token: they are wiped when they have been written,
    /// or when the session ends first.
    bool secret = false;
};

/// Counts the audit records of one kind that were left out because the one
/// before was too recent.
struct audit_burst
{
    std::optional<steady::time_point> last;
    std::uint64_t suppressed = 0;
};

/// One connection.  Everything here belongs to the client thread, except
/// `work` and `agent`, which belong to the request pool.
struct session
{
    session(std::unique_ptr<agent::local_stream> connection, transport how,
            agent::peer_identity who_connected, std::uint64_t serial,
            boost::asio::io_context& io, boost::asio::thread_pool& pool)
        : stream(std::move(connection)), via(how),
          peer(std::move(who_connected)), number(serial),
          work(pool.get_executor()), handshake_timer(io),
          reader(max_hello_frame_size), read_buffer(read_buffer_size)
    {
    }

    ~session()
    {
        // A reply with a token in it that was never written.
        for (outgoing& item : unsent)
        {
            if (item.secret)
                secure_wipe(reinterpret_cast<char*>(item.bytes.data()),
                            item.bytes.size());
        }
    }

    session(const session&) = delete;
    session& operator=(const session&) = delete;

    std::unique_ptr<agent::local_stream> stream;
    const transport via;
    const agent::peer_identity peer;
    /// Names the connection in the audit log.
    const std::uint64_t number;
    boost::asio::strand<boost::asio::thread_pool::executor_type> work;
    boost::asio::steady_timer handshake_timer;
    agent::frame_reader reader;
    byte_buffer read_buffer;

    std::deque<outgoing> unsent;
    std::size_t unsent_bytes = 0;
    std::size_t write_offset = 0;
    bool writing = false;

    bool reading = false;
    bool closed = false;
    /// An error was sent: nothing more is read, and the connection closes
    /// when the error has been written.
    bool leaving = false;
    /// Why the connection is being closed, for the audit log.
    std::string close_reason;
    /// Requests handed to the pool whose replies are not queued yet.
    std::size_t pending = 0;

    /// Set by `hello`, once, and never changed.
    std::optional<principal> who;
    /// The id of the token this session was opened with; empty for a
    /// session of the local user.
    std::string token_id;

    /// Who the session is, for the audit log.  Empty before `hello`.
    const std::string& principal_id() const noexcept
    {
        static const std::string nobody;
        return who ? who->id : nobody;
    }

    /// What the session may do.  Nothing before `hello`.
    scope_set scopes() const noexcept
    {
        return who ? who->scopes : scope_set{};
    }

    /// The requests this session may still send now.
    double tokens = 0.0;
    steady::time_point refilled;

    audit_burst forbidden_burst;
    audit_burst rate_burst;

    /// The request pool only: this session's own connection to the agent.
    std::unique_ptr<agent::client> agent;
};

using session_ptr = std::shared_ptr<session>;

reply error_reply_for(std::uint64_t id, api_errc error)
{
    return reply{id, error_reply{to_api_error(error)}};
}

std::string joined(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& name : names)
    {
        if (!out.empty())
            out += ' ';
        out += name;
    }
    return out;
}

port_entry summarise(const agent::port_summary& port)
{
    port_entry out;
    out.id = port_id_text(port.id);
    out.name = port.name;
    out.path = port.path;
    out.kind = to_string(port.kind);
    out.state = to_string(port.state);
    out.open = port.open;
    out.bytes_rx = port.bytes_rx;
    out.bytes_tx = port.bytes_tx;
    return out;
}

link_entry summarise(const agent::link_summary& link)
{
    link_entry out;
    out.id = link_id_text(link.id);
    out.mode = to_string(link.mode);
    for (const endpoint_id source : link.sources)
        out.sources.push_back(port_id_text(source));
    for (const endpoint_id target : link.targets)
        out.targets.push_back(port_id_text(target));
    out.bytes = link.bytes;
    return out;
}

} // namespace

std::string gateway_version()
{
    return VSS_CONTROL_VERSION;
}

class gateway::impl
{
public:
    explicit impl(gateway_options options) : options_(std::move(options)) {}

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

        // The public endpoint first: it is the one that says whether a
        // gateway of this user is already running.
        auto listening = agent::listen_local(io_.get_executor(),
                                             agent::local_endpoint::control,
                                             options_.instance);
        if (!listening)
            return listening.error();
        auto administering = agent::listen_local(
            io_.get_executor(), agent::local_endpoint::control_admin,
            options_.instance);
        if (!administering)
            return administering.error();

        // No audit log, no gateway: nothing is served that is not recorded.
        audit_options where;
        where.directory = options_.audit_directory;
        if (where.directory.empty())
        {
            const auto directory = default_audit_directory();
            if (!directory)
                return directory.error();
            where.directory = *directory;
        }
        // No token file that can be trusted, no gateway either: it is never
        // replaced by an empty one, which would bring revoked tokens back.
        std::filesystem::path token_file = options_.token_file;
        if (token_file.empty())
            token_file = where.directory / "tokens.json";
        auto opened = audit_log::open(std::move(where));
        if (!opened)
            return opened.error();
        auto tokens = token_store::open(std::move(token_file), options_.clock);
        if (!tokens)
            return tokens.error();
        audit_ = std::move(*opened);
        tokens_ = std::move(*tokens);

        doors_[0].listener = std::move(*listening);
        doors_[0].via = transport::local;
        doors_[1].listener = std::move(*administering);
        doors_[1].via = transport::local_admin;
        address_ = doors_[0].listener->address();
        admin_address_ = doors_[1].listener->address();
        started_at_ = steady::now();

        audit_->record("gateway.started",
                       {{"version", gateway_version()},
                        {"address", address_},
                        {"admin_address", admin_address_},
                        {"local_scopes", joined(scope_names(local_scopes()))}});

        pool_ = std::make_unique<boost::asio::thread_pool>(request_threads);
        work_.emplace(io_.get_executor());
        for (door& entrance : doors_)
        {
            entrance.retry = std::make_unique<boost::asio::steady_timer>(io_);
            accept_next(entrance);
        }
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
        // Close the doors and every connection on the thread that owns them.
        std::promise<void> closed;
        boost::asio::post(io_,
                          [this, &closed]
                          {
                              stopping_ = true;
                              for (door& entrance : doors_)
                              {
                                  entrance.retry->cancel();
                                  entrance.listener->close();
                              }
                              const std::vector<session_ptr> all(
                                  sessions_.begin(), sessions_.end());
                              for (const session_ptr& client : all)
                                  close(client, "gateway_stopped");
                              closed.set_value();
                          });
        closed.get_future().wait();
        // Requests still running finish; what they post back finds its
        // session closed.  Only then may the client thread run out of work.
        pool_->join();
        work_.reset();
        if (io_thread_.joinable())
            io_thread_.join();
        for (door& entrance : doors_)
            entrance.listener.reset();
        audit_->record("gateway.stopped");
        // The file is let go, so the next gateway of this name can open it.
        audit_.reset();
        request_stop();
    }

    std::string address() const
    {
        const std::lock_guard lock(state_mutex_);
        return address_;
    }

    std::string admin_address() const
    {
        const std::lock_guard lock(state_mutex_);
        return admin_address_;
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

    gateway_counters counters() const
    {
        gateway_counters out;
        out.accepted = accepted_.load();
        out.refused_peer = refused_peer_.load();
        out.refused_busy = refused_busy_.load();
        out.protocol_errors = protocol_errors_.load();
        out.handshake_timeouts = handshake_timeouts_.load();
        out.auth_failures = auth_failures_.load();
        out.forbidden = forbidden_.load();
        out.rate_limited = rate_limited_.load();
        out.slow_clients_closed = slow_clients_closed_.load();
        out.connections = connections_.load();
        out.sessions = greeted_.load();
        return out;
    }

private:
    /// One of the two endpoints: its listener and what it makes of whoever
    /// comes through it.
    struct door
    {
        std::unique_ptr<agent::local_listener> listener;
        transport via = transport::local;
        std::unique_ptr<boost::asio::steady_timer> retry;
    };

    /// What the public endpoint grants.  `admin` cannot be configured in.
    scope_set local_scopes() const noexcept
    {
        return effective_scopes(options_.local_scopes, transport::local,
                                every_scope());
    }

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
                // Out of memory in a handler.  Ask the program to stop, and
                // keep the thread running so that `stop` can still reach it.
                request_stop();
            }
        }
    }

    void accept_next(door& entrance)
    {
        entrance.listener->async_accept(
            [this, &entrance](boost::system::error_code failed,
                              std::unique_ptr<agent::local_stream> stream)
            { on_accept(entrance, failed, std::move(stream)); });
    }

    void on_accept(door& entrance, const boost::system::error_code& failed,
                   std::unique_ptr<agent::local_stream> stream)
    {
        if (stopping_ || failed == boost::asio::error::operation_aborted)
            return;
        if (failed || !stream)
        {
            // A client that came and went, or the system out of handles for
            // a moment.  Try again, but not in a tight loop.
            entrance.retry->expires_after(accept_retry_delay);
            entrance.retry->async_wait(
                [this, &entrance](boost::system::error_code cancelled)
                {
                    if (!cancelled && !stopping_)
                        accept_next(entrance);
                });
            return;
        }
        accepted_.fetch_add(1);
        admit(entrance.via, std::move(stream));
        accept_next(entrance);
    }

    /// The second check of a connection: the kernel is asked who made it.
    /// A connection of another user, or one the kernel will not vouch for,
    /// is closed before a byte of it is read and is told nothing.
    void admit(transport via, std::unique_ptr<agent::local_stream> stream)
    {
        auto asked = stream->peer();
        if (!asked || !asked->same_user)
        {
            refused_peer_.fetch_add(1);
            record_burst(
                refused_burst_, "connection.refused",
                {{"transport", transport_name(via)},
                 {"reason", asked ? "other_user" : "peer_unknown"},
                 {"user", asked ? asked->user : std::string()},
                 {"process", asked ? asked->process_id : std::uint64_t{0}}});
            stream->close();
            return;
        }

        auto client =
            std::make_shared<session>(std::move(stream), via, std::move(*asked),
                                      ++last_session_, io_, *pool_);
        client->tokens = options_.request_burst;
        client->refilled = steady::now();
        const bool full = waiting_for_hello() >= options_.max_unauthenticated;
        sessions_.insert(client);
        connections_.store(sessions_.size());
        // Bounds the life of a connection that never speaks, and of one that
        // was refused and does not read why.
        client->handshake_timer.expires_after(options_.handshake_timeout);
        client->handshake_timer.async_wait(
            [this, client](boost::system::error_code cancelled)
            {
                if (cancelled || client->closed || client->who)
                    return;
                handshake_timeouts_.fetch_add(1);
                reject(client, "handshake_timeout");
                close(client, "handshake_timeout");
            });
        if (full)
        {
            refused_busy_.fetch_add(1);
            reject(client, "too_many_waiting");
            fail(client, 0, api_errc::busy);
        }
        else
        {
            pump(client);
        }
    }

    std::size_t waiting_for_hello() const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(sessions_, [](const session_ptr& other)
                                  { return !other->who.has_value(); }));
    }

    /// Handles the frames that have arrived, then reads more.  It stops while
    /// the session has `max_pending_requests` waiting, which is what slows a
    /// client that sends faster than its requests can run.
    void pump(const session_ptr& client)
    {
        int budget = frames_per_turn;
        while (!client->closed && !client->leaving &&
               client->pending < options_.max_pending_requests)
        {
            if (budget-- == 0)
            {
                // Let the other sessions have the thread.
                boost::asio::post(io_, [this, client] { pump(client); });
                return;
            }
            auto next = client->reader.next();
            if (!next)
            {
                // A length that cannot be right, or a frame over the limit:
                // the stream cannot be read past it.
                protocol_errors_.fetch_add(1);
                refuse_connection(client, 0,
                                  next.error() ==
                                          agent::protocol_errc::oversized_frame
                                      ? api_errc::limit_exceeded
                                      : api_errc::invalid_argument,
                                  "bad_frame");
                return;
            }
            std::optional<frame> ready = std::move(*next);
            if (!ready)
                break;
            const bool may_hold_token = !client->who.has_value();
            handle_frame(client, *ready);
            if (may_hold_token)
            {
                // A `hello` may have carried a token.  The copies this
                // function can reach are wiped; the one the token store was
                // given is wiped by its own type.
                secure_wipe(ready->header.data(), ready->header.size());
                if (!client->reading)
                    secure_wipe(
                        reinterpret_cast<char*>(client->read_buffer.data()),
                        client->read_buffer.size());
            }
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
                    close(client, "peer_closed");
                    return;
                }
                client->reader.feed(
                    bytes_view(client->read_buffer.data(), size));
                pump(client);
            });
    }

    void handle_frame(const session_ptr& client, const frame& received)
    {
        auto decoded = control::decode_request(received);
        if (!client->who)
        {
            // Before `hello` there is one request, and a caller that sends
            // anything else learns nothing: not which operations exist and
            // not what was wrong with what it sent.
            const auto* greeting =
                decoded ? std::get_if<hello_request>(&decoded->body) : nullptr;
            if (greeting == nullptr)
            {
                protocol_errors_.fetch_add(1);
                refuse_connection(client, control::peek_request_id(received),
                                  api_errc::unauthenticated,
                                  "request_before_hello");
                return;
            }
            greet(client, decoded->id, *greeting);
            return;
        }

        if (!decoded)
        {
            protocol_errors_.fetch_add(1);
            const std::uint64_t id = control::peek_request_id(received);
            // A well-formed request for something this gateway does not
            // have costs the client an error, not its connection.
            if (decoded.error() == api_errc::unsupported)
                send_reply(client, error_reply_for(id, api_errc::unsupported));
            else
                refuse_connection(client, id, api_errc::invalid_argument,
                                  "bad_request");
            return;
        }

        request& asked = *decoded;
        const operation_info& wanted = info_of(operation_of(asked.body));
        if (wanted.id == operation::hello)
        {
            // The principal is made once.  A second `hello` cannot change it.
            protocol_errors_.fetch_add(1);
            refuse_connection(client, asked.id, api_errc::invalid_argument,
                              "second_hello");
            return;
        }
        if (!client->token_id.empty())
        {
            // A token that was revoked or has ended stops working at the
            // next request, not at the next connection.
            const token_check standing = tokens_->standing(client->token_id);
            if (standing != token_check::accepted)
            {
                client->close_reason =
                    "token_" + std::string(token_check_name(standing));
                fail(client, asked.id, api_errc::unauthenticated);
                return;
            }
        }
        if (!take_token(*client))
        {
            rate_limited_.fetch_add(1);
            record_burst(client->rate_burst, "request.rate_limited",
                         {{"session", client->number},
                          {"principal", client->principal_id()}});
            send_reply(client,
                       error_reply_for(asked.id, api_errc::limit_exceeded));
            return;
        }

        // The one place a request is allowed or refused.
        const decision allowed = authorise(*client->who, wanted);
        if (!allowed.allowed)
        {
            forbidden_.fetch_add(1);
            record_burst(
                client->forbidden_burst, "request.forbidden",
                {{"session", client->number},
                 {"principal", client->principal_id()},
                 {"operation", wanted.name},
                 {"missing", allowed.missing ? scope_name(*allowed.missing)
                                             : std::string_view()}});
            send_reply(client, error_reply_for(asked.id, api_errc::forbidden));
            return;
        }

        if (const auto* making =
                std::get_if<tokens_create_request>(&asked.body))
        {
            create_token(client, asked.id, *making);
            return;
        }
        if (std::holds_alternative<tokens_list_request>(asked.body))
        {
            tokens_reply listed;
            for (const token_record& record : tokens_->list())
                listed.tokens.push_back(entry_of(record));
            send_reply(client, reply{asked.id, std::move(listed)});
            return;
        }
        if (const auto* revoking =
                std::get_if<tokens_revoke_request>(&asked.body))
        {
            revoke_token(client, asked.id, *revoking);
            return;
        }
        if (wanted.id == operation::endpoint_stop)
        {
            audit_->record("operation", {{"session", client->number},
                                         {"principal", client->principal_id()},
                                         {"operation", wanted.name},
                                         {"outcome", "ok"}});
            // The gateway stops once the client has its answer.
            send_reply(client, reply{asked.id, done_reply{}},
                       [this] { request_stop(); });
            return;
        }

        ++client->pending;
        const std::uint64_t sessions_now = greeted_.load();
        boost::asio::post(client->work,
                          [this, client, asked = std::move(asked), sessions_now]
                          {
                              reply answer = run(*client, asked, sessions_now);
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

    /// `hello`: the connection becomes a principal, or is closed.
    void greet(const session_ptr& client, std::uint64_t id,
               const hello_request& hello)
    {
        if (hello.api != api_version)
        {
            protocol_errors_.fetch_add(1);
            refuse_connection(client, id, api_errc::unsupported, "api_version");
            return;
        }

        principal made;
        if (hello.token.empty())
        {
            made.id = "local:" + client->peer.user;
            made.via = client->via;
            made.scopes =
                client->via == transport::local_admin
                    ? effective_scopes(every_scope(), transport::local_admin,
                                       every_scope())
                    : local_scopes();
        }
        else
        {
            auto authenticated = authenticate(*client, hello.token);
            if (!authenticated)
            {
                // One answer for every way a token can be wrong.
                refuse_connection(client, id, api_errc::unauthenticated,
                                  "auth_failed");
                return;
            }
            made = std::move(*authenticated);
        }

        if (!has_room_for(made))
        {
            refused_busy_.fetch_add(1);
            reject(client, "too_many_sessions");
            fail(client, id, api_errc::busy);
            return;
        }

        client->who = std::move(made);
        client->handshake_timer.cancel();
        client->reader.set_limit(agent::max_frame_size);
        greeted_.fetch_add(1);

        hello_reply welcome;
        welcome.api = api_version;
        welcome.version = gateway_version();
        welcome.principal = client->principal_id();
        welcome.scopes = scope_names(client->scopes());
        welcome.features = {"ports.read"};

        audit_->record("session.opened",
                       {{"session", client->number},
                        {"principal", client->principal_id()},
                        {"transport", transport_name(client->via)},
                        {"scopes", joined(welcome.scopes)},
                        {"process", client->peer.process_id},
                        {"program", client->peer.program},
                        {"client", hello.client}});
        send_reply(client, reply{id, std::move(welcome)});
    }

    /// Checks the token a `hello` carried.  Nothing when it is refused; why
    /// is written to the audit log and told to nobody else.
    std::optional<principal> authenticate(session& client,
                                          const secret_string& token)
    {
        const auto now = steady::now();
        const std::string source = "local:" + client.peer.user;
        std::string token_id;
        std::string_view reason;
        if (client.via == transport::local_admin)
        {
            // The administration endpoint takes no token: who may use it is
            // decided by the operating system alone.
            reason = "token_on_admin_endpoint";
        }
        else
        {
            if (const auto parts = parse_token(token))
                token_id = parts->id;
            const std::string token_key = "token:" + token_id;
            if (!limiter_.allowed(source, now) ||
                (!token_id.empty() && !limiter_.allowed(token_key, now)))
            {
                // Not checked at all: a caller that is being slowed down
                // learns nothing from trying meanwhile.
                reason = "backoff";
            }
            else
            {
                const token_verdict verdict =
                    tokens_->verify(token, client.via);
                if (verdict.outcome == token_check::accepted)
                {
                    limiter_.succeeded(token_key);
                    client.token_id = verdict.id;
                    principal made;
                    made.id = "token:" + verdict.id;
                    made.via = client.via;
                    made.scopes = effective_scopes(verdict.scopes, client.via,
                                                   options_.local_ceiling);
                    return made;
                }
                reason = token_check_name(verdict.outcome);
                limiter_.failed(source, now);
                // An id no token has counts against the source only, so
                // guessing ids cannot fill the table of keys.
                if (verdict.outcome != token_check::malformed &&
                    verdict.outcome != token_check::unknown)
                    limiter_.failed(token_key, now);
            }
        }
        auth_failures_.fetch_add(1);
        record_burst(auth_burst_, "auth.failed",
                     {{"transport", transport_name(client.via)},
                      {"reason", reason},
                      {"token", token_id},
                      {"process", client.peer.process_id},
                      {"program", client.peer.program}});
        return std::nullopt;
    }

    static std::uint64_t milliseconds_of(wall_clock::time_point when)
    {
        const auto since =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                when.time_since_epoch());
        return since.count() < 0 ? 0
                                 : static_cast<std::uint64_t>(since.count());
    }

    static token_entry entry_of(const token_record& record)
    {
        token_entry out;
        out.id = record.id;
        out.label = record.label;
        out.scopes = scope_names(record.scopes);
        for (const transport via : record.transports)
            out.transports.emplace_back(transport_name(via));
        out.created_ms = milliseconds_of(record.created);
        out.expires_ms = milliseconds_of(record.expires);
        out.last_used_ms =
            record.last_used ? milliseconds_of(*record.last_used) : 0;
        out.revoked = record.revoked;
        return out;
    }

    /// `tokens.create`: makes a token and sends it, once, to whoever asked on
    /// the administration endpoint.
    void create_token(const session_ptr& client, std::uint64_t id,
                      const tokens_create_request& wanted)
    {
        api_errc refused = api_errc::success;
        token_request asked;
        asked.label = wanted.label;
        for (const std::string& name : wanted.scopes)
        {
            const auto parsed = parse_scope(name);
            if (!parsed)
                refused = api_errc::invalid_argument;
            else
                asked.scopes = asked.scopes | scope_set{*parsed};
        }
        const auto longest = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(longest_token_life)
                .count());
        if (wanted.life_s > longest)
            refused = api_errc::invalid_argument;
        else if (wanted.life_s != 0)
            asked.life = std::chrono::seconds(
                static_cast<std::chrono::seconds::rep>(wanted.life_s));

        result<new_token> made = make_error_code(errc::invalid_argument);
        if (refused == api_errc::success)
        {
            made = tokens_->create(asked);
            if (!made)
            {
                if (made.error() == errc::invalid_argument)
                    refused = api_errc::invalid_argument;
                else if (made.error() == errc::queue_full)
                    refused = api_errc::limit_exceeded;
                else
                    refused = api_errc::internal;
            }
        }
        audit_->record(
            "operation",
            {{"session", client->number},
             {"principal", client->principal_id()},
             {"operation", info_of(operation::tokens_create).name},
             {"token", made ? made->record.id : std::string()},
             {"label", wanted.label},
             {"scopes", joined(scope_names(asked.scopes))},
             {"outcome", refused == api_errc::success ? std::string_view("ok")
                                                      : code_name(refused)}});
        if (refused != api_errc::success)
        {
            send_reply(client, error_reply_for(id, refused));
            return;
        }
        token_created_reply answer;
        answer.entry = entry_of(made->record);
        answer.token = std::move(made->token);
        send_reply(client, reply{id, std::move(answer)}, {}, true);
    }

    /// `tokens.revoke`: the token stops working, and the sessions that were
    /// opened with it are closed now.
    void revoke_token(const session_ptr& client, std::uint64_t id,
                      const tokens_revoke_request& wanted)
    {
        const auto revoked = tokens_->revoke(wanted.id);
        const bool unknown = !revoked && revoked.error() == errc::not_found;
        std::uint64_t closed = 0;
        if (!unknown)
        {
            // Also when the file could not be written: the token is revoked
            // for as long as this gateway runs, and its sessions end.
            const std::vector<session_ptr> all(sessions_.begin(),
                                               sessions_.end());
            for (const session_ptr& other : all)
            {
                if (other->token_id == wanted.id)
                {
                    close(other, "token_revoked");
                    ++closed;
                }
            }
        }
        const api_errc outcome = revoked   ? api_errc::success
                                 : unknown ? api_errc::not_found
                                           : api_errc::internal;
        audit_->record(
            "operation",
            {{"session", client->number},
             {"principal", client->principal_id()},
             {"operation", info_of(operation::tokens_revoke).name},
             {"token", valid_token_id(wanted.id) ? wanted.id : std::string()},
             {"sessions_closed", closed},
             {"outcome", outcome == api_errc::success ? std::string_view("ok")
                                                      : code_name(outcome)}});
        if (outcome == api_errc::success)
            send_reply(client, reply{id, done_reply{}});
        else
            send_reply(client, error_reply_for(id, outcome));
    }

    /// True when one more session of `candidate` fits its endpoint.
    bool has_room_for(const principal& candidate) const
    {
        std::size_t on_endpoint = 0;
        std::size_t of_principal = 0;
        for (const session_ptr& other : sessions_)
        {
            if (!other->who || other->who->via != candidate.via)
                continue;
            ++on_endpoint;
            if (other->principal_id() == candidate.id)
                ++of_principal;
        }
        if (candidate.via == transport::local_admin)
            return on_endpoint < options_.max_admin_sessions;
        return on_endpoint < options_.max_sessions &&
               of_principal < options_.max_sessions_per_principal;
    }

    /// Takes one request out of the session's allowance, which refills at
    /// `requests_per_second` up to `request_burst`.
    bool take_token(session& client) const
    {
        const auto now = steady::now();
        const std::chrono::duration<double> passed = now - client.refilled;
        client.refilled = now;
        client.tokens =
            (std::min)(options_.request_burst,
                       client.tokens +
                           passed.count() * options_.requests_per_second);
        if (client.tokens < 1.0)
            return false;
        client.tokens -= 1.0;
        return true;
    }

    /// Writes an audit record unless one of the same kind from the same
    /// source was written less than `audit_burst_period` ago; then it is
    /// counted, and the count goes into the next one that is written.
    void record_burst(audit_burst& burst, std::string_view event,
                      std::vector<audit_field> fields)
    {
        const auto now = steady::now();
        if (burst.last && now - *burst.last < options_.audit_burst_period)
        {
            ++burst.suppressed;
            return;
        }
        fields.emplace_back("suppressed", burst.suppressed);
        burst.last = now;
        burst.suppressed = 0;
        audit_->record(event, fields);
    }

    /// Notes a connection that ended before it became a session.
    void reject(const session_ptr& client, std::string_view reason)
    {
        record_burst(rejected_burst_, "connection.rejected",
                     {{"transport", transport_name(client->via)},
                      {"reason", reason},
                      {"process", client->peer.process_id},
                      {"program", client->peer.program}});
    }

    /// Ends a connection for something the client did: before `hello` it is
    /// a rejected connection, afterwards the end of a session.
    void refuse_connection(const session_ptr& client, std::uint64_t id,
                           api_errc error, std::string_view reason)
    {
        if (!client->who)
            reject(client, reason);
        client->close_reason = std::string(reason);
        fail(client, id, error);
    }

    void send_reply(const session_ptr& client, const reply& answer,
                    std::function<void()> after = {}, bool secret = false)
    {
        frame encoded = encode(answer);
        auto bytes = agent::encode_frame(encoded);
        if (secret)
            secure_wipe(encoded.header.data(), encoded.header.size());
        if (!bytes)
            bytes = agent::encode_frame(
                encode(error_reply_for(answer.id, api_errc::internal)));
        if (!bytes)
        {
            close(client, "internal_error");
            return;
        }
        enqueue(client, outgoing{std::move(*bytes), std::move(after), secret});
    }

    /// Sends an error and closes the connection once it is written.
    void fail(const session_ptr& client, std::uint64_t id, api_errc error)
    {
        client->leaving = true;
        // A client that does not read the error is not waited for for ever.
        client->handshake_timer.expires_after(options_.handshake_timeout);
        client->handshake_timer.async_wait(
            [this, client](boost::system::error_code cancelled)
            {
                if (!cancelled)
                    close(client, client->close_reason);
            });
        send_reply(client, error_reply_for(id, error),
                   [this, client] { close(client, client->close_reason); });
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
            close(client, "slow_client");
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
                    close(client, "peer_closed");
                    return;
                }
                client->write_offset += size;
                if (client->write_offset == client->unsent.front().bytes.size())
                {
                    outgoing sent = std::move(client->unsent.front());
                    client->unsent.pop_front();
                    client->unsent_bytes -= sent.bytes.size();
                    client->write_offset = 0;
                    if (sent.secret)
                        secure_wipe(reinterpret_cast<char*>(sent.bytes.data()),
                                    sent.bytes.size());
                    if (sent.after)
                        sent.after();
                }
                if (!client->closed && !client->unsent.empty())
                    write_next(client);
            });
    }

    void close(const session_ptr& client, std::string_view reason)
    {
        if (client->closed)
            return;
        client->closed = true;
        client->handshake_timer.cancel();
        client->stream->close();
        // The bytes stay until the session goes: a write that is being
        // cancelled may still be looking at them.  What was to run after
        // them holds the session, so it is let go here.
        for (outgoing& item : client->unsent)
            item.after = nullptr;
        // The record first, the counters after: whoever waits for a count
        // to drop then finds the record already written.
        if (client->who)
        {
            audit_->record(
                "session.closed",
                {{"session", client->number},
                 {"principal", client->principal_id()},
                 {"reason",
                  reason.empty() ? std::string_view("closed") : reason},
                 {"forbidden_unrecorded", client->forbidden_burst.suppressed},
                 {"rate_limited_unrecorded", client->rate_burst.suppressed}});
            greeted_.fetch_sub(1);
        }
        sessions_.erase(client);
        connections_.store(sessions_.size());
    }

    // -- the request pool ----------------------------------------------------

    /// This session's connection to the agent, made on first use.
    agent::client* agent_of(session& client)
    {
        if (!client.agent)
        {
            agent::client_options how;
            how.instance = options_.agent_instance;
            how.name = "vss_gate";
            how.timeout = options_.agent_timeout;
            auto connected = agent::client::connect(how);
            if (!connected)
                return nullptr;
            client.agent = std::move(*connected);
        }
        return client.agent.get();
    }

    reply run(session& client, const request& asked,
              std::uint64_t sessions_now) noexcept
    {
        try
        {
            if (std::holds_alternative<status_request>(asked.body))
                return reply{asked.id, status(client, sessions_now)};
            if (std::holds_alternative<ports_list_request>(asked.body))
                return list_ports(client, asked.id);
        }
        catch (const std::exception&)
        {
            client.agent.reset();
        }
        return error_reply_for(asked.id, api_errc::internal);
    }

    status_reply status(session& client, std::uint64_t sessions_now)
    {
        status_reply out;
        out.uptime_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                steady::now() - started_at_)
                .count());
        out.sessions = sessions_now;
        // Asking the agent for its list is how a connection that has gone
        // quiet is told from one that still answers.
        if (agent::client* const source = agent_of(client))
        {
            if (source->list())
            {
                out.agent_running = true;
                out.agent_version = source->agent().agent_version;
            }
            else
            {
                client.agent.reset();
            }
        }
        return out;
    }

    reply list_ports(session& client, std::uint64_t id)
    {
        agent::client* const source = agent_of(client);
        if (source == nullptr)
            return error_reply_for(id, api_errc::unavailable);
        const auto listed = source->list();
        if (!listed)
        {
            // Whatever went wrong, the next request starts with a new
            // connection.  Nothing about the agent's error crosses.
            client.agent.reset();
            return error_reply_for(id, api_errc::unavailable);
        }
        ports_reply out;
        out.revision = listed->revision;
        out.ports.reserve(listed->ports.size());
        for (const agent::port_summary& port : listed->ports)
            out.ports.push_back(summarise(port));
        out.links.reserve(listed->links.size());
        for (const agent::link_summary& link : listed->links)
            out.links.push_back(summarise(link));
        return reply{id, std::move(out)};
    }

    const gateway_options options_;

    boost::asio::io_context io_;
    std::optional<boost::asio::executor_work_guard<
        boost::asio::io_context::executor_type>>
        work_;
    std::thread io_thread_;
    std::unique_ptr<boost::asio::thread_pool> pool_;
    /// Written from the client thread; its own lock makes that safe.
    std::unique_ptr<audit_log> audit_;
    std::unique_ptr<token_store> tokens_;
    steady::time_point started_at_;

    // The client thread only.
    std::array<door, 2> doors_;
    std::unordered_set<session_ptr> sessions_;
    std::uint64_t last_session_ = 0;
    audit_burst refused_burst_;
    audit_burst auth_burst_;
    failure_limiter limiter_{options_.auth_limits};
    audit_burst rejected_burst_;
    bool stopping_ = false;

    mutable std::mutex state_mutex_;
    std::condition_variable stop_signal_;
    std::string address_;
    std::string admin_address_;
    bool started_ = false;
    bool stopped_ = false;
    bool stop_requested_ = false;

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> refused_peer_{0};
    std::atomic<std::uint64_t> refused_busy_{0};
    std::atomic<std::uint64_t> protocol_errors_{0};
    std::atomic<std::uint64_t> handshake_timeouts_{0};
    std::atomic<std::uint64_t> auth_failures_{0};
    std::atomic<std::uint64_t> forbidden_{0};
    std::atomic<std::uint64_t> rate_limited_{0};
    std::atomic<std::uint64_t> slow_clients_closed_{0};
    std::atomic<std::uint64_t> connections_{0};
    std::atomic<std::uint64_t> greeted_{0};
};

gateway::gateway(gateway_options options)
    : impl_(std::make_unique<impl>(std::move(options)))
{
}

gateway::~gateway() = default;

result<void> gateway::start()
{
    return impl_->start();
}

void gateway::stop()
{
    impl_->stop();
}

std::string gateway::address() const
{
    return impl_->address();
}

std::string gateway::admin_address() const
{
    return impl_->admin_address();
}

void gateway::request_stop()
{
    impl_->request_stop();
}

void gateway::wait_for_stop_request()
{
    impl_->wait_for_stop_request();
}

bool gateway::wait_for_stop_request(std::chrono::milliseconds limit)
{
    return impl_->wait_for_stop_request(limit);
}

gateway_counters gateway::counters() const
{
    return impl_->counters();
}

} // namespace vss::control
