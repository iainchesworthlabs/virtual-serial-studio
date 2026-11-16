//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test plugin as a plugin.  It says "hello", answers what the host asks
// as a plugin does, and does what its modifiers say wrong.  It reads and
// writes one message at a time and has no thread.
//

#include "serve.hpp"

#include "platform.hpp"

#include <vss/agent/proto/frame.hpp>
#include <vss/core/bytes.hpp>
#include <vss/plugin/protocol.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace test_plugin
{

namespace
{

using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::bye_message;
using vss::plugin::decode_message;
using vss::plugin::encode_message;
using vss::plugin::error_message;
using vss::plugin::hello_ack_message;
using vss::plugin::hello_message;
using vss::plugin::log_message;
using vss::plugin::make_error;
using vss::plugin::message_body;
using vss::plugin::ok_message;
using vss::plugin::other_message;
using vss::plugin::ping_message;
using vss::plugin::plugin_api_version;
using vss::plugin::pong_message;
using vss::plugin::shutdown_message;
using vss::plugin::status_level;
using vss::plugin::status_message;

constexpr int exit_channel_ended = 2;
constexpr int exit_no_channel = 3;
constexpr int exit_write_failed = 4;

/// The id of the request that `--send-op` makes.
constexpr std::uint64_t own_request_id = 2;

/// The id of the first request that `--send-header` makes; the next ones
/// count up from it.
constexpr std::uint64_t first_header_id = 3;

/// How many bytes of garbage `--garbage-after-ready` writes.
constexpr std::size_t garbage_size = 64;

/// How many messages `--flood-after-ready` writes with one call.
constexpr std::uint32_t flood_batch = 256;

/// One line, flushed at once, so that the host sees it as it happens.
void print_line(std::FILE* stream, std::string_view text)
{
    std::string line(text);
    line.push_back('\n');
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), stream));
    static_cast<void>(std::fflush(stream));
}

std::optional<std::string_view> value_after(std::string_view argument,
                                            std::string_view name)
{
    if (!argument.starts_with(name))
        return std::nullopt;
    return argument.substr(name.size());
}

template <class Number>
std::optional<Number> parse_number(std::string_view text)
{
    Number value{};
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

api_message make_message(message_body body, std::uint64_t id = 0,
                         std::uint64_t re = 0)
{
    api_message message;
    message.id = id;
    message.re = re;
    message.body = std::move(body);
    return message;
}

/// A request of an extension point: an "op" this code does not know and
/// nothing else.
api_message request_of(std::string_view op, std::uint64_t id)
{
    std::string header = R"({"op":")";
    header.append(op);
    header.append(R"(","id":)");
    header.append(std::to_string(id));
    header.push_back('}');

    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.body = other_message{std::move(header)};
    return message;
}

/// A request whose header is `text`, a JSON object that holds an "op", with
/// the "id" put in front.  Nothing when the result is not a message.
std::optional<api_message> request_with_header(std::string_view text,
                                               std::uint64_t id)
{
    const std::size_t open = text.find('{');
    if (open == std::string_view::npos)
        return std::nullopt;
    std::string_view rest = text.substr(open + 1);
    const std::size_t next = rest.find_first_not_of(" \t\r\n");
    const bool empty_object =
        next != std::string_view::npos && rest[next] == '}';
    std::string header = R"({"id":)";
    header.append(std::to_string(id));
    if (!empty_object)
        header.push_back(',');
    header.append(rest);

    vss::agent::frame frame;
    frame.header = std::move(header);
    auto message = decode_message(frame);
    if (message.has_error())
        return std::nullopt;
    return std::move(message).value();
}

/// The bytes of a message, or nothing when it cannot be written.
std::optional<vss::byte_buffer> bytes_of(const api_message& message)
{
    const auto frame = encode_message(message);
    if (frame.has_error())
        return std::nullopt;
    auto bytes = vss::agent::encode_frame(frame.value());
    if (bytes.has_error())
        return std::nullopt;
    return std::move(bytes).value();
}

/// The channel, a message at a time.
class channel_io
{
public:
    explicit channel_io(std::intptr_t handle) : handle_(handle) {}

    bool send(const api_message& message)
    {
        const std::optional<vss::byte_buffer> bytes = bytes_of(message);
        return bytes.has_value() && write_bytes(*bytes);
    }

    bool write_bytes(const vss::byte_buffer& bytes)
    {
        // The channel takes characters; the bytes are the same.
        return write_handle(
            handle_, reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    /// The next message of the host.  Nothing when the channel ended or
    /// brought something that is not a message.
    std::optional<api_message> receive()
    {
        for (;;)
        {
            auto next = reader_.next();
            if (next.has_error())
                return std::nullopt;
            const std::optional<vss::agent::frame>& frame = next.value();
            if (frame.has_value())
            {
                auto message = decode_message(frame.value());
                if (message.has_error())
                    return std::nullopt;
                return std::move(message).value();
            }
            const long long got =
                read_handle(handle_, chunk_.data(), chunk_.size());
            if (got <= 0)
                return std::nullopt;
            reader_.feed(vss::bytes_view(
                reinterpret_cast<const vss::byte*>(chunk_.data()),
                static_cast<std::size_t>(got)));
        }
    }

private:
    std::intptr_t handle_;
    vss::agent::frame_reader reader_;
    std::array<char, 4096> chunk_{};
};

class server
{
public:
    server(const serve_options& options, std::intptr_t handle)
        : options_(options), channel_(handle)
    {
    }

    int run()
    {
        if (!options_.no_hello)
        {
            if (const std::optional<int> code = say_hello())
                return *code;
        }
        for (;;)
        {
            const std::optional<api_message> message = channel_.receive();
            if (!message.has_value())
                return exit_channel_ended;
            if (const std::optional<int> code = take(*message))
                return *code;
        }
    }

private:
    std::optional<int> say_hello()
    {
        const std::uint64_t count =
            options_.hello_twice ? std::uint64_t{2} : std::uint64_t{1};
        for (std::uint64_t id = 1; id <= count; ++id)
        {
            hello_message body;
            body.api = plugin_api_version;
            body.plugin_id = options_.hello_id;
            body.version = options_.hello_version;
            body.pid = static_cast<std::uint64_t>(process_id());
            if (!channel_.send(make_message(std::move(body), id)))
                return exit_write_failed;
        }
        return std::nullopt;
    }

    /// What the plugin does at once when "hello_ack" arrives.  A value is
    /// the exit code.
    std::optional<int> after_ready()
    {
        if (options_.exit_after_ready.has_value())
            return options_.exit_after_ready;
        if (options_.crash_after_ready)
        {
            crash();
            return 1;
        }
        if (options_.stderr_each.has_value())
            print_line(stderr, *options_.stderr_each);
        if (options_.status.has_value())
        {
            status_message body;
            body.level = status_level::warning;
            body.text = *options_.status;
            if (!channel_.send(make_message(std::move(body))))
                return exit_write_failed;
        }
        if (options_.log.has_value())
        {
            log_message body;
            body.text = *options_.log;
            if (!channel_.send(make_message(std::move(body))))
                return exit_write_failed;
        }
        if (options_.send_op.has_value())
        {
            if (!channel_.send(request_of(*options_.send_op, own_request_id)))
                return exit_write_failed;
        }
        std::uint64_t header_id = first_header_id;
        for (const std::string& header : options_.send_headers)
        {
            const std::optional<api_message> request =
                request_with_header(header, header_id);
            if (!request.has_value() || !channel_.send(*request))
                return exit_write_failed;
            ++header_id;
        }
        if (options_.garbage_after_ready)
        {
            // A length prefix that no frame may have, then more of the same.
            const vss::byte_buffer garbage(garbage_size, vss::byte{0xff});
            if (!channel_.write_bytes(garbage))
                return exit_write_failed;
        }
        if (options_.flood_after_ready.has_value())
            return flood(*options_.flood_after_ready);
        return std::nullopt;
    }

    /// `count` "log" messages, a batch to each write.
    std::optional<int> flood(std::uint32_t count)
    {
        vss::byte_buffer batch;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            log_message body;
            body.text = "flood ";
            body.text.append(std::to_string(index));
            const std::optional<vss::byte_buffer> bytes =
                bytes_of(make_message(std::move(body)));
            if (!bytes.has_value())
                return exit_write_failed;
            batch.insert(batch.end(), bytes->begin(), bytes->end());
            const bool batch_full = (index % flood_batch) == flood_batch - 1;
            if (batch_full || index + 1 == count)
            {
                if (!channel_.write_bytes(batch))
                    return exit_write_failed;
                batch.clear();
            }
        }
        return std::nullopt;
    }

    /// What one message of the host asks.  A value is the exit code.
    std::optional<int> take(const api_message& message)
    {
        if (std::holds_alternative<hello_ack_message>(message.body))
        {
            if (ready_)
                return std::nullopt;
            ready_ = true;
            return after_ready();
        }
        if (std::holds_alternative<ping_message>(message.body))
        {
            if (options_.ignore_ping || message.id == 0)
                return std::nullopt;
            if (!channel_.send(make_message(pong_message{}, 0, message.id)))
                return exit_write_failed;
            return std::nullopt;
        }
        if (std::holds_alternative<shutdown_message>(message.body))
        {
            if (options_.ignore_shutdown)
                return std::nullopt;
            if (!channel_.send(make_message(bye_message{}, 0, message.id)))
                return exit_write_failed;
            return 0;
        }
        if (message.re != 0)
        {
            if (is_own_request(message.re))
                print_answer(message);
            return std::nullopt;
        }
        if (message.id != 0 &&
            std::holds_alternative<other_message>(message.body))
            return take_request(message);
        return std::nullopt;
    }

    /// Whether `re` is the id of a request that `--send-op` or
    /// `--send-header` made.
    bool is_own_request(std::uint64_t re) const
    {
        if (options_.send_op.has_value() && re == own_request_id)
            return true;
        return re >= first_header_id &&
               re - first_header_id < options_.send_headers.size();
    }

    /// A request of an extension point: "ok" when its op is one that is
    /// echoed, "refused" when it is one that is refused, "unknown_op"
    /// otherwise.
    std::optional<int> take_request(const api_message& message)
    {
        if (std::ranges::find(options_.refuse_ops, message.op) !=
            options_.refuse_ops.end())
        {
            std::string line = "got ";
            line.append(message.op);
            print_line(stdout, line);
            const api_message refusal = make_error(
                message.id, api_errc::refused, "refused by the test plugin");
            if (!channel_.send(refusal))
                return exit_write_failed;
            return std::nullopt;
        }
        const bool echoed = std::ranges::find(options_.echo_ops, message.op) !=
                            options_.echo_ops.end();
        if (echoed)
        {
            std::string line = "got ";
            line.append(message.op);
            print_line(stdout, line);
            if (!channel_.send(make_message(ok_message{}, 0, message.id)))
                return exit_write_failed;
            return std::nullopt;
        }
        const api_message refusal =
            make_error(message.id, api_errc::unknown_op,
                       "the test plugin does not know this op");
        if (!channel_.send(refusal))
            return exit_write_failed;
        return std::nullopt;
    }

    static void print_answer(const api_message& message)
    {
        std::string line = "answer ";
        line.append(message.op);
        print_line(stdout, line);
        if (const auto* const error = std::get_if<error_message>(&message.body))
        {
            std::string value = "answer-value ";
            value.append(error->value);
            print_line(stdout, value);
        }
    }

    const serve_options& options_;
    channel_io channel_;
    bool ready_ = false;
};

} // namespace

bool take_serve_modifier(std::string_view argument, serve_options& options)
{
    if (const auto text = value_after(argument, "--hello-id="))
    {
        options.hello_id = std::string(*text);
        return true;
    }
    if (const auto text = value_after(argument, "--hello-version="))
    {
        options.hello_version = std::string(*text);
        return true;
    }
    if (argument == "--no-hello")
    {
        options.no_hello = true;
        return true;
    }
    if (argument == "--hello-twice")
    {
        options.hello_twice = true;
        return true;
    }
    if (const auto text = value_after(argument, "--exit-after-ready="))
    {
        if (const auto code = parse_number<int>(*text))
        {
            options.exit_after_ready = code;
            return true;
        }
        return false;
    }
    if (argument == "--crash-after-ready")
    {
        options.crash_after_ready = true;
        return true;
    }
    if (argument == "--ignore-shutdown")
    {
        options.ignore_shutdown = true;
        return true;
    }
    if (argument == "--ignore-ping")
    {
        options.ignore_ping = true;
        return true;
    }
    if (const auto text = value_after(argument, "--status="))
    {
        options.status = std::string(*text);
        return true;
    }
    if (const auto text = value_after(argument, "--log="))
    {
        options.log = std::string(*text);
        return true;
    }
    if (const auto text = value_after(argument, "--send-op="))
    {
        options.send_op = std::string(*text);
        return true;
    }
    if (const auto text = value_after(argument, "--send-header="))
    {
        const std::size_t first = text->find_first_not_of(" \t\r\n");
        const std::size_t last = text->find_last_not_of(" \t\r\n");
        if (first == std::string_view::npos || (*text)[first] != '{' ||
            (*text)[last] != '}')
            return false;
        options.send_headers.emplace_back(*text);
        return true;
    }
    if (const auto text = value_after(argument, "--echo-op="))
    {
        options.echo_ops.emplace_back(*text);
        return true;
    }
    if (const auto text = value_after(argument, "--refuse-op="))
    {
        options.refuse_ops.emplace_back(*text);
        return true;
    }
    if (argument == "--garbage-after-ready")
    {
        options.garbage_after_ready = true;
        return true;
    }
    if (const auto text = value_after(argument, "--flood-after-ready="))
    {
        if (const auto count = parse_number<std::uint32_t>(*text))
        {
            options.flood_after_ready = count;
            return true;
        }
        return false;
    }
    if (const auto text = value_after(argument, "--stderr-each="))
    {
        options.stderr_each = std::string(*text);
        return true;
    }
    return false;
}

int serve(const serve_options& options)
{
    const std::optional<std::intptr_t> handle = channel_handle();
    if (!handle.has_value())
    {
        print_line(stdout, "no-channel");
        return exit_no_channel;
    }
    server plugin(options, *handle);
    return plugin.run();
}

} // namespace test_plugin
