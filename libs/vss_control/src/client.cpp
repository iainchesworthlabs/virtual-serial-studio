//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/client.hpp>

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/error.hpp>
#include <vss/agent/proto/frame.hpp>

#include <boost/asio/io_context.hpp>

#include <functional>
#include <utility>
#include <variant>

namespace vss::control
{
namespace
{

using steady = std::chrono::steady_clock;

constexpr std::size_t read_buffer_size = std::size_t{64} * 1024;

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
    result<reply_body> call(request_body body)
    {
        if (broken_)
            return agent::make_error_code(
                agent::protocol_errc::connection_closed);
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
            reply& answer = *message;
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

    hello_reply hello;
    std::string last_message;

private:
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
                return false;
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
    result<reply> receive(steady::time_point deadline)
    {
        for (;;)
        {
            auto next = reader_.next();
            if (!next)
                return next.error();
            const std::optional<frame> ready = std::move(*next);
            if (ready)
                return decode_reply(*ready);
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
    auto answer = state->call(std::move(greeting));
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

result<void> client::stop()
{
    auto answer = impl_->call(endpoint_stop_request{});
    if (!answer)
        return answer.error();
    if (!std::holds_alternative<done_reply>(*answer))
        return make_error_code(api_errc::invalid_argument);
    return {};
}

const std::string& client::last_error_message() const noexcept
{
    return impl_->last_message;
}

} // namespace vss::control
