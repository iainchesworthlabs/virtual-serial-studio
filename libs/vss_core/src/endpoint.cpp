//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/endpoint.hpp>
#include <vss/core/profile.hpp>
#include <vss/core/hub.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <utility>
#include <vector>

namespace vss {

endpoint::endpoint(hub& owner, endpoint_id id, std::string name,
                   endpoint_kind kind, std::shared_ptr<transport> transport,
                   const endpoint_options& options)
    : owner_(owner)
    , id_(id)
    , name_(std::move(name))
    , kind_(kind)
    , transport_(std::move(transport))
    , options_(options)
    , strand_(boost::asio::make_strand(owner.executor()))
    , read_buffer_(options_.read_buffer_size == 0 ? std::size_t{1}
                                                  : options_.read_buffer_size)
{
    stats_.started = clock_type::now();
    if (options_.max_write_queue_bytes == 0)
        options_.max_write_queue_bytes = 1;
    if (transport_)
        lines_ = transport_->lines();
}

endpoint::~endpoint()
{
    if (const auto current = active_transport())
        current->close();
}

std::shared_ptr<transport> endpoint::active_transport() const
{
    std::lock_guard lock(transport_mutex_);
    return transport_;
}

endpoint_state endpoint::state() const noexcept
{
    return state_.load();
}

bool endpoint::is_open() const noexcept
{
    const auto current = active_transport();
    return state_.load() == endpoint_state::open && current
        && current->is_open();
}

std::string endpoint::public_path() const
{
    const auto current = active_transport();
    return current ? current->public_path() : std::string();
}

std::string endpoint::description() const
{
    const auto current = active_transport();
    return current ? current->describe() : std::string();
}

boost::asio::any_io_executor endpoint::get_executor() const
{
    return strand_;
}

void endpoint::start()
{
    if (running_.exchange(true))
        return;

    VSS_PROFILE_ZONE("endpoint.start");

    transition(endpoint_state::opening);

    const auto current = active_transport();
    if (!current || !current->is_open())
    {
        running_ = false;
        transition(endpoint_state::failed);
        emit(make_error_code(errc::not_open), "transport is not open");
        return;
    }

    {
        std::lock_guard lock(lines_mutex_);
        lines_ = current->lines();
    }
    transition(endpoint_state::open);

    // A transport may log what it notices about its device. The sink holds the
    // endpoint weakly: the transport can outlive a call into it.
    current->set_notice_sink(
        [weak = weak_from_this()](std::string text)
        {
            if (const auto self = weak.lock())
                self->emit({}, std::move(text), event_kind::log_message);
        });

    attach_break_sink(current);

    // Arm the first read on the strand, like every later one.
    auto self = shared_from_this();
    boost::asio::post(strand_, [self] { self->do_read(); });
}

void endpoint::attach_break_sink(const std::shared_ptr<transport>& current)
{
    // A break reaches the strand in the order the transport reports it, which
    // is the order of its read completions.  The sink holds the endpoint
    // weakly, as the notice sink does, and the generation of the transport it
    // was given to, so a break of a transport since replaced is dropped.
    const std::uint64_t generation = generation_.load();
    current->set_break_sink(
        [weak = weak_from_this(), generation](const line_break& item)
        {
            const auto self = weak.lock();
            if (!self)
                return;
            boost::asio::post(self->strand_,
                              [self, generation, item]
                              {
                                  if (generation == self->generation_.load())
                                      self->on_break_read(generation, item);
                              });
        });
}

void endpoint::receive_break(const line_break& item)
{
    auto self = shared_from_this();
    boost::asio::post(strand_,
                      [self, item]
                      { self->on_break_read(self->generation_.load(), item); });
}

void endpoint::on_break_read(std::uint64_t /*generation*/,
                             const line_break& item)
{
    VSS_PROFILE_ZONE("endpoint.break_read");
    if (!running_.load())
        return;

    {
        std::lock_guard lock(stats_mutex_);
        if (item.ends_break())
            stats_.breaks_rx += 1;
        stats_.last_activity = clock_type::now();
    }

    // The same path as bytes: routed before the event is published, on the
    // I/O thread, in order with the chunks this endpoint read before it.
    owner_.route_break(id_, item);
    if (options_.capture_traffic)
        emit_break(data_direction::rx, item);
}

void endpoint::write_break(const line_break& item, write_handler handler)
{
    VSS_PROFILE_ZONE("endpoint.enqueue_break");
    {
        std::lock_guard lock(write_mutex_);
        auto pending = std::make_shared<pending_write>();
        pending->data = std::make_shared<byte_buffer>();
        pending->handler = std::move(handler);
        pending->item = item;
        write_queue_.push_back(std::move(pending));
    }
    request_write();
}

void endpoint::start_break_write(const std::shared_ptr<pending_write>& write,
                                 std::uint64_t generation)
{
    const auto current = active_transport();
    auto self = shared_from_this();
    if (!current->can_deliver_break())
    {
        // Said once for the port, counted every time, and the queue goes on.
        boost::asio::post(self->strand_,
                          [self, write, generation]
                          {
                              if (generation != self->generation_.load())
                                  return;
                              self->on_break_written(
                                  write, generation,
                                  make_error_code(errc::unsupported));
                          });
        return;
    }
    current->async_write_break(
        *write->item,
        [self, write, generation](boost::system::error_code ec, std::size_t)
        {
            boost::asio::post(self->strand_,
                              [self, write, generation, ec]
                              {
                                  if (generation == self->generation_.load())
                                      self->on_break_written(write, generation,
                                                             ec);
                              });
        });
}

void endpoint::on_break_written(const std::shared_ptr<pending_write>& write,
                                std::uint64_t generation,
                                boost::system::error_code ec)
{
    const line_break item = *write->item;
    const bool undelivered = ec == make_error_code(errc::unsupported);
    if (undelivered)
    {
        {
            std::lock_guard lock(stats_mutex_);
            if (item.ends_break())
                stats_.dropped += 1;
        }
        if (!break_drop_logged_)
        {
            break_drop_logged_ = true;
            emit({}, "break not delivered: this port cannot show one to the "
                     "program",
                 event_kind::log_message);
        }
    }
    else if (ec)
    {
        std::lock_guard lock(stats_mutex_);
        stats_.errors += 1;
    }
    else
    {
        std::lock_guard lock(stats_mutex_);
        if (item.ends_break())
            stats_.breaks_tx += 1;
        stats_.last_activity = clock_type::now();
    }

    write_handler finished;
    {
        std::lock_guard lock(write_mutex_);
        if (generation != generation_.load())
            return;
        finished = std::exchange(write->handler, nullptr);
        if (!write_queue_.empty() && write_queue_.front() == write)
            write_queue_.pop_front();
    }
    if (!ec && options_.capture_traffic)
        emit_break(data_direction::tx, item);
    if (finished)
        boost::asio::post(strand_, [finished, ec] { finished(ec, 0); });

    start_next_write(generation);
}

result<void> endpoint::reopen(std::shared_ptr<transport> fresh)
{
    VSS_PROFILE_ZONE("endpoint.reopen");
    VSS_PROFILE_MESSAGE("endpoint.reopen");
    if (!fresh)
        return make_error_code(errc::invalid_argument);
    if (!fresh->is_open())
        return make_error_code(errc::not_open);
    if (!owner_.running())
        return make_error_code(errc::not_open);

    running_ = false;

    std::shared_ptr<transport> old;
    std::vector<write_handler> discarded;
    {
        // Everything the old transport still owes (a read, a write) is stale
        // from here on: the completions check the generation and are dropped.
        // The generation changes together with the transport and with the
        // write queue, under both locks, so whoever reads one of them sees the
        // generation that goes with it.  A write that is scheduled either
        // carries the old generation and is dropped, or finds the queue
        // already emptied and starts the one chain of the new transport.
        std::scoped_lock lock(write_mutex_, transport_mutex_);
        generation_.fetch_add(1);
        old = std::move(transport_);
        transport_ = std::move(fresh);
        // Bytes queued for the old connection are not sent down the new one.
        discarded = take_write_queue_locked();
    }
    // A transport is a single-threaded object: the read that is parked on it
    // runs on this endpoint's strand, so it is cancelled and closed there too,
    // not from the caller's thread.
    if (old)
    {
        auto self = shared_from_this();
        boost::asio::post(strand_, [self, old] {
            old->cancel();
            old->close();
        });
    }
    post_write_handlers(std::move(discarded),
                        boost::asio::error::operation_aborted);

    start();
    return state() == endpoint_state::open ? result<void>()
                                           : result<void>(make_error_code(
                                                 errc::not_open));
}

void endpoint::stop()
{
    shutdown({});
}

void endpoint::shutdown(std::function<void()> done)
{
    if (!running_.exchange(false))
    {
        if (done)
            done();
        return;
    }

    VSS_PROFILE_ZONE("endpoint.shutdown");

    auto self = shared_from_this();
    boost::asio::post(strand_, [self, done = std::move(done)] {
        VSS_PROFILE_ZONE("endpoint.shutdown.close");
        if (const auto current = self->active_transport())
        {
            current->cancel();
            current->close();
        }
        std::vector<write_handler> discarded;
        {
            std::lock_guard lock(self->write_mutex_);
            discarded = self->take_write_queue_locked();
        }
        self->transition(endpoint_state::closed);
        // Already on the strand, so the handlers are called here and not
        // posted: `done` can let the hub stop its io_context, and a handler
        // posted after that would never run.
        for (auto& handler : discarded)
            handler(boost::asio::error::operation_aborted, 0);
        if (done)
            done();
    });
}

void endpoint::close()
{
    VSS_PROFILE_ZONE("endpoint.close");
    running_ = false;
    if (const auto current = active_transport())
    {
        current->cancel();
        current->close();
    }
    transition(endpoint_state::closed);
}

void endpoint::do_read()
{
    if (!running_.load())
        return;

    VSS_PROFILE_ZONE("endpoint.arm_read");

    // The transport takes a std::function, which carries no executor, so a
    // bind_executor around the handler would be lost on the way. The completion
    // enters the strand here instead, and everything that touches the
    // transport stays serialised, as the serial library's contract asks.
    auto self = shared_from_this();
    std::shared_ptr<transport> current;
    std::uint64_t generation = 0;
    {
        // Together, as reopen() changes them together.
        std::lock_guard lock(transport_mutex_);
        current = transport_;
        generation = generation_.load();
    }

    // One read per generation.  An on_read of the transport before a reopen()
    // can still be running when start() posts the first read of the new one,
    // and both end in a do_read; two reads would share read_buffer_.
    if (read_armed_ == generation)
        return;
    read_armed_ = generation;

    current->async_read_some(
        boost::asio::buffer(read_buffer_),
        [self, generation](boost::system::error_code ec, std::size_t count) {
            boost::asio::post(self->strand_, [self, generation, ec, count] {
                // A read that belongs to a transport since replaced by
                // reopen() is no news: drop it.
                if (generation == self->generation_.load())
                {
                    self->read_armed_.reset();
                    self->on_read(generation, ec, count);
                }
            });
        });
}

void endpoint::on_read(std::uint64_t generation, boost::system::error_code ec,
                       std::size_t count)
{
    VSS_PROFILE_ZONE_NAMED(zone, "endpoint.read");
    VSS_PROFILE_ZONE_VALUE(zone, count);

    if (!running_.load())
        return;

    if (ec)
    {
        const auto current = active_transport();
        if (current && current->is_line_error(ec))
        {
            // The device saw a fault on the line.  The channel is still
            // there, so count it, say so and read on; closing it would
            // drop every frame in flight for a fault of one byte.
            on_line_error(ec);
            do_read();
            return;
        }
        handle_read_failure(generation, ec, "read");
        return;
    }

    if (count == 0)
    {
        // A transport that has control lines completes a read with no bytes
        // when only its lines changed (an RFC 2217 client set DTR).
        if (const auto current = active_transport();
            current && current->supports_control_lines())
            refresh_lines();
        do_read();
        return;
    }

    // The chunk's one time: for a stage, for a half-duplex bus and for the
    // data event.  Read before routing, so it does not include it.
    const time_point read_at = owner_.now();

    {
        std::lock_guard lock(stats_mutex_);
        stats_.bytes_rx += count;
        stats_.read_ops += 1;
        stats_.last_activity = clock_type::now();
    }

    auto chunk = std::make_shared<const byte_buffer>(
        read_buffer_.begin(), read_buffer_.begin() + static_cast<std::ptrdiff_t>(count));

    if (read_stage_)
        read_stage_->on_read(id_, *chunk, read_at);

    // Route on the I/O thread: the fast path does not wait for the event
    // dispatcher, and never copies for links with no subscriber.
    owner_.route_data(id_, *chunk, read_at);
    if (options_.capture_traffic)
        emit_data(data_direction::rx, std::move(chunk), read_at);

    do_read();
}

void endpoint::on_line_error(const boost::system::error_code& ec)
{
    {
        std::lock_guard lock(stats_mutex_);
        stats_.errors += 1;
        stats_.last_activity = clock_type::now();
    }

    using namespace std::chrono_literals;
    const time_point now = owner_.now();
    if (line_error_logged_ != time_point{} && now - line_error_logged_ < 1s)
    {
        ++line_errors_unlogged_;
        return;
    }
    std::string text = "line error: " + ec.message();
    if (line_errors_unlogged_ != 0)
    {
        text += " (and " + std::to_string(line_errors_unlogged_) +
                " more in the last second)";
    }
    line_error_logged_ = now;
    line_errors_unlogged_ = 0;
    emit(ec, std::move(text));
}

void endpoint::handle_read_failure(std::uint64_t generation,
                                   const boost::system::error_code& ec,
                                   std::string_view what)
{
    if (ec == boost::asio::error::operation_aborted)
    {
        // A cancel from stop()/close(); the state transition is handled there.
        return;
    }

    VSS_PROFILE_ZONE("endpoint.read_failure");

    // The generation is checked, and `running_` written, under the lock that
    // reopen() swaps the transport under.  A read of a transport since
    // replaced fails nothing: the endpoint is open on the new one, and
    // closing or marking it failed here would undo a reopen that succeeded.
    std::shared_ptr<transport> failed;
    {
        std::lock_guard lock(transport_mutex_);
        if (generation != generation_.load())
            return;
        running_ = false;
        failed = transport_;
    }
    {
        std::lock_guard lock(stats_mutex_);
        stats_.errors += 1;
        stats_.last_activity = clock_type::now();
    }

    const bool graceful = ec == boost::asio::error::eof
        || ec == boost::asio::error::connection_reset
        || ec == boost::asio::error::broken_pipe;

    // The transport this read belongs to, not whatever is active by now.
    if (failed)
        failed->close();

    // A reopen() that runs in full between the check above and the transitions
    // below leaves the state label wrong (failed or closed) while the endpoint
    // works; it takes the swap and the transitions on one strand to close that.
    if (graceful)
    {
        transition(endpoint_state::closed);
        emit(ec, std::string(what) + ": peer closed the connection",
             event_kind::stopped);
    }
    else
    {
        transition(endpoint_state::failed);
        emit(ec, std::string(what) + " failed");
    }
}

std::vector<write_handler> endpoint::take_write_queue_locked()
{
    std::vector<write_handler> handlers;
    handlers.reserve(write_queue_.size());
    for (const auto& pending : write_queue_)
    {
        // std::exchange and not std::move: a std::function that was moved
        // from need not be empty (libc++ leaves a small callable in place),
        // and a handler left behind here would be called a second time.
        if (pending->handler)
            handlers.push_back(std::exchange(pending->handler, nullptr));
    }
    write_queue_.clear();
    write_queue_bytes_ = 0;
    writing_ = false;
    return handlers;
}

void endpoint::post_write_handlers(std::vector<write_handler> handlers,
                                   boost::system::error_code ec)
{
    for (auto& handler : handlers)
    {
        boost::asio::post(strand_, [handler = std::move(handler), ec]
                          { handler(ec, 0); });
    }
}

void endpoint::request_write()
{
    bool start = false;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(write_mutex_);
        if (!writing_)
        {
            writing_ = true;
            start = true;
            // Read in the lock that sets `writing_`, as reopen() changes both
            // in it: a start that carries the old generation is dropped, and
            // one that carries the new generation is the only one for it.
            generation = generation_.load();
        }
    }
    if (!start)
        return;

    // A start posted for a transport since replaced would run beside the one
    // reopen() has made room for, and send the same bytes twice.
    auto self = shared_from_this();
    boost::asio::post(strand_, [self, generation] {
        if (generation == self->generation_.load())
            self->start_next_write(generation);
    });
}

void endpoint::start_next_write(std::uint64_t generation)
{
    VSS_PROFILE_ZONE("endpoint.arm_write");
    std::shared_ptr<pending_write> next;
    {
        std::lock_guard lock(write_mutex_);
        // `writing_` and the queue are those of the generation now, not ours.
        if (generation != generation_.load())
            return;
        if (write_queue_.empty())
        {
            writing_ = false;
            return;
        }
        next = write_queue_.front();
    }

    const auto current = active_transport();
    if (!current || !current->is_open())
    {
        std::vector<write_handler> discarded;
        {
            std::lock_guard lock(write_mutex_);
            // The queue of a later generation is not ours to empty.
            if (generation != generation_.load())
                return;
            discarded = take_write_queue_locked();
        }
        post_write_handlers(std::move(discarded),
                            boost::asio::error::operation_aborted);
        return;
    }

    // reopen() bumps the generation before it swaps the transport and empties
    // the queue.  The head of the queue and the transport above were read
    // after the check that scheduled this write, so a reopen() that began in
    // between could leave us holding bytes for the old connection and the new
    // transport.  Checking again, now both are in hand, closes that window: if
    // the generation still matches, the swap had not begun when they were
    // read, so they belong together, and the completion is judged by it too.
    if (generation != generation_.load())
        return;

    // A break waits its turn in the queue, between the bytes written before
    // and after it.
    if (next->item)
    {
        start_break_write(next, generation);
        return;
    }

    auto self = shared_from_this();
    // As in do_read: the std::function loses an executor binding, so the
    // completion enters the strand explicitly.
    current->async_write_some(
        boost::asio::buffer(*next->data) + next->offset,
        [self, next, generation](boost::system::error_code ec,
                                 std::size_t count) {
            boost::asio::post(self->strand_, [self, next, generation, ec, count] {
                if (generation == self->generation_.load())
                    self->on_write(next, generation, ec, count);
            });
        });
}

void endpoint::on_write(const std::shared_ptr<pending_write>& write,
                        std::uint64_t generation, boost::system::error_code ec,
                        std::size_t count)
{
    VSS_PROFILE_ZONE_NAMED(zone, "endpoint.write");
    VSS_PROFILE_ZONE_VALUE(zone, count);

    if (ec)
    {
        // The write that failed gets the error; those behind it are abandoned.
        write_handler failed;
        std::vector<write_handler> abandoned;
        {
            std::lock_guard lock(write_mutex_);
            // A reopen() since the check that brought us here has taken this
            // write, and its handler, with the queue.
            if (generation != generation_.load())
                return;
            // Emptied by hand, as in take_write_queue_locked(): this write
            // is still in the queue that is taken next.
            failed = std::exchange(write->handler, nullptr);
            abandoned = take_write_queue_locked();
        }
        if (failed)
            boost::asio::post(strand_, [failed, ec] { failed(ec, 0); });
        post_write_handlers(std::move(abandoned),
                            boost::asio::error::operation_aborted);
        {
            std::lock_guard stats_lock(stats_mutex_);
            stats_.errors += 1;
        }
        emit(ec, "write failed");
        return;
    }

    write->offset += count;

    {
        std::lock_guard lock(stats_mutex_);
        stats_.bytes_tx += count;
        stats_.write_ops += 1;
        stats_.last_activity = clock_type::now();
    }

    const bool complete = write->offset >= write->data->size();

    if (complete)
    {
        write_handler finished;
        {
            std::lock_guard lock(write_mutex_);
            if (generation != generation_.load())
                return;
            finished = std::exchange(write->handler, nullptr);
            if (!write_queue_.empty() && write_queue_.front() == write)
                write_queue_.pop_front();
            write_queue_bytes_ -= std::min(write_queue_bytes_, write->data->size());
            VSS_PROFILE_PLOT("endpoint.write_queue", write_queue_bytes_);
        }
        if (options_.capture_traffic)
        {
            auto done = std::shared_ptr<const byte_buffer>(write->data);
            emit_data(data_direction::tx, std::move(done), owner_.now());
        }
        if (finished)
        {
            const std::size_t total = write->data->size();
            boost::asio::post(strand_,
                              [finished, total] { finished({}, total); });
        }
    }

    start_next_write(generation);
}

void endpoint::write(bytes_view data, write_handler handler)
{
    write(byte_buffer(data.begin(), data.end()), std::move(handler));
}

void endpoint::write(byte_buffer data, write_handler handler)
{
    VSS_PROFILE_ZONE("endpoint.enqueue_write");
    if (data.empty())
    {
        if (handler)
        {
            boost::asio::post(strand_, [handler] { handler({}, 0); });
        }
        return;
    }

    auto chunk = std::make_shared<byte_buffer>(std::move(data));
    const std::size_t size = chunk->size();

    {
        std::lock_guard lock(write_mutex_);
        if (write_queue_bytes_ + size > options_.max_write_queue_bytes)
        {
            {
                std::lock_guard stats_lock(stats_mutex_);
                stats_.dropped += size;
            }
            VSS_PROFILE_MESSAGE("endpoint.write_queue_full");
            if (handler)
            {
                auto rejected = std::move(handler);
                boost::asio::post(strand_, [rejected] {
                    rejected(make_error_code(errc::queue_full), 0);
                });
            }
            emit(make_error_code(errc::queue_full), "write queue full");
            return;
        }

        auto pending = std::make_shared<pending_write>();
        pending->data = std::move(chunk);
        pending->handler = std::move(handler);
        write_queue_.push_back(std::move(pending));
        write_queue_bytes_ += size;
        VSS_PROFILE_PLOT("endpoint.write_queue", write_queue_bytes_);
    }

    request_write();
}

result<std::size_t> endpoint::write_sync(bytes_view data,
                                         std::chrono::milliseconds timeout)
{
    VSS_PROFILE_ZONE("endpoint.write_sync");
    auto promise = std::make_shared<std::promise<std::pair<error_code, std::size_t>>>();
    auto future = promise->get_future();

    write(data, [promise](boost::system::error_code ec, std::size_t count) {
        promise->set_value({ec, count});
    });

    if (timeout.count() <= 0)
    {
        const auto [ec, count] = future.get();
        if (ec)
            return ec;
        return count;
    }

    if (future.wait_for(timeout) != std::future_status::ready)
        return make_error_code(errc::timeout);

    const auto [ec, count] = future.get();
    if (ec)
        return ec;
    return count;
}

control_lines endpoint::lines() const
{
    std::lock_guard lock(lines_mutex_);
    return lines_;
}

std::optional<bool> endpoint::peer_rts() const
{
    const auto current = active_transport();
    if (!current || !current->reports_peer_rts())
        return std::nullopt;
    return lines().rts();
}

void endpoint::refresh_lines()
{
    VSS_PROFILE_ZONE("endpoint.refresh_lines");
    const auto current = active_transport();
    if (!current)
        return;

    current->refresh_lines();
    control_lines updated;
    {
        std::lock_guard lock(lines_mutex_);
        const control_lines before = lines_;
        lines_ = current->lines();
        updated = lines_;
        if (updated == before)
            return;
    }
    emit_lines();
    owner_.route_lines(id_, updated);
}

error_code endpoint::set_line(control_line line, bool state)
{
    VSS_PROFILE_ZONE("endpoint.set_line");
    const auto current = active_transport();
    if (!current)
        return make_error_code(errc::not_open);
    if (!current->supports_control_lines())
        return make_error_code(errc::unsupported);

    current->set_control_line(line, state);

    control_lines snapshot;
    {
        std::lock_guard lock(lines_mutex_);
        lines_.set(line, state);
        snapshot = lines_;
    }
    emit_lines();
    owner_.route_lines(id_, snapshot);
    return {};
}

void endpoint::set_peer_inputs(control_lines inputs)
{
    control_lines updated;
    {
        std::lock_guard lock(lines_mutex_);
        updated = with_peer_inputs(lines_, inputs);
        if (updated == lines_)
            return;
        lines_ = updated;
    }

    if (const auto current = active_transport();
        current && current->supports_control_lines())
    {
        current->set_control_line(control_line::cts, updated.cts());
        current->set_control_line(control_line::dsr, updated.dsr());
        current->set_control_line(control_line::dcd, updated.dcd());
    }

    emit_lines();
}

endpoint_stats endpoint::stats() const
{
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

void endpoint::reset_stats()
{
    std::lock_guard lock(stats_mutex_);
    const auto started = stats_.started;
    stats_ = endpoint_stats{};
    stats_.started = started;
}

std::string endpoint::name() const
{
    std::lock_guard lock(name_mutex_);
    return name_;
}

void endpoint::set_name(std::string name)
{
    std::lock_guard lock(name_mutex_);
    name_ = std::move(name);
}

endpoint_info endpoint::info() const
{
    endpoint_info out;
    out.id = id_;
    out.name = name();
    out.kind = kind_;
    out.state = state();
    out.path = public_path();
    out.description = description();
    out.serial = options_.serial;
    out.lines = lines();
    out.stats = stats();
    const auto current = active_transport();
    out.supports_control_lines = current && current->supports_control_lines();
    out.open = is_open();
    if (current)
    {
        out.peer_format = current->peer_format();
        out.clients = current->client_count();
    }
    return out;
}

void endpoint::transition(endpoint_state next)
{
    const endpoint_state previous = state_.exchange(next);
    if (previous == next)
        return;

    VSS_PROFILE_ZONE("endpoint.transition");
    VSS_PROFILE_MESSAGE_TEXT(std::string("endpoint.") +
                             std::string(to_string(next)) + ' ' + name());

    endpoint_event event;
    event.kind = next == endpoint_state::open ? event_kind::started
                                               : event_kind::stopped;
    event.endpoint = id_;
    event.name = name();
    event.when = owner_.now();
    event.message = std::string(to_string(previous)) + " -> "
        + to_string(next);
    owner_.publish(std::move(event));
}

void endpoint::emit(error_code ec, std::string message, event_kind kind)
{
    endpoint_event event;
    event.kind = kind;
    event.endpoint = id_;
    event.name = name();
    event.error = ec;
    event.message = std::move(message);
    event.when = owner_.now();
    owner_.publish(std::move(event));
}

void endpoint::set_read_stage(std::shared_ptr<read_stage> stage,
                              std::function<void()> done)
{
    boost::asio::post(
        strand_, [self = shared_from_this(), stage = std::move(stage),
                  done = std::move(done)]() mutable
        {
            // The old stage goes when `stage` does, after `done`.
            self->read_stage_.swap(stage);
            if (done)
                done();
        });
}

void endpoint::emit_data(data_direction direction,
                         std::shared_ptr<const byte_buffer> data,
                         time_point when)
{
    endpoint_event event;
    event.kind = event_kind::data;
    event.endpoint = id_;
    event.name = name();
    event.direction = direction;
    event.data = std::move(data);
    event.when = when;
    owner_.publish(std::move(event));
}

void endpoint::emit_break(data_direction direction, const line_break& item)
{
    endpoint_event event;
    event.kind = event_kind::line_break;
    event.endpoint = id_;
    event.name = name();
    event.direction = direction;
    event.when = item.when();
    event.phase = item.phase;
    event.start = item.start;
    event.duration = item.duration;
    event.measured = item.measured;
    owner_.publish(std::move(event));
}

void endpoint::emit_lines()
{
    endpoint_event event;
    event.kind = event_kind::lines;
    event.endpoint = id_;
    event.name = name();
    event.lines = lines();
    event.when = owner_.now();
    owner_.publish(std::move(event));
}

} // namespace vss
