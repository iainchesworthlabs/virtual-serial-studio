//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/client.hpp>

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>

#include <boost/asio/io_context.hpp>

#include <deque>
#include <functional>
#include <utility>
#include <variant>

namespace vss::agent
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
        auto connected = connect_local(io_.get_executor(), options_.instance);
        if (!connected)
            return connected.error();
        stream_ = std::move(*connected);
        return {};
    }

    const client_options& options() const noexcept { return options_; }

    /// Sends a request and waits for its reply.  An error reply comes back
    /// as the error it carries.
    result<reply_body> call(request_body body)
    {
        if (broken_)
            return make_error_code(protocol_errc::connection_closed);
        request asked;
        asked.id = next_id_++;
        asked.body = std::move(body);
        const auto deadline = steady::now() + options_.timeout;

        const error_code unsent = send(encode(asked), deadline);
        if (unsent == errc::timeout)
        {
            drop();
            return unsent;
        }
        // When the write failed the agent may have said why before it closed
        // the connection (`busy`), so what it sent is still read.
        for (;;)
        {
            auto message = receive(deadline);
            if (!message)
            {
                drop();
                return unsent
                           ? make_error_code(protocol_errc::connection_closed)
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
                return from_wire(refused->error);
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
                return make_error_code(protocol_errc::connection_closed);
            auto message = receive(deadline);
            if (!message)
            {
                // Nothing came in time: not a fault of the connection.
                if (message.error() != errc::timeout)
                    drop();
                return message.error();
            }
            if (auto* batch = std::get_if<event_batch>(&*message))
                keep(std::move(*batch));
        }
    }

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
        auto encoded = encode_frame(value);
        if (!encoded)
            return encoded.error();
        // Shared with the handler: a write that is given up on may still be
        // looking at the bytes when this function has returned.
        const auto bytes =
            std::make_shared<const byte_buffer>(std::move(*encoded));
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
                return make_error_code(protocol_errc::connection_closed);
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

    /// The next message from the agent.  `errc::timeout` leaves the read in
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
                return make_error_code(protocol_errc::connection_closed);
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
    std::unique_ptr<local_stream> stream_;
    frame_reader reader_;
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
    greeting.protocol = protocol_version;
    greeting.client = options.name;
    greeting.read_only = options.read_only;
    auto answer = state->call(std::move(greeting));
    if (!answer)
        return answer.error();
    auto* welcome = std::get_if<hello_reply>(&*answer);
    if (welcome == nullptr)
        return make_error_code(protocol_errc::malformed_message);
    if (welcome->protocol != protocol_version)
        return make_error_code(protocol_errc::version_mismatch);
    // An agent that does not say it will hold the connection to looking
    // would let it change things: that is not the connection asked for.
    if (options.read_only && !welcome->read_only)
        return make_error_code(protocol_errc::version_mismatch);
    state->hello = std::move(*welcome);
    return std::unique_ptr<client>(new client(std::move(state)));
}

const hello_reply& client::agent() const noexcept
{
    return impl_->hello;
}

result<list_reply> client::list()
{
    auto answer = impl_->call(list_request{});
    if (!answer)
        return answer.error();
    auto* listed = std::get_if<list_reply>(&*answer);
    if (listed == nullptr)
        return make_error_code(protocol_errc::malformed_message);
    return std::move(*listed);
}

result<endpoint_id> client::create_port(const std::string& name,
                                        const std::string& kind)
{
    auto answer = impl_->call(create_port_request{name, kind});
    if (!answer)
        return answer.error();
    const auto* created = std::get_if<create_port_reply>(&*answer);
    if (created == nullptr)
        return make_error_code(protocol_errc::malformed_message);
    return created->endpoint;
}

result<void> client::remove_port(endpoint_id endpoint)
{
    const auto answer = impl_->call(remove_port_request{endpoint});
    if (!answer)
        return answer.error();
    return {};
}

result<link_id> client::connect_ports(endpoint_id first, endpoint_id second,
                                      bool one_way)
{
    auto answer = impl_->call(connect_request{first, second, one_way});
    if (!answer)
        return answer.error();
    const auto* linked = std::get_if<connect_reply>(&*answer);
    if (linked == nullptr)
        return make_error_code(protocol_errc::malformed_message);
    return linked->link;
}

result<void> client::write(endpoint_id endpoint, bytes_view data)
{
    write_request body;
    body.endpoint = endpoint;
    body.data.assign(data.begin(), data.end());
    const auto answer = impl_->call(std::move(body));
    if (!answer)
        return answer.error();
    return {};
}

result<void> client::subscribe(bool data, std::uint32_t interval_ms)
{
    const auto answer = impl_->call(subscribe_request{data, interval_ms});
    if (!answer)
        return answer.error();
    return {};
}

result<void> client::shutdown()
{
    const auto answer = impl_->call(shutdown_request{});
    if (!answer)
        return answer.error();
    return {};
}

result<event_batch> client::next_events(std::chrono::milliseconds limit)
{
    return impl_->next_events(limit);
}

const std::string& client::last_error_message() const noexcept
{
    return impl_->last_message;
}

} // namespace vss::agent
