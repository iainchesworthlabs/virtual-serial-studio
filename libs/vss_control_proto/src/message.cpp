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
        // Sent as it is: a token is ASCII, and one that is not is refused by
        // the gateway, not repaired here.
        if (!body.token.empty())
            header["token"] = body.token.expose();
    }
    void operator()(const tokens_create_request& body) const
    {
        header["label"] = clean_text(body.label);
        header["scopes"] = text_list(body.scopes);
        header["life_s"] = body.life_s;
    }
    void operator()(const tokens_list_request&) const {}
    void operator()(const tokens_revoke_request& body) const
    {
        header["token_id"] = clean_text(body.id);
    }
    void operator()(const status_request&) const {}
    void operator()(const ports_list_request&) const {}
    void operator()(const endpoint_stop_request&) const {}
    void write_stream(const stream_options& options) const
    {
        header["interval_ms"] = options.interval_ms;
        header["ports"] = text_list(options.ports);
    }
    void operator()(const events_subscribe_request& body) const
    {
        write_stream(body.options);
    }
    void operator()(const traffic_subscribe_request& body) const
    {
        write_stream(body.options);
    }
    void operator()(const events_unsubscribe_request&) const {}
    void operator()(const traffic_unsubscribe_request&) const {}
    void operator()(const sessions_list_request&) const {}
    void operator()(const sessions_kick_request& body) const
    {
        header["session"] = body.session;
    }
    void operator()(const ports_create_request& body) const
    {
        header["name"] = clean_text(body.name);
        header["kind"] = clean_text(body.kind);
    }
    void operator()(const ports_remove_request& body) const
    {
        header["port"] = clean_text(body.port);
    }
    void operator()(const links_connect_request& body) const
    {
        header["first"] = clean_text(body.first);
        header["second"] = clean_text(body.second);
        header["one_way"] = body.one_way;
    }
    void operator()(const ports_send_request& body) const
    {
        header["port"] = clean_text(body.port);
    }
};

/// Both fields may be left out.  A list of ports that is too long, or holds
/// something that is not a text, is a mistake; whether each text names a
/// port is for the gateway to say, with an error the caller can read.
bool read_stream(const json::object& header, stream_options& out)
{
    if (header.contains("interval_ms") &&
        !read_u32(header, "interval_ms", out.interval_ms))
        return false;
    if (!header.contains("ports"))
        return true;
    return read_text_list(header, "ports", out.ports) &&
           out.ports.size() <= max_port_filter_size;
}

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
        if (const json::value* const token = header.if_contains("token"))
        {
            if (!token->is_string() ||
                token->get_string().size() > max_token_text_size)
                return malformed();
            body.token = secret_string(std::string_view(
                token->get_string().data(), token->get_string().size()));
        }
        return request_body{std::move(body)};
    }
    case operation::tokens_create:
    {
        tokens_create_request body;
        if (!read_text(header, "label", body.label) ||
            !read_text_list(header, "scopes", body.scopes) ||
            !read_u64(header, "life_s", body.life_s))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::tokens_list:
        return request_body{tokens_list_request{}};
    case operation::tokens_revoke:
    {
        tokens_revoke_request body;
        if (!read_text(header, "token_id", body.id))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::status:
        return request_body{status_request{}};
    case operation::ports_list:
        return request_body{ports_list_request{}};
    case operation::endpoint_stop:
        return request_body{endpoint_stop_request{}};
    case operation::events_subscribe:
    {
        events_subscribe_request body;
        if (!read_stream(header, body.options))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::traffic_subscribe:
    {
        traffic_subscribe_request body;
        if (!read_stream(header, body.options))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::events_unsubscribe:
        return request_body{events_unsubscribe_request{}};
    case operation::traffic_unsubscribe:
        return request_body{traffic_unsubscribe_request{}};
    case operation::sessions_list:
        return request_body{sessions_list_request{}};
    case operation::sessions_kick:
    {
        sessions_kick_request body;
        if (!read_u64(header, "session", body.session))
            return malformed();
        return request_body{body};
    }
    case operation::ports_create:
    {
        ports_create_request body;
        if (!read_text(header, "name", body.name) ||
            !read_optional_text(header, "kind", body.kind))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::ports_remove:
    {
        ports_remove_request body;
        if (!read_text(header, "port", body.port))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::links_connect:
    {
        links_connect_request body;
        if (!read_text(header, "first", body.first) ||
            !read_text(header, "second", body.second))
            return malformed();
        if (header.contains("one_way") &&
            !read_bool(header, "one_way", body.one_way))
            return malformed();
        return request_body{std::move(body)};
    }
    case operation::ports_send:
    {
        // The bytes are put in by `decode_request`, which has the frame.
        ports_send_request body;
        if (!read_text(header, "port", body.port))
            return malformed();
        return request_body{std::move(body)};
    }
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

json::object write_token(const token_entry& token)
{
    json::object out;
    out["id"] = clean_text(token.id);
    out["label"] = clean_text(token.label);
    out["scopes"] = text_list(token.scopes);
    out["transports"] = text_list(token.transports);
    out["created_ms"] = token.created_ms;
    out["expires_ms"] = token.expires_ms;
    out["last_used_ms"] = token.last_used_ms;
    out["revoked"] = token.revoked;
    return out;
}

bool read_token(const json::value& item, token_entry& out)
{
    if (!item.is_object())
        return false;
    const json::object& object = item.get_object();
    return read_text(object, "id", out.id) &&
           read_text(object, "label", out.label) &&
           read_text_list(object, "scopes", out.scopes) &&
           read_text_list(object, "transports", out.transports) &&
           read_u64(object, "created_ms", out.created_ms) &&
           read_u64(object, "expires_ms", out.expires_ms) &&
           read_u64(object, "last_used_ms", out.last_used_ms) &&
           read_bool(object, "revoked", out.revoked);
}

json::object write_session(const session_entry& session)
{
    json::object out;
    out["number"] = session.number;
    out["principal"] = clean_text(session.principal);
    out["transport"] = clean_text(session.transport);
    out["client"] = clean_text(session.client);
    out["program"] = clean_text(session.program);
    out["process"] = session.process;
    out["opened_ms"] = session.opened_ms;
    out["requests"] = session.requests;
    out["events"] = session.events;
    out["traffic"] = session.traffic;
    out["events_sent"] = session.events_sent;
    out["bytes_sent"] = session.bytes_sent;
    out["events_dropped"] = session.events_dropped;
    return out;
}

bool read_session(const json::value& item, session_entry& out)
{
    if (!item.is_object())
        return false;
    const json::object& object = item.get_object();
    return read_u64(object, "number", out.number) &&
           read_text(object, "principal", out.principal) &&
           read_text(object, "transport", out.transport) &&
           read_text(object, "client", out.client) &&
           read_text(object, "program", out.program) &&
           read_u64(object, "process", out.process) &&
           read_u64(object, "opened_ms", out.opened_ms) &&
           read_u64(object, "requests", out.requests) &&
           read_bool(object, "events", out.events) &&
           read_bool(object, "traffic", out.traffic) &&
           read_u64(object, "events_sent", out.events_sent) &&
           read_u64(object, "bytes_sent", out.bytes_sent) &&
           read_u64(object, "events_dropped", out.events_dropped);
}

struct reply_writer
{
    json::object& header;

    void operator()(const created_reply& body) const
    {
        header["reply"] = "created";
        header["created"] = clean_text(body.id);
    }

    void operator()(const subscribed_reply& body) const
    {
        header["reply"] = "subscribed";
        header["interval_ms"] = body.interval_ms;
        header["next_seq"] = body.next_seq;
    }

    void operator()(const sessions_reply& body) const
    {
        header["reply"] = "sessions";
        json::array sessions;
        for (const session_entry& session : body.sessions)
        {
            if (sessions.size() == max_list_size)
                break;
            sessions.emplace_back(write_session(session));
        }
        header["sessions"] = std::move(sessions);
    }

    void operator()(const token_created_reply& body) const
    {
        header["reply"] = "token";
        header["token"] = body.token.expose();
        header["entry"] = write_token(body.entry);
    }

    void operator()(const tokens_reply& body) const
    {
        header["reply"] = "tokens";
        json::array tokens;
        for (const token_entry& token : body.tokens)
        {
            if (tokens.size() == max_list_size)
                break;
            tokens.emplace_back(write_token(token));
        }
        header["tokens"] = std::move(tokens);
    }

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

result<reply_body> read_token_created_reply(const json::object& header)
{
    token_created_reply body;
    const json::value* const token = header.if_contains("token");
    const json::value* const entry = header.if_contains("entry");
    if (token == nullptr || !token->is_string() ||
        token->get_string().size() > max_token_text_size || entry == nullptr ||
        !read_token(*entry, body.entry))
        return malformed();
    body.token = secret_string(std::string_view(token->get_string().data(),
                                                token->get_string().size()));
    return reply_body{std::move(body)};
}

result<reply_body> read_tokens_reply(const json::object& header)
{
    tokens_reply body;
    const json::array* const tokens = read_array(header, "tokens");
    if (tokens == nullptr)
        return malformed();
    body.tokens.reserve(tokens->size());
    for (const json::value& item : *tokens)
    {
        token_entry token;
        if (!read_token(item, token))
            return malformed();
        body.tokens.push_back(std::move(token));
    }
    return reply_body{std::move(body)};
}

result<reply_body> read_subscribed_reply(const json::object& header)
{
    subscribed_reply body;
    if (!read_u32(header, "interval_ms", body.interval_ms) ||
        !read_u64(header, "next_seq", body.next_seq))
        return malformed();
    return reply_body{body};
}

result<reply_body> read_sessions_reply(const json::object& header)
{
    sessions_reply body;
    const json::array* const sessions = read_array(header, "sessions");
    if (sessions == nullptr)
        return malformed();
    body.sessions.reserve(sessions->size());
    for (const json::value& item : *sessions)
    {
        session_entry session;
        if (!read_session(item, session))
            return malformed();
        body.sessions.push_back(std::move(session));
    }
    return reply_body{std::move(body)};
}

// -- events ------------------------------------------------------------------

/// A field of an event is left out when it has nothing to say, which is most
/// of them for most events: a busy port sends thousands a second.
json::object write_event(const event_entry& event)
{
    json::object out;
    out["kind"] = clean_text(event.kind);
    if (!event.port.empty())
        out["port"] = clean_text(event.port);
    if (!event.name.empty())
        out["name"] = clean_text(event.name);
    if (!event.direction.empty())
        out["direction"] = clean_text(event.direction);
    if (event.size != 0)
        out["size"] = event.size;
    if (!event.data.empty())
        out["stored"] = event.data.size();
    if (event.lines != 0)
        out["lines"] = event.lines;
    if (!event.message.empty())
        out["message"] = clean_text(event.message);
    if (!event.error.empty())
        out["error"] = clean_text(event.error);
    if (event.mono_ns != 0)
        out["mono_ns"] = event.mono_ns;
    return out;
}

/// Reads one event and takes its bytes from `blob` at `cursor`.  The events
/// of a batch use the blob in order and without gaps, so a small frame cannot
/// name the same bytes many times and ask the reader for more than it sent.
bool read_event(const json::value& item, const byte_buffer& blob,
                std::size_t& cursor, event_entry& out)
{
    if (!item.is_object())
        return false;
    const json::object& event = item.get_object();
    std::uint64_t stored = 0;
    if (!read_text(event, "kind", out.kind) ||
        !read_optional_text(event, "port", out.port) ||
        !read_optional_text(event, "name", out.name) ||
        !read_optional_text(event, "direction", out.direction) ||
        !read_optional_text(event, "message", out.message) ||
        !read_optional_text(event, "error", out.error))
        return false;
    if ((event.contains("size") && !read_u64(event, "size", out.size)) ||
        (event.contains("stored") && !read_u64(event, "stored", stored)) ||
        (event.contains("lines") && !read_u32(event, "lines", out.lines)) ||
        (event.contains("mono_ns") && !read_u64(event, "mono_ns", out.mono_ns)))
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
    const json::value* const time = header.if_contains("time");
    if (events == nullptr || time == nullptr || !time->is_object() ||
        !read_u64(header, "seq", batch.seq) ||
        !read_u64(header, "revision", batch.revision) ||
        !read_u64(header, "dropped", batch.dropped) ||
        !read_u64(time->get_object(), "mono_ns", batch.mono_ns) ||
        !read_u64(time->get_object(), "wall_ms", batch.wall_ms))
        return malformed();
    batch.events.resize(events->size());
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < events->size(); ++index)
    {
        if (!read_event((*events)[index], value.blob, cursor,
                        batch.events[index]))
            return malformed();
    }
    if (cursor != value.blob.size())
        return malformed();
    return server_message{std::move(batch)};
}

/// The body of a reply of the kind `kind`.
result<reply_body> read_reply_body(const json::object& header,
                                   std::string_view kind);

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

result<reply_body> read_reply_body(const json::object& header,
                                   std::string_view kind)
{
    if (kind == "done")
        return reply_body{done_reply{}};
    if (kind == "hello")
        return read_hello_reply(header);
    if (kind == "status")
        return read_status_reply(header);
    if (kind == "ports")
        return read_ports_reply(header);
    if (kind == "token")
        return read_token_created_reply(header);
    if (kind == "tokens")
        return read_tokens_reply(header);
    if (kind == "subscribed")
        return read_subscribed_reply(header);
    if (kind == "created")
    {
        created_reply body;
        if (!read_text(header, "created", body.id))
            return malformed();
        return reply_body{std::move(body)};
    }
    if (kind == "sessions")
        return read_sessions_reply(header);
    if (kind == "error")
        return read_error_reply(header);
    return malformed();
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
        operation operator()(const tokens_create_request&) const noexcept
        {
            return operation::tokens_create;
        }
        operation operator()(const tokens_list_request&) const noexcept
        {
            return operation::tokens_list;
        }
        operation operator()(const tokens_revoke_request&) const noexcept
        {
            return operation::tokens_revoke;
        }
        operation operator()(const events_subscribe_request&) const noexcept
        {
            return operation::events_subscribe;
        }
        operation operator()(const events_unsubscribe_request&) const noexcept
        {
            return operation::events_unsubscribe;
        }
        operation operator()(const traffic_subscribe_request&) const noexcept
        {
            return operation::traffic_subscribe;
        }
        operation operator()(const traffic_unsubscribe_request&) const noexcept
        {
            return operation::traffic_unsubscribe;
        }
        operation operator()(const sessions_list_request&) const noexcept
        {
            return operation::sessions_list;
        }
        operation operator()(const sessions_kick_request&) const noexcept
        {
            return operation::sessions_kick;
        }
        operation operator()(const ports_create_request&) const noexcept
        {
            return operation::ports_create;
        }
        operation operator()(const ports_remove_request&) const noexcept
        {
            return operation::ports_remove;
        }
        operation operator()(const links_connect_request&) const noexcept
        {
            return operation::links_connect;
        }
        operation operator()(const ports_send_request&) const noexcept
        {
            return operation::ports_send;
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
    if (const auto* sending = std::get_if<ports_send_request>(&value.body))
        out.blob = sending->data;
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

frame encode(const event_batch& value)
{
    json::object header;
    header["event"] = "batch";
    header["seq"] = value.seq;
    header["revision"] = value.revision;
    header["dropped"] = value.dropped;
    json::object time;
    time["mono_ns"] = value.mono_ns;
    time["wall_ms"] = value.wall_ms;
    header["time"] = std::move(time);
    json::array events;
    events.reserve(value.events.size());
    frame out;
    for (const event_entry& event : value.events)
    {
        events.emplace_back(write_event(event));
        out.blob.insert(out.blob.end(), event.data.begin(), event.data.end());
    }
    header["events"] = std::move(events);
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
    // Only `ports.send` carries bytes; a blob anywhere else is a mistake.
    if (wanted->id != operation::ports_send && !value.blob.empty())
        return malformed();
    auto body = read_request_body(wanted->id, *header);
    if (!body)
        return body.error();
    if (auto* sending = std::get_if<ports_send_request>(&*body))
        sending->data = value.blob;
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

    auto body = read_reply_body(*header, kind);
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
    if (const json::value* const event = header->if_contains("event"))
    {
        if (!event->is_string() || event->get_string() != "batch")
            return malformed();
        return read_batch(*header, value);
    }
    auto answer = decode_reply(value);
    if (!answer)
        return answer.error();
    return server_message{std::move(*answer)};
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

std::optional<std::uint64_t> parse_port_id(std::string_view text) noexcept
{
    // "p_" and one to twenty digits: the longest 64 bit number has twenty.
    if (text.size() < 3 || text.size() > 22 || text.substr(0, 2) != "p_")
        return std::nullopt;
    const std::string_view digits = text.substr(2);
    if (digits.size() > 1 && digits.front() == '0')
        return std::nullopt;
    std::uint64_t value = 0;
    for (const char digit : digits)
    {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        const auto next = static_cast<std::uint64_t>(digit - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - next) / 10)
            return std::nullopt;
        value = value * 10 + next;
    }
    return value;
}

} // namespace vss::control
