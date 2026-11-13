//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/control/proto/message.hpp>

#include <boost/json.hpp>

#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace vss::control
{
namespace json = boost::json;

namespace
{

error_code malformed() noexcept
{
    return make_error_code(api_errc::invalid_argument);
}

// -- text --------------------------------------------------------------------

/// The length of the UTF-8 sequence that starts at `text[index]`, or 0 when
/// the bytes there are not one.
std::size_t utf8_sequence(std::string_view text, std::size_t index) noexcept
{
    const auto at = [&](std::size_t offset)
    { return static_cast<unsigned char>(text[index + offset]); };
    const unsigned char lead = at(0);
    if (lead < 0x80)
        return 1;
    std::size_t length = 0;
    std::uint32_t point = 0;
    std::uint32_t least = 0;
    if ((lead & 0xe0) == 0xc0)
    {
        length = 2;
        point = lead & 0x1fu;
        least = 0x80;
    }
    else if ((lead & 0xf0) == 0xe0)
    {
        length = 3;
        point = lead & 0x0fu;
        least = 0x800;
    }
    else if ((lead & 0xf8) == 0xf0)
    {
        length = 4;
        point = lead & 0x07u;
        least = 0x10000;
    }
    else
    {
        return 0;
    }
    if (text.size() - index < length)
        return 0;
    for (std::size_t offset = 1; offset < length; ++offset)
    {
        const unsigned char next = at(offset);
        if ((next & 0xc0) != 0x80)
            return 0;
        point = (point << 6) | (next & 0x3fu);
    }
    if (point < least || point > 0x10ffff ||
        (point >= 0xd800 && point <= 0xdfff))
        return 0;
    return length;
}

/// Text that is safe to send: valid UTF-8 (a byte that is not becomes `?`) and
/// no longer than `max_text_size`, cut between characters.  A port name or a
/// path is not ours to trust, and a header that is not valid JSON would cost
/// the reader its connection.
std::string clean_text(std::string_view text)
{
    std::string out;
    out.reserve(text.size() < max_text_size ? text.size() : max_text_size);
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::size_t length = utf8_sequence(text, index);
        const std::size_t step = length == 0 ? 1 : length;
        if (out.size() + step > max_text_size)
            break;
        if (length == 0)
            out.push_back('?');
        else
            out.append(text.substr(index, length));
        index += step;
    }
    return out;
}

json::array text_list(const std::vector<std::string>& items)
{
    json::array out;
    for (const std::string& item : items)
    {
        if (out.size() == max_list_size)
            break;
        out.emplace_back(clean_text(item));
    }
    return out;
}

// -- reading JSON ------------------------------------------------------------

std::optional<json::object> parse_header(const std::string& header)
{
    json::parse_options options;
    options.max_depth = max_json_depth;
    boost::system::error_code failed;
    json::value parsed = json::parse(header, failed, {}, options);
    if (failed || !parsed.is_object())
        return std::nullopt;
    // The object is a part of `parsed`, so it is moved out, not elided.
    return std::optional<json::object>{std::in_place,
                                       std::move(parsed.as_object())};
}

bool to_u64(const json::value& item, std::uint64_t& out) noexcept
{
    if (item.is_uint64())
    {
        out = item.get_uint64();
        return true;
    }
    if (item.is_int64() && item.get_int64() >= 0)
    {
        out = static_cast<std::uint64_t>(item.get_int64());
        return true;
    }
    return false;
}

bool read_u64(const json::object& object, std::string_view key,
              std::uint64_t& out) noexcept
{
    const json::value* const item = object.if_contains(key);
    return item != nullptr && to_u64(*item, out);
}

bool read_u32(const json::object& object, std::string_view key,
              std::uint32_t& out) noexcept
{
    std::uint64_t wide = 0;
    if (!read_u64(object, key, wide) ||
        wide > std::numeric_limits<std::uint32_t>::max())
        return false;
    out = static_cast<std::uint32_t>(wide);
    return true;
}

bool read_bool(const json::object& object, std::string_view key,
               bool& out) noexcept
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr || !item->is_bool())
        return false;
    out = item->get_bool();
    return true;
}

bool to_text(const json::value& item, std::string& out)
{
    if (!item.is_string() || item.get_string().size() > max_text_size)
        return false;
    out.assign(item.get_string().data(), item.get_string().size());
    return true;
}

bool read_text(const json::object& object, std::string_view key,
               std::string& out)
{
    const json::value* const item = object.if_contains(key);
    return item != nullptr && to_text(*item, out);
}

/// As `read_text`, for a field that may be left out.  False only when the
/// field is there and is not a text.
bool read_optional_text(const json::object& object, std::string_view key,
                        std::string& out)
{
    const json::value* const item = object.if_contains(key);
    return item == nullptr || to_text(*item, out);
}

bool read_text_list(const json::object& object, std::string_view key,
                    std::vector<std::string>& out)
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr || !item->is_array() ||
        item->get_array().size() > max_list_size)
        return false;
    out.clear();
    out.reserve(item->get_array().size());
    for (const json::value& entry : item->get_array())
    {
        std::string text;
        if (!to_text(entry, text))
            return false;
        out.push_back(std::move(text));
    }
    return true;
}

const json::array* read_array(const json::object& object,
                              std::string_view key) noexcept
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr || !item->is_array() ||
        item->get_array().size() > max_list_size)
        return nullptr;
    return &item->get_array();
}

// -- requests ----------------------------------------------------------------

struct request_writer
{
    json::object& header;

    void operator()(const hello_request& body) const
    {
        header["api"] = body.api;
        header["client"] = clean_text(body.client);
    }
    void operator()(const status_request&) const {}
    void operator()(const ports_list_request&) const {}
    void operator()(const endpoint_stop_request&) const {}
};

result<request_body> read_request_body(operation wanted,
                                       const json::object& header)
{
    switch (wanted)
    {
    case operation::hello:
    {
        hello_request body;
        if (!read_u32(header, "api", body.api) ||
            !read_optional_text(header, "client", body.client))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::status:
        return request_body{status_request{}};
    case operation::ports_list:
        return request_body{ports_list_request{}};
    case operation::endpoint_stop:
        return request_body{endpoint_stop_request{}};
    }
    return make_error_code(api_errc::unsupported);
}

// -- replies -----------------------------------------------------------------

json::object write_port(const port_entry& port)
{
    json::object out;
    out["id"] = clean_text(port.id);
    out["name"] = clean_text(port.name);
    out["path"] = clean_text(port.path);
    out["kind"] = clean_text(port.kind);
    out["state"] = clean_text(port.state);
    out["open"] = port.open;
    out["bytes_rx"] = port.bytes_rx;
    out["bytes_tx"] = port.bytes_tx;
    return out;
}

bool read_port(const json::value& item, port_entry& out)
{
    if (!item.is_object())
        return false;
    const json::object& object = item.get_object();
    return read_text(object, "id", out.id) &&
           read_text(object, "name", out.name) &&
           read_text(object, "path", out.path) &&
           read_text(object, "kind", out.kind) &&
           read_text(object, "state", out.state) &&
           read_bool(object, "open", out.open) &&
           read_u64(object, "bytes_rx", out.bytes_rx) &&
           read_u64(object, "bytes_tx", out.bytes_tx);
}

json::object write_link(const link_entry& link)
{
    json::object out;
    out["id"] = clean_text(link.id);
    out["mode"] = clean_text(link.mode);
    out["sources"] = text_list(link.sources);
    out["targets"] = text_list(link.targets);
    out["bytes"] = link.bytes;
    return out;
}

bool read_link(const json::value& item, link_entry& out)
{
    if (!item.is_object())
        return false;
    const json::object& object = item.get_object();
    return read_text(object, "id", out.id) &&
           read_text(object, "mode", out.mode) &&
           read_text_list(object, "sources", out.sources) &&
           read_text_list(object, "targets", out.targets) &&
           read_u64(object, "bytes", out.bytes);
}

struct reply_writer
{
    json::object& header;

    void operator()(const done_reply&) const { header["reply"] = "done"; }

    void operator()(const hello_reply& body) const
    {
        header["reply"] = "hello";
        header["api"] = body.api;
        header["version"] = clean_text(body.version);
        header["principal"] = clean_text(body.principal);
        header["scopes"] = text_list(body.scopes);
        header["features"] = text_list(body.features);
    }

    void operator()(const status_reply& body) const
    {
        header["reply"] = "status";
        header["uptime_ms"] = body.uptime_ms;
        header["sessions"] = body.sessions;
        json::object agent;
        agent["running"] = body.agent_running;
        agent["version"] = clean_text(body.agent_version);
        header["agent"] = std::move(agent);
    }

    void operator()(const ports_reply& body) const
    {
        header["reply"] = "ports";
        header["revision"] = body.revision;
        json::array ports;
        for (const port_entry& port : body.ports)
        {
            if (ports.size() == max_list_size)
                break;
            ports.emplace_back(write_port(port));
        }
        header["ports"] = std::move(ports);
        json::array links;
        for (const link_entry& link : body.links)
        {
            if (links.size() == max_list_size)
                break;
            links.emplace_back(write_link(link));
        }
        header["links"] = std::move(links);
    }

    void operator()(const error_reply& body) const
    {
        header["reply"] = "error";
        json::object error;
        error["code"] = clean_text(body.error.code);
        error["message"] = clean_text(body.error.message);
        error["retryable"] = body.error.retryable;
        header["error"] = std::move(error);
    }
};

result<reply_body> read_hello_reply(const json::object& header)
{
    hello_reply body;
    if (!read_u32(header, "api", body.api) ||
        !read_text(header, "version", body.version) ||
        !read_text(header, "principal", body.principal) ||
        !read_text_list(header, "scopes", body.scopes) ||
        !read_text_list(header, "features", body.features))
        return malformed();
    return reply_body{std::move(body)};
}

result<reply_body> read_status_reply(const json::object& header)
{
    status_reply body;
    const json::value* const agent = header.if_contains("agent");
    if (agent == nullptr || !agent->is_object() ||
        !read_u64(header, "uptime_ms", body.uptime_ms) ||
        !read_u64(header, "sessions", body.sessions) ||
        !read_bool(agent->get_object(), "running", body.agent_running) ||
        !read_text(agent->get_object(), "version", body.agent_version))
        return malformed();
    return reply_body{std::move(body)};
}

result<reply_body> read_ports_reply(const json::object& header)
{
    ports_reply body;
    const json::array* const ports = read_array(header, "ports");
    const json::array* const links = read_array(header, "links");
    if (ports == nullptr || links == nullptr ||
        !read_u64(header, "revision", body.revision))
        return malformed();
    body.ports.reserve(ports->size());
    for (const json::value& item : *ports)
    {
        port_entry port;
        if (!read_port(item, port))
            return malformed();
        body.ports.push_back(std::move(port));
    }
    body.links.reserve(links->size());
    for (const json::value& item : *links)
    {
        link_entry link;
        if (!read_link(item, link))
            return malformed();
        body.links.push_back(std::move(link));
    }
    return reply_body{std::move(body)};
}

result<reply_body> read_error_reply(const json::object& header)
{
    error_reply body;
    const json::value* const error = header.if_contains("error");
    if (error == nullptr || !error->is_object() ||
        !read_text(error->get_object(), "code", body.error.code) ||
        !read_text(error->get_object(), "message", body.error.message) ||
        !read_bool(error->get_object(), "retryable", body.error.retryable))
        return malformed();
    return reply_body{std::move(body)};
}

} // namespace

operation operation_of(const request_body& body)
{
    struct visitor
    {
        operation operator()(const hello_request&) const noexcept
        {
            return operation::hello;
        }
        operation operator()(const status_request&) const noexcept
        {
            return operation::status;
        }
        operation operator()(const ports_list_request&) const noexcept
        {
            return operation::ports_list;
        }
        operation operator()(const endpoint_stop_request&) const noexcept
        {
            return operation::endpoint_stop;
        }
    };
    return std::visit(visitor{}, body);
}

frame encode(const request& value)
{
    json::object header;
    header["id"] = value.id;
    header["op"] = info_of(operation_of(value.body)).name;
    std::visit(request_writer{header}, value.body);
    frame out;
    out.header = json::serialize(header);
    return out;
}

frame encode(const reply& value)
{
    json::object header;
    header["id"] = value.id;
    std::visit(reply_writer{header}, value.body);
    frame out;
    out.header = json::serialize(header);
    return out;
}

result<request> decode_request(const frame& value)
{
    const auto header = parse_header(value.header);
    if (!header)
        return malformed();
    request out;
    std::string name;
    if (!read_u64(*header, "id", out.id) || out.id == 0 ||
        !read_text(*header, "op", name))
        return malformed();
    const operation_info* const wanted = find_operation(name);
    if (wanted == nullptr)
        return make_error_code(api_errc::unsupported);
    // No request of this version carries bytes.
    if (!value.blob.empty())
        return malformed();
    auto body = read_request_body(wanted->id, *header);
    if (!body)
        return body.error();
    out.body = std::move(*body);
    return out;
}

result<reply> decode_reply(const frame& value)
{
    const auto header = parse_header(value.header);
    if (!header)
        return malformed();
    reply out;
    std::string kind;
    if (!read_u64(*header, "id", out.id) ||
        !read_text(*header, "reply", kind) || !value.blob.empty())
        return malformed();

    result<reply_body> body = malformed();
    if (kind == "done")
        body = reply_body{done_reply{}};
    else if (kind == "hello")
        body = read_hello_reply(*header);
    else if (kind == "status")
        body = read_status_reply(*header);
    else if (kind == "ports")
        body = read_ports_reply(*header);
    else if (kind == "error")
        body = read_error_reply(*header);
    if (!body)
        return body.error();
    out.body = std::move(*body);
    return out;
}

std::uint64_t peek_request_id(const frame& value) noexcept
{
    try
    {
        const auto header = parse_header(value.header);
        std::uint64_t id = 0;
        if (header && read_u64(*header, "id", id))
            return id;
        return 0;
    }
    catch (...)
    {
        // Out of memory while looking: the caller answers with id 0.
        return 0;
    }
}

std::string port_id_text(std::uint64_t endpoint)
{
    return "p_" + std::to_string(endpoint);
}

std::string link_id_text(std::uint64_t link)
{
    return "l_" + std::to_string(link);
}

} // namespace vss::control
