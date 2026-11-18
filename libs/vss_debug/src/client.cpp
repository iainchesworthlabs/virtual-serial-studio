//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/client.hpp>

#include <vss/agent/local_stream.hpp>
#include <vss/agent/proto/frame.hpp>
#include <vss/control/audit_log.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <iterator>
#include <optional>

namespace vss::debug
{
namespace json = boost::json;
namespace agent = vss::agent;

result<std::filesystem::path> default_instance_directory()
{
    auto control = control::default_audit_directory();
    if (!control)
        return control.error();
    return control->parent_path() / "debug" / "instances";
}

result<std::filesystem::path> default_audit_directory()
{
    auto control = control::default_audit_directory();
    if (!control)
        return control.error();
    return control->parent_path() / "debug" / "audit";
}

std::vector<instance_info>
find_instances(const std::filesystem::path& directory)
{
    std::vector<instance_info> out;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(directory, ec))
    {
        if (item.path().extension() != ".json")
            continue;
        std::ifstream in(item.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        boost::system::error_code parse_ec;
        const json::value body = json::parse(text, parse_ec);
        if (parse_ec || !body.is_object())
            continue;
        const auto& object = body.as_object();
        instance_info found;
        if (const auto* name = object.if_contains("instance");
            name != nullptr && name->is_string())
            found.instance = std::string(name->as_string());
        if (const auto* pid = object.if_contains("process");
            pid != nullptr && pid->is_number())
            found.process_id = pid->to_number<std::uint64_t>();
        if (const auto* where = object.if_contains("address");
            where != nullptr && where->is_string())
            found.address = std::string(where->as_string());
        if (!found.instance.empty() &&
            agent::valid_instance_name(found.instance))
            out.push_back(std::move(found));
    }
    std::ranges::sort(out, {}, &instance_info::process_id);
    return out;
}

class client::impl
{
public:
    explicit impl(std::chrono::milliseconds timeout) : timeout_(timeout) {}

    result<std::optional<agent::frame>>
    read_frame(std::chrono::steady_clock::time_point deadline)
    {
        for (;;)
        {
            auto next = reader_.next();
            if (!next)
                return next.error();
            if (*next)
                return std::move(*next);

            if (std::chrono::steady_clock::now() >= deadline)
                return std::optional<agent::frame>{};

            if (!reading_)
            {
                reading_ = true;
                stream_->async_read_some(
                    boost::asio::buffer(chunk_),
                    [this](const boost::system::error_code& ec, std::size_t n)
                    {
                        reading_ = false;
                        read_ec_ = ec;
                        read_size_ = n;
                        completed_ = true;
                    });
            }
            io_.restart();
            io_.run_until(deadline);
            if (completed_)
            {
                completed_ = false;
                if (read_ec_)
                    return make_error_code(
                        boost::system::errc::connection_reset);
                reader_.feed(bytes_view(chunk_.data(), read_size_));
            }
        }
    }

    result<void> write_frame(const agent::frame& value)
    {
        auto bytes = agent::encode_frame(value);
        if (!bytes)
            return bytes.error();
        std::size_t done = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout_;
        while (done < bytes->size())
        {
            boost::system::error_code failed;
            std::size_t wrote = 0;
            bool finished = false;
            stream_->async_write_some(
                boost::asio::buffer(bytes->data() + done, bytes->size() - done),
                [&](const boost::system::error_code& ec, std::size_t n)
                {
                    failed = ec;
                    wrote = n;
                    finished = true;
                });
            io_.restart();
            io_.run_until(deadline);
            if (!finished || failed)
                return make_error_code(boost::system::errc::broken_pipe);
            done += wrote;
        }
        return {};
    }

    boost::asio::io_context io_;
    std::unique_ptr<agent::local_stream> stream_;
    agent::frame_reader reader_;
    std::array<byte, 16 * 1024> chunk_{};
    bool reading_ = false;
    boost::system::error_code read_ec_;
    std::size_t read_size_ = 0;
    bool completed_ = false;
    std::chrono::milliseconds timeout_;
    std::int64_t next_id_ = 1;
    scope_set granted_ = 0;
    std::deque<std::string> events_;
};

result<std::unique_ptr<client>> client::connect(const client_options& options)
{
    auto state = std::make_unique<impl>(options.timeout);
    auto stream =
        agent::connect_local(state->io_.get_executor(),
                             agent::local_endpoint::debug, options.instance);
    if (!stream)
        return stream.error();
    state->stream_ = std::move(stream).value();

    json::object hello;
    hello["id"] = 0;
    hello["op"] = "hello";
    hello["version"] = protocol_version;
    hello["client"] = options.name;
    json::array scopes;
    for (const scope item : {scope::read, scope::act, scope::gui, scope::data})
    {
        if (has(options.scopes, item))
            scopes.push_back(json::string(scope_name(item)));
    }
    hello["scopes"] = std::move(scopes);
    if (const auto sent = state->write_frame({json::serialize(hello), {}});
        !sent)
        return sent.error();

    // The person may take a while to answer.
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    for (;;)
    {
        auto got = state->read_frame(deadline);
        if (!got)
            return got.error();
        if (!*got)
            return make_error_code(errc::timeout);
        boost::system::error_code ec;
        const json::value body = json::parse((*got)->header, ec);
        if (ec || !body.is_object())
            return make_error_code(errc::io_error);
        const auto& object = body.as_object();
        if (object.if_contains("event") != nullptr)
            continue;
        const auto* ok = object.if_contains("ok");
        if (ok == nullptr || !ok->is_bool() || !ok->as_bool())
            return make_error_code(errc::permission_denied);
        if (const auto* result = object.if_contains("result");
            result != nullptr && result->is_object())
        {
            if (const auto* list = result->as_object().if_contains("granted");
                list != nullptr && list->is_array())
            {
                for (const auto& item : list->as_array())
                {
                    if (item.is_string())
                        if (const auto known =
                                scope_from_name(item.as_string()))
                            state->granted_ |= bit(*known);
                }
            }
        }
        break;
    }
    return std::unique_ptr<client>(new client(std::move(state)));
}

client::client(std::unique_ptr<impl> state) : impl_(std::move(state)) {}

client::~client()
{
    if (impl_ && impl_->stream_)
        impl_->stream_->close();
}

scope_set client::granted() const noexcept
{
    return impl_->granted_;
}

result<reply> client::call(const std::string& operation,
                           const std::string& args_json)
{
    const std::int64_t id = impl_->next_id_++;
    json::object request;
    request["id"] = id;
    request["op"] = operation;
    boost::system::error_code ec;
    json::value args = json::parse(args_json, ec);
    request["args"] = ec ? json::value(json::object{}) : std::move(args);
    if (const auto sent = impl_->write_frame({json::serialize(request), {}});
        !sent)
        return sent.error();

    const auto deadline = std::chrono::steady_clock::now() + impl_->timeout_;
    for (;;)
    {
        auto got = impl_->read_frame(deadline);
        if (!got)
            return got.error();
        if (!*got)
            return make_error_code(errc::timeout);
        boost::system::error_code parse_ec;
        const json::value body = json::parse((*got)->header, parse_ec);
        if (parse_ec || !body.is_object())
            return make_error_code(errc::io_error);
        const auto& object = body.as_object();
        if (object.if_contains("event") != nullptr)
        {
            impl_->events_.push_back((*got)->header);
            continue;
        }
        const auto* answered = object.if_contains("id");
        if (answered == nullptr || !answered->is_number() ||
            answered->to_number<std::int64_t>() != id)
            continue;

        reply out;
        const auto* ok = object.if_contains("ok");
        out.ok = ok != nullptr && ok->is_bool() && ok->as_bool();
        if (out.ok)
        {
            if (const auto* result = object.if_contains("result"))
                out.json = json::serialize(*result);
        }
        else if (const auto* error = object.if_contains("error");
                 error != nullptr && error->is_object())
        {
            const auto& details = error->as_object();
            if (const auto* code = details.if_contains("code");
                code != nullptr && code->is_string())
                out.code = std::string(code->as_string());
            if (const auto* message = details.if_contains("message");
                message != nullptr && message->is_string())
                out.message = std::string(message->as_string());
        }
        out.blob = std::move((*got)->blob);
        return out;
    }
}

result<std::string> client::next_event(std::chrono::milliseconds limit)
{
    if (!impl_->events_.empty())
    {
        std::string out = std::move(impl_->events_.front());
        impl_->events_.pop_front();
        return out;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;)
    {
        auto got = impl_->read_frame(deadline);
        if (!got)
            return got.error();
        if (!*got)
            return make_error_code(errc::timeout);
        boost::system::error_code ec;
        const json::value body = json::parse((*got)->header, ec);
        if (!ec && body.is_object() &&
            body.as_object().if_contains("event") != nullptr)
            return (*got)->header;
    }
}

} // namespace vss::debug
