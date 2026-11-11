//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/agent/proto/message.hpp>

#include <boost/json.hpp>

#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace vss::agent
{
namespace json = boost::json;

namespace
{

error_code malformed() noexcept
{
    return make_error_code(protocol_errc::malformed_message);
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
/// no longer than `max_text_size`, cut between characters.  A path from the
/// operating system or a message from a driver is not ours to trust, and a
/// header that is not valid JSON would cost the reader its connection.
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

// -- enumerations ------------------------------------------------------------

constexpr std::array endpoint_kinds{
    endpoint_kind::virtual_port, endpoint_kind::physical, endpoint_kind::tcp,
    endpoint_kind::udp,          endpoint_kind::rfc2217,  endpoint_kind::mock};
constexpr std::array endpoint_states{
    endpoint_state::created, endpoint_state::opening, endpoint_state::open,
    endpoint_state::closed, endpoint_state::failed};
constexpr std::array link_modes{link_mode::duplex, link_mode::one_way,
                                link_mode::bus};
constexpr std::array directions{data_direction::rx, data_direction::tx};
constexpr std::array event_kinds{event_kind::started,    event_kind::stopped,
                                 event_kind::data,       event_kind::lines,
                                 event_kind::error,      event_kind::stats,
                                 event_kind::log_message};

/// The enumerator whose engine name (`to_string`) is `text`.
template <class Enum, std::size_t Count>
bool parse_enum(std::string_view text, const std::array<Enum, Count>& all,
                Enum& out) noexcept
{
    for (const Enum candidate : all)
    {
        if (text == to_string(candidate))
        {
            out = candidate;
            return true;
        }
    }
    return false;
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

/// The text of a key, as a view into the object.
std::optional<std::string_view> view_text(const json::object& object,
                                          std::string_view key) noexcept
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr || !item->is_string())
        return std::nullopt;
    return std::string_view(item->get_string().data(),
                            item->get_string().size());
}

template <class Enum, std::size_t Count>
bool read_enum(const json::object& object, std::string_view key,
               const std::array<Enum, Count>& all, Enum& out) noexcept
{
    const auto text = view_text(object, key);
    return text && parse_enum(*text, all, out);
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

bool read_ids(const json::object& object, std::string_view key,
              std::vector<endpoint_id>& out)
{
    const json::array* const items = read_array(object, key);
    if (items == nullptr)
        return false;
    out.reserve(items->size());
    for (const json::value& item : *items)
    {
        std::uint64_t id = 0;
        if (!to_u64(item, id))
            return false;
        out.push_back(id);
    }
    return true;
}

bool read_error(const json::object& object, std::string_view key,
                wire_error& out)
{
    const json::value* const item = object.if_contains(key);
    if (item == nullptr || !item->is_object())
        return false;
    const json::object& error = item->get_object();
    const json::value* const value = error.if_contains("value");
    if (value == nullptr || !value->is_int64() ||
        value->get_int64() < std::numeric_limits<int>::min() ||
        value->get_int64() > std::numeric_limits<int>::max())
        return false;
    out.value = static_cast<int>(value->get_int64());
    return read_text(error, "category", out.category) &&
           read_text(error, "message", out.message);
}

// -- writing JSON ------------------------------------------------------------

json::object error_to_json(const wire_error& error)
{
    json::object out;
    out["category"] = clean_text(error.category);
    out["value"] = error.value;
    out["message"] = clean_text(error.message);
    return out;
}

json::array ids_to_json(const std::vector<endpoint_id>& ids)
{
    json::array out;
    out.reserve(ids.size());
    for (const endpoint_id id : ids)
        out.emplace_back(id);
    return out;
}

frame make_frame(const json::object& header, byte_buffer blob = {})
{
    frame out;
    out.header = json::serialize(header);
    out.blob = std::move(blob);
    return out;
}

// -- requests ----------------------------------------------------------------

struct request_writer
{
    json::object& header;
    byte_buffer& blob;

    void operator()(const hello_request& body) const
    {
        header["op"] = "hello";
        header["protocol"] = body.protocol;
        header["client"] = clean_text(body.client);
    }
    void operator()(const list_request&) const { header["op"] = "list"; }
    void operator()(const create_port_request& body) const
    {
        header["op"] = "create_port";
        header["name"] = clean_text(body.name);
        header["kind"] = clean_text(body.kind);
    }
    void operator()(const remove_port_request& body) const
    {
        header["op"] = "remove_port";
        header["endpoint"] = body.endpoint;
    }
    void operator()(const connect_request& body) const
    {
        header["op"] = "connect";
        header["first"] = body.first;
        header["second"] = body.second;
        header["one_way"] = body.one_way;
    }
    void operator()(const write_request& body) const
    {
        header["op"] = "write";
        header["endpoint"] = body.endpoint;
        blob = body.data;
    }
    void operator()(const subscribe_request& body) const
    {
        header["op"] = "subscribe";
        header["data"] = body.data;
        header["interval_ms"] = body.interval_ms;
    }
    void operator()(const shutdown_request&) const
    {
        header["op"] = "shutdown";
    }
};

result<request_body> read_request_body(const json::object& header,
                                       std::string_view op, const frame& value)
{
    // Only a write carries bytes; a blob anywhere else is a mistake.
    if (op != "write" && !value.blob.empty())
        return malformed();

    if (op == "hello")
    {
        hello_request body;
        if (!read_u32(header, "protocol", body.protocol) ||
            !read_text(header, "client", body.client))
            return malformed();
        return request_body{std::move(body)};
    }
    if (op == "list")
        return request_body{list_request{}};
    if (op == "create_port")
    {
        create_port_request body;
        if (!read_text(header, "name", body.name) ||
            !read_text(header, "kind", body.kind))
            return malformed();
        return request_body{std::move(body)};
    }
    if (op == "remove_port")
    {
        remove_port_request body;
        if (!read_u64(header, "endpoint", body.endpoint))
            return malformed();
        return request_body{body};
    }
    if (op == "connect")
    {
        connect_request body;
        if (!read_u64(header, "first", body.first) ||
            !read_u64(header, "second", body.second) ||
            !read_bool(header, "one_way", body.one_way))
            return malformed();
        return request_body{body};
    }
    if (op == "write")
    {
        write_request body;
        if (!read_u64(header, "endpoint", body.endpoint))
            return malformed();
        body.data = value.blob;
        return request_body{std::move(body)};
    }
    if (op == "subscribe")
    {
        subscribe_request body;
        if (!read_bool(header, "data", body.data) ||
            !read_u32(header, "interval_ms", body.interval_ms))
            return malformed();
        return request_body{body};
    }
    if (op == "shutdown")
        return request_body{shutdown_request{}};
    return make_error_code(protocol_errc::unknown_operation);
}

// -- replies -----------------------------------------------------------------

json::object port_to_json(const port_summary& port)
{
    json::object out;
    out["id"] = port.id;
    out["name"] = clean_text(port.name);
    out["path"] = clean_text(port.path);
    out["kind"] = to_string(port.kind);
    out["state"] = to_string(port.state);
    out["open"] = port.open;
    out["bytes_rx"] = port.bytes_rx;
    out["bytes_tx"] = port.bytes_tx;
    return out;
}

bool port_from_json(const json::value& item, port_summary& out)
{
    if (!item.is_object())
        return false;
    const json::object& port = item.get_object();
    return read_u64(port, "id", out.id) && read_text(port, "name", out.name) &&
           read_text(port, "path", out.path) &&
           read_enum(port, "kind", endpoint_kinds, out.kind) &&
           read_enum(port, "state", endpoint_states, out.state) &&
           read_bool(port, "open", out.open) &&
           read_u64(port, "bytes_rx", out.bytes_rx) &&
           read_u64(port, "bytes_tx", out.bytes_tx);
}

json::object link_to_json(const link_summary& link)
{
    json::object out;
    out["id"] = link.id;
    out["mode"] = to_string(link.mode);
    out["sources"] = ids_to_json(link.sources);
    out["targets"] = ids_to_json(link.targets);
    out["bytes"] = link.bytes;
    return out;
}

bool link_from_json(const json::value& item, link_summary& out)
{
    if (!item.is_object())
        return false;
    const json::object& link = item.get_object();
    return read_u64(link, "id", out.id) &&
           read_enum(link, "mode", link_modes, out.mode) &&
           read_ids(link, "sources", out.sources) &&
           read_ids(link, "targets", out.targets) &&
           read_u64(link, "bytes", out.bytes);
}

struct reply_writer
{
    json::object& header;

    void operator()(const done_reply&) const { header["reply"] = "done"; }
    void operator()(const hello_reply& body) const
    {
        header["reply"] = "hello";
        header["protocol"] = body.protocol;
        header["agent_version"] = clean_text(body.agent_version);
        header["process_id"] = body.process_id;
        json::array kinds;
        for (const std::string& kind : body.port_kinds)
            kinds.emplace_back(clean_text(kind));
        header["port_kinds"] = std::move(kinds);
    }
    void operator()(const list_reply& body) const
    {
        header["reply"] = "list";
        header["revision"] = body.revision;
        json::array ports;
        ports.reserve(body.ports.size());
        for (const port_summary& port : body.ports)
            ports.emplace_back(port_to_json(port));
        header["ports"] = std::move(ports);
        json::array links;
        links.reserve(body.links.size());
        for (const link_summary& link : body.links)
            links.emplace_back(link_to_json(link));
        header["links"] = std::move(links);
    }
    void operator()(const create_port_reply& body) const
    {
        header["reply"] = "port";
        header["endpoint"] = body.endpoint;
    }
    void operator()(const connect_reply& body) const
    {
        header["reply"] = "link";
        header["link"] = body.link;
    }
    void operator()(const error_reply& body) const
    {
        header["reply"] = "error";
        header["error"] = error_to_json(body.error);
    }
};

result<reply_body> read_reply_body(const json::object& header,
                                   std::string_view kind)
{
    if (kind == "done")
        return reply_body{done_reply{}};
    if (kind == "hello")
    {
        hello_reply body;
        const json::array* const kinds = read_array(header, "port_kinds");
        if (kinds == nullptr || !read_u32(header, "protocol", body.protocol) ||
            !read_text(header, "agent_version", body.agent_version) ||
            !read_u64(header, "process_id", body.process_id))
            return malformed();
        for (const json::value& item : *kinds)
        {
            std::string text;
            if (!to_text(item, text))
                return malformed();
            body.port_kinds.push_back(std::move(text));
        }
        return reply_body{std::move(body)};
    }
    if (kind == "list")
    {
        list_reply body;
        const json::array* const ports = read_array(header, "ports");
        const json::array* const links = read_array(header, "links");
        if (ports == nullptr || links == nullptr ||
            !read_u64(header, "revision", body.revision))
            return malformed();
        body.ports.resize(ports->size());
        for (std::size_t index = 0; index < ports->size(); ++index)
        {
            if (!port_from_json((*ports)[index], body.ports[index]))
                return malformed();
        }
        body.links.resize(links->size());
        for (std::size_t index = 0; index < links->size(); ++index)
        {
            if (!link_from_json((*links)[index], body.links[index]))
                return malformed();
        }
        return reply_body{std::move(body)};
    }
    if (kind == "port")
    {
        create_port_reply body;
        if (!read_u64(header, "endpoint", body.endpoint))
            return malformed();
        return reply_body{body};
    }
    if (kind == "link")
    {
        connect_reply body;
        if (!read_u64(header, "link", body.link))
            return malformed();
        return reply_body{body};
    }
    if (kind == "error")
    {
        error_reply body;
        if (!read_error(header, "error", body.error))
            return malformed();
        return reply_body{std::move(body)};
    }
    return malformed();
}

// -- events ------------------------------------------------------------------

/// Reads one event and takes its bytes from `blob` at `cursor`.  The events
/// of a batch use the blob in order and without gaps, so a small frame cannot
/// name the same bytes many times and ask the reader for more than it sent.
bool event_from_json(const json::value& item, const byte_buffer& blob,
                     std::size_t& cursor, event_record& out)
{
    if (!item.is_object())
        return false;
    const json::object& event = item.get_object();
    std::uint64_t stored = 0;
    if (!read_enum(event, "kind", event_kinds, out.kind) ||
        !read_u64(event, "endpoint", out.endpoint) ||
        !read_text(event, "name", out.name) ||
        !read_enum(event, "direction", directions, out.direction) ||
        !read_u64(event, "size", out.size) ||
        !read_u64(event, "stored", stored) ||
        !read_u32(event, "lines", out.lines) ||
        !read_text(event, "message", out.message))
        return false;
    if (event.contains("error") && !read_error(event, "error", out.error))
        return false;
    if (stored > blob.size() - cursor)
        return false;
    const auto first = blob.begin() + static_cast<std::ptrdiff_t>(cursor);
    out.data.assign(first, first + static_cast<std::ptrdiff_t>(stored));
    cursor += static_cast<std::size_t>(stored);
    return true;
}

result<server_message> read_batch(const json::object& header,
                                  const frame& value)
{
    event_batch batch;
    const json::array* const events = read_array(header, "events");
    if (events == nullptr || !read_u64(header, "revision", batch.revision) ||
        !read_u64(header, "dropped", batch.dropped))
        return malformed();
    batch.events.resize(events->size());
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < events->size(); ++index)
    {
        if (!event_from_json((*events)[index], value.blob, cursor,
                             batch.events[index]))
            return malformed();
    }
    if (cursor != value.blob.size())
        return malformed();
    return server_message{std::move(batch)};
}

} // namespace

frame encode(const request& value)
{
    json::object header;
    header["id"] = value.id;
    byte_buffer blob;
    std::visit(request_writer{header, blob}, value.body);
    return make_frame(header, std::move(blob));
}

frame encode(const reply& value)
{
    json::object header;
    header["id"] = value.id;
    std::visit(reply_writer{header}, value.body);
    return make_frame(header);
}

frame encode(const event_batch& value)
{
    json::object header;
    header["event"] = "batch";
    header["revision"] = value.revision;
    header["dropped"] = value.dropped;
    json::array events;
    events.reserve(value.events.size());
    byte_buffer blob;
    for (const event_record& record : value.events)
    {
        json::object event;
        event["kind"] = to_string(record.kind);
        event["endpoint"] = record.endpoint;
        event["name"] = clean_text(record.name);
        event["direction"] = to_string(record.direction);
        event["size"] = record.size;
        event["stored"] = record.data.size();
        event["lines"] = record.lines;
        event["message"] = clean_text(record.message);
        if (record.error != wire_error{})
            event["error"] = error_to_json(record.error);
        events.emplace_back(std::move(event));
        blob.insert(blob.end(), record.data.begin(), record.data.end());
    }
    header["events"] = std::move(events);
    return make_frame(header, std::move(blob));
}

result<request> decode_request(const frame& value)
{
    const auto header = parse_header(value.header);
    if (!header)
        return malformed();
    request out;
    const auto op = view_text(*header, "op");
    if (!read_u64(*header, "id", out.id) || out.id == 0 || !op)
        return malformed();
    auto body = read_request_body(*header, *op, value);
    if (!body)
        return body.error();
    out.body = std::move(*body);
    return out;
}

result<server_message> decode_server_message(const frame& value)
{
    const auto header = parse_header(value.header);
    if (!header)
        return malformed();
    if (const auto event = view_text(*header, "event"))
    {
        if (*event != "batch")
            return malformed();
        return read_batch(*header, value);
    }
    reply out;
    const auto kind = view_text(*header, "reply");
    if (!kind || !read_u64(*header, "id", out.id) || !value.blob.empty())
        return malformed();
    auto body = read_reply_body(*header, *kind);
    if (!body)
        return body.error();
    out.body = std::move(*body);
    return server_message{std::move(out)};
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
        // Out of memory while looking: the caller answers without an id.
        return 0;
    }
}

} // namespace vss::agent
