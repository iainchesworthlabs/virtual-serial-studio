//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli control ...` talks to the user's `vss_gate`.  Like `agent` it makes
// no studio of its own: it only asks the gateway, which asks the agent.
//

#include "control_command.hpp"

#include <vss/agent/proto/error.hpp>
#include <vss/control/client.hpp>
#include <vss/control/proto/error.hpp>
#include <vss/control/proto/message.hpp>
#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss_cli
{
namespace
{

using vss::control::client;
using vss::control::secret_string;

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;
constexpr int exit_usage = 2;
constexpr int exit_not_running = 3;

/// The most bytes of a token file that are looked at.  A token is shorter.
constexpr std::size_t token_file_read_limit = 256;
/// The longest a token may live, in days, and the length of a day.
constexpr unsigned longest_life_days = 365;
constexpr std::uint64_t seconds_per_day = 86400;
/// The last millisecond of 9999-12-31, so that a date always has four digits.
constexpr std::uint64_t last_date_ms = 253402300799999ULL;
/// The longest `watch` waits for a batch before it looks at the clock again.
constexpr std::chrono::milliseconds watch_poll{500};

enum class action_kind
{
    none,
    status,
    stop,
    token_create,
    token_list,
    token_revoke,
    watch,
    sessions,
    kick,
    port_create,
    port_remove,
    link,
    send,
    expect,
    run_begin,
    run_end,
    run_record,
    run_report
};

struct control_arguments
{
    action_kind action = action_kind::none;
    std::string instance;
    std::string label;
    std::vector<std::string> scopes;
    std::uint64_t life_s = 0;
    /// For `token create`: the transports the token is for (`local`,
    /// `loopback`); none for the gateway's default, which is `local`.
    std::vector<std::string> transports;
    std::optional<std::string> out_file;
    std::optional<std::string> token_file;
    /// For the commands on the public endpoint: connect over TCP to this
    /// port on the loopback address instead.  Needs `token_file`.
    std::optional<std::uint16_t> loopback_port;
    std::string token_id;
    /// For `watch`: the bytes as well, the ports to hear about (empty for
    /// all), the time between batches, and when to stop.  0 seconds or
    /// events means no limit.
    bool data = false;
    std::vector<std::string> ports;
    std::uint32_t interval_ms = vss::control::default_stream_interval_ms;
    std::uint32_t seconds = 0;
    std::uint64_t count = 0;
    /// For `kick`: the number of the session.
    std::uint64_t session = 0;
    /// For `port create`, `port remove`, `link` and `send`: the name or kind
    /// of a port, the ports (the second is for `link`), whether a link goes
    /// one way, and the bytes to send.
    std::string name;
    std::string kind;
    std::string port;
    std::string other_port;
    bool one_way = false;
    vss::byte_buffer bytes;
    /// For `expect`: the bytes to wait for, how long to wait in ms, whether
    /// to hear what the port sent, and the port and bytes to send once the
    /// gateway listens (empty for none).
    vss::byte_buffer pattern;
    std::uint32_t timeout_ms = vss::control::default_expect_timeout_ms;
    bool tx = false;
    std::string send_port;
    vss::byte_buffer send_data;
    /// For `run begin`, `run record` and `run report`: the name of the run or
    /// of the check, the id of the run to report (empty for the latest), how
    /// the check ended, what it says and how long it took in ms.  `expect`
    /// uses `label` for what the check is called in the report.
    std::string run_name;
    std::string run_id;
    std::string outcome;
    std::string message;
    std::uint64_t duration_ms = 0;
    bool valid = true;
};

/// What the command line said, before it is understood.
struct raw_arguments
{
    std::vector<std::string> words;
    std::optional<std::string> instance;
    std::optional<std::string> label;
    std::optional<std::string> role;
    std::optional<std::string> scope;
    std::optional<std::string> days;
    std::optional<std::string> out_file;
    std::optional<std::string> token_file;
    std::optional<std::string> loopback_port;
    /// Every `--transport=`; unlike the other options it may come twice.
    std::vector<std::string> transports;
    std::optional<std::string> ports;
    std::optional<std::string> interval;
    std::optional<std::string> seconds;
    std::optional<std::string> count;
    std::optional<std::string> kind;
    std::optional<std::string> hex;
    std::optional<std::string> pattern;
    std::optional<std::string> timeout;
    std::optional<std::string> send_to;
    std::optional<std::string> send;
    std::optional<std::string> outcome;
    std::optional<std::string> message;
    std::optional<std::string> duration;
    bool data = false;
    bool one_way = false;
    bool tx = false;
    bool valid = true;
};

/// Takes the value of `word` into `slot` when it starts with `prefix`.  An
/// option given twice makes the command line invalid.
bool take_option(std::string_view word, std::string_view prefix,
                 std::optional<std::string>& slot, bool& valid)
{
    if (!word.starts_with(prefix))
        return false;
    if (slot)
        valid = false;
    slot = std::string(word.substr(prefix.size()));
    return true;
}

raw_arguments read_words(const std::vector<std::string>& arguments)
{
    raw_arguments raw;
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view word = arguments[index];
        if (take_option(word, "--instance=", raw.instance, raw.valid) ||
            take_option(word, "--label=", raw.label, raw.valid) ||
            take_option(word, "--role=", raw.role, raw.valid) ||
            take_option(word, "--scope=", raw.scope, raw.valid) ||
            take_option(word, "--days=", raw.days, raw.valid) ||
            take_option(word, "--out=", raw.out_file, raw.valid) ||
            take_option(word, "--token-file=", raw.token_file, raw.valid) ||
            take_option(word, "--loopback-port=", raw.loopback_port,
                        raw.valid) ||
            take_option(word, "--ports=", raw.ports, raw.valid) ||
            take_option(word, "--interval=", raw.interval, raw.valid) ||
            take_option(word, "--seconds=", raw.seconds, raw.valid) ||
            take_option(word, "--count=", raw.count, raw.valid) ||
            take_option(word, "--kind=", raw.kind, raw.valid) ||
            take_option(word, "--hex=", raw.hex, raw.valid) ||
            take_option(word, "--pattern=", raw.pattern, raw.valid) ||
            take_option(word, "--timeout=", raw.timeout, raw.valid) ||
            take_option(word, "--send-to=", raw.send_to, raw.valid) ||
            take_option(word, "--send=", raw.send, raw.valid) ||
            take_option(word, "--outcome=", raw.outcome, raw.valid) ||
            take_option(word, "--message=", raw.message, raw.valid) ||
            take_option(word, "--duration=", raw.duration, raw.valid))
            continue;
        constexpr std::string_view transport_prefix = "--transport=";
        if (word.starts_with(transport_prefix))
        {
            raw.transports.emplace_back(word.substr(transport_prefix.size()));
            continue;
        }
        if (word == "--data")
        {
            if (raw.data)
                raw.valid = false;
            raw.data = true;
            continue;
        }
        if (word == "--one-way")
        {
            if (raw.one_way)
                raw.valid = false;
            raw.one_way = true;
            continue;
        }
        if (word == "--tx")
        {
            if (raw.tx)
                raw.valid = false;
            raw.tx = true;
            continue;
        }
        if (word.starts_with("--"))
            raw.valid = false;
        else
            raw.words.emplace_back(word);
    }
    return raw;
}

/// The names between the commas.  Nothing when one of them is empty.
std::optional<std::vector<std::string>> split_names(std::string_view text)
{
    std::vector<std::string> names;
    for (;;)
    {
        const std::size_t comma = text.find(',');
        const std::string_view name = text.substr(0, comma);
        if (name.empty())
            return std::nullopt;
        names.emplace_back(name);
        if (comma == std::string_view::npos)
            return names;
        text.remove_prefix(comma + 1);
    }
}

/// The pieces between the commas, empty ones included: the gateway decides
/// what a port id may be.
std::vector<std::string> split_commas(std::string_view text)
{
    std::vector<std::string> pieces;
    for (;;)
    {
        const std::size_t comma = text.find(',');
        pieces.emplace_back(text.substr(0, comma));
        if (comma == std::string_view::npos)
            return pieces;
        text.remove_prefix(comma + 1);
    }
}

/// The whole of `text` as a number, which is all digits.  Nothing for
/// anything else, or a number that does not fit.
template <class Number>
std::optional<Number> whole_number(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    Number number{};
    const char* const first = &text.front();
    const char* const end = first + text.size();
    const auto parsed = std::from_chars(first, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::nullopt;
    return number;
}

/// The port of `--loopback-port=`: all digits, 1 to 65535.  Nothing for
/// anything else.
std::optional<std::uint16_t> loopback_port_from(std::string_view text)
{
    const auto number = whole_number<std::uint16_t>(text);
    if (!number || *number == 0)
        return std::nullopt;
    return number;
}

/// The scope names of a role, or nothing for a word that is not a role.
std::optional<std::vector<std::string>> role_scopes(std::string_view role)
{
    if (role == "monitor")
        return vss::control::scope_names(vss::control::role_monitor());
    if (role == "control")
        return vss::control::scope_names(vss::control::role_control());
    if (role == "control-apply")
        return vss::control::scope_names(vss::control::role_control_apply());
    return std::nullopt;
}

/// The lifetime in seconds for `--days`: 1 to 365 days, all digits.
std::optional<std::uint64_t> life_from_days(const std::string& days)
{
    unsigned count = 0;
    const char* const end = days.data() + days.size();
    const auto parsed = std::from_chars(days.data(), end, count);
    if (parsed.ec != std::errc{} || parsed.ptr != end || count < 1 ||
        count > longest_life_days)
        return std::nullopt;
    return std::uint64_t{count} * seconds_per_day;
}

/// The value of a hexadecimal digit, or -1 for a character that is not one.
int hex_value(char letter) noexcept
{
    if (letter >= '0' && letter <= '9')
        return letter - '0';
    if (letter >= 'a' && letter <= 'f')
        return letter - 'a' + 10;
    if (letter >= 'A' && letter <= 'F')
        return letter - 'A' + 10;
    return -1;
}

/// The bytes that `text` spells: an even number of hex digits, at least two,
/// with no separator.  Nothing for anything else.
std::optional<vss::byte_buffer> hex_bytes(std::string_view text)
{
    if (text.empty() || text.size() % 2 != 0)
        return std::nullopt;
    vss::byte_buffer bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t index = 0; index < text.size(); index += 2)
    {
        const int high = hex_value(text[index]);
        const int low = hex_value(text[index + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes.push_back(static_cast<vss::byte>(high * 16 + low));
    }
    return bytes;
}

/// Reads the options of `token create` into `parsed`.  False when they are
/// not what the command takes.
bool read_create_options(const raw_arguments& raw, control_arguments& parsed)
{
    // A label, and exactly one of the role and the scopes.
    if (!raw.label || raw.label->empty())
        return false;
    if (raw.role.has_value() == raw.scope.has_value())
        return false;
    parsed.label = *raw.label;
    const auto names =
        raw.role ? role_scopes(*raw.role) : split_names(*raw.scope);
    if (!names)
        return false;
    parsed.scopes = *names;
    if (raw.days)
    {
        const auto life = life_from_days(*raw.days);
        if (!life)
            return false;
        parsed.life_s = *life;
    }
    // One or both of the two transports, each once.
    parsed.transports.reserve(raw.transports.size());
    for (const std::string& name : raw.transports)
    {
        if ((name != "local" && name != "loopback") ||
            std::ranges::find(parsed.transports, name) !=
                parsed.transports.end())
            return false;
        parsed.transports.push_back(name);
    }
    parsed.out_file = raw.out_file;
    return true;
}

/// True when none of the options that only `token create` takes is there.
bool no_create_options(const raw_arguments& raw)
{
    return !raw.label && !raw.role && !raw.scope && !raw.days &&
           !raw.out_file && raw.transports.empty();
}

/// True when none of the options that only `watch` takes is there.
bool no_watch_options(const raw_arguments& raw)
{
    return !raw.data && !raw.ports && !raw.interval && !raw.seconds &&
           !raw.count;
}

/// True when none of the options that only `port create`, `link` and `send`
/// take is there.
bool no_port_options(const raw_arguments& raw)
{
    return !raw.kind && !raw.hex && !raw.one_way;
}

/// True when none of the options that only `expect` takes is there.
bool no_expect_options(const raw_arguments& raw)
{
    return !raw.pattern && !raw.timeout && !raw.send_to && !raw.send && !raw.tx;
}

/// True when none of the options that only `token create` takes is there,
/// but the label, which `expect` takes as well.
bool no_create_options_but_label(const raw_arguments& raw)
{
    return !raw.role && !raw.scope && !raw.days && !raw.out_file &&
           raw.transports.empty();
}

/// True when none of the options that only `run record` takes is there.
bool no_run_options(const raw_arguments& raw)
{
    return !raw.outcome && !raw.message && !raw.duration;
}

/// True when none of the options of `token create`, of `watch`, of `expect`
/// and of `run record` is there.
bool no_foreign_options(const raw_arguments& raw)
{
    return no_create_options(raw) && no_watch_options(raw) &&
           no_expect_options(raw) && no_run_options(raw);
}

/// Reads the options of `watch` into `parsed`.  False when they are not what
/// the command takes.  The options that say how long to run are a number of
/// at least 1.
bool read_watch_options(const raw_arguments& raw, control_arguments& parsed)
{
    parsed.data = raw.data;
    if (raw.ports)
        parsed.ports = split_commas(*raw.ports);
    if (raw.interval)
    {
        const auto interval = whole_number<std::uint32_t>(*raw.interval);
        if (!interval)
            return false;
        parsed.interval_ms = *interval;
    }
    if (raw.seconds)
    {
        const auto seconds = whole_number<std::uint32_t>(*raw.seconds);
        if (!seconds || *seconds < 1)
            return false;
        parsed.seconds = *seconds;
    }
    if (raw.count)
    {
        const auto count = whole_number<std::uint64_t>(*raw.count);
        if (!count || *count < 1)
            return false;
        parsed.count = *count;
    }
    return true;
}

/// Reads the options of `expect` into `parsed`.  False when they are not what
/// the command takes: a pattern and a send that are not hex, a timeout that
/// is not a number of at least 1, or one of `--send-to` and `--send` alone.
/// Whether the timeout is too long is for the gateway to say.
bool read_expect_options(const raw_arguments& raw, control_arguments& parsed)
{
    if (!raw.pattern || raw.send_to.has_value() != raw.send.has_value())
        return false;
    auto pattern = hex_bytes(*raw.pattern);
    if (!pattern)
        return false;
    parsed.pattern = std::move(*pattern);
    if (raw.timeout)
    {
        const auto timeout = whole_number<std::uint32_t>(*raw.timeout);
        if (!timeout || *timeout < 1)
            return false;
        parsed.timeout_ms = *timeout;
    }
    parsed.tx = raw.tx;
    if (raw.label)
    {
        if (raw.label->empty())
            return false;
        parsed.label = *raw.label;
    }
    if (raw.send_to && raw.send)
    {
        auto data = hex_bytes(*raw.send);
        if (!data || raw.send_to->empty())
            return false;
        parsed.send_port = *raw.send_to;
        parsed.send_data = std::move(*data);
    }
    return true;
}

/// True when `outcome` is one of the four ways a check may end.
bool is_outcome(std::string_view outcome) noexcept
{
    return outcome == "passed" || outcome == "failed" || outcome == "error" ||
           outcome == "skipped";
}

/// Reads the words and options of `run <what> ...` into `parsed`.  False when
/// they are not what the command takes: a name or an id that is empty, an
/// outcome that is missing or not one of the four, a duration that is not a
/// number, or an option that belongs to another command.  Whether a name is
/// too long is for the gateway to say.
bool read_run_command(const raw_arguments& raw, control_arguments& parsed)
{
    const std::vector<std::string>& words = raw.words;
    if (words.size() < 2 || raw.label || raw.role || raw.scope || raw.days ||
        !raw.transports.empty() || !no_watch_options(raw) ||
        !no_port_options(raw) || !no_expect_options(raw))
        return false;
    const std::string& what = words[1];
    parsed.token_file = raw.token_file;
    // Only `report` writes to a file.
    if (raw.out_file && what != "report")
        return false;
    if (what == "begin")
    {
        if (words.size() != 3 || words[2].empty() || !no_run_options(raw))
            return false;
        parsed.action = action_kind::run_begin;
        parsed.run_name = words[2];
        return true;
    }
    if (what == "end")
    {
        if (words.size() != 2 || !no_run_options(raw))
            return false;
        parsed.action = action_kind::run_end;
        return true;
    }
    if (what == "record")
    {
        if (words.size() != 3 || words[2].empty() || !raw.outcome ||
            !is_outcome(*raw.outcome))
            return false;
        parsed.action = action_kind::run_record;
        parsed.run_name = words[2];
        parsed.outcome = *raw.outcome;
        parsed.message = raw.message.value_or(std::string());
        if (raw.duration)
        {
            const auto duration = whole_number<std::uint64_t>(*raw.duration);
            if (!duration)
                return false;
            parsed.duration_ms = *duration;
        }
        return true;
    }
    if (what == "report")
    {
        if (words.size() > 3 || (words.size() == 3 && words[2].empty()) ||
            !no_run_options(raw) || (raw.out_file && raw.out_file->empty()))
            return false;
        parsed.action = action_kind::run_report;
        parsed.out_file = raw.out_file;
        if (words.size() == 3)
            parsed.run_id = words[2];
        return true;
    }
    return false;
}

control_arguments parse_command(const raw_arguments& raw)
{
    control_arguments parsed;
    parsed.valid = false;
    if (!raw.valid || raw.words.empty())
        return parsed;
    if (raw.instance)
        parsed.instance = *raw.instance;

    const std::vector<std::string>& words = raw.words;
    if (words.size() == 1 && (words[0] == "status" || words[0] == "stop"))
    {
        const bool is_status = words[0] == "status";
        // Only `status` takes a token; no command takes the options of
        // `token create` or of `watch`.
        if (!no_foreign_options(raw) || !no_port_options(raw) ||
            (!is_status && raw.token_file))
            return parsed;
        parsed.action = is_status ? action_kind::status : action_kind::stop;
        parsed.token_file = raw.token_file;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 1 && words[0] == "watch")
    {
        if (!no_create_options(raw) || !no_port_options(raw) ||
            !no_expect_options(raw) || !no_run_options(raw))
            return parsed;
        parsed.action = action_kind::watch;
        parsed.token_file = raw.token_file;
        parsed.valid = read_watch_options(raw, parsed);
        return parsed;
    }
    if (words.size() == 1 && words[0] == "sessions")
    {
        if (!no_foreign_options(raw) || !no_port_options(raw) || raw.token_file)
            return parsed;
        parsed.action = action_kind::sessions;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 2 && words[0] == "kick")
    {
        const auto number = whole_number<std::uint64_t>(words[1]);
        if (!number || !no_foreign_options(raw) || !no_port_options(raw) ||
            raw.token_file)
            return parsed;
        parsed.action = action_kind::kick;
        parsed.session = *number;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 3 && words[0] == "port" &&
        (words[1] == "create" || words[1] == "remove"))
    {
        // A port is made with a name and removed by its id; only `create`
        // takes a kind.
        const bool create = words[1] == "create";
        if (!no_foreign_options(raw) || raw.hex || raw.one_way ||
            (!create && raw.kind) || words[2].empty() ||
            (raw.kind && raw.kind->empty()))
            return parsed;
        parsed.action =
            create ? action_kind::port_create : action_kind::port_remove;
        if (create)
        {
            parsed.name = words[2];
            parsed.kind = raw.kind.value_or(std::string());
        }
        else
        {
            parsed.port = words[2];
        }
        parsed.token_file = raw.token_file;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 3 && words[0] == "link")
    {
        if (!no_foreign_options(raw) || raw.kind || raw.hex)
            return parsed;
        parsed.action = action_kind::link;
        parsed.port = words[1];
        parsed.other_port = words[2];
        parsed.one_way = raw.one_way;
        parsed.token_file = raw.token_file;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 2 && words[0] == "send")
    {
        if (!no_foreign_options(raw) || raw.kind || raw.one_way || !raw.hex)
            return parsed;
        auto bytes = hex_bytes(*raw.hex);
        if (!bytes)
            return parsed;
        parsed.action = action_kind::send;
        parsed.port = words[1];
        parsed.bytes = std::move(*bytes);
        parsed.token_file = raw.token_file;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 2 && words[0] == "expect")
    {
        if (!no_create_options_but_label(raw) || !no_watch_options(raw) ||
            !no_port_options(raw) || !no_run_options(raw))
            return parsed;
        parsed.action = action_kind::expect;
        parsed.port = words[1];
        parsed.token_file = raw.token_file;
        parsed.valid = read_expect_options(raw, parsed);
        return parsed;
    }
    if (!words.empty() && words[0] == "run")
    {
        parsed.valid = read_run_command(raw, parsed);
        return parsed;
    }
    if (words.size() < 2 || words[0] != "token" || raw.token_file ||
        !no_watch_options(raw) || !no_port_options(raw) ||
        !no_expect_options(raw) || !no_run_options(raw))
        return parsed;
    if (words.size() == 2 && words[1] == "create")
    {
        parsed.action = action_kind::token_create;
        parsed.valid = read_create_options(raw, parsed);
    }
    else if (words.size() == 2 && words[1] == "list" && no_create_options(raw))
    {
        parsed.action = action_kind::token_list;
        parsed.valid = true;
    }
    else if (words.size() == 3 && words[1] == "revoke" &&
             no_create_options(raw) && !words[2].empty())
    {
        parsed.action = action_kind::token_revoke;
        parsed.token_id = words[2];
        parsed.valid = true;
    }
    return parsed;
}

/// True for the commands that are for this user on the administration
/// endpoint, which takes no token and is not served over TCP.  The others use
/// the public endpoint.
bool is_administration(action_kind action) noexcept
{
    return action == action_kind::stop || action == action_kind::token_create ||
           action == action_kind::token_list ||
           action == action_kind::token_revoke ||
           action == action_kind::sessions || action == action_kind::kick;
}

/// What the command line says, as `parse_command` understood it and with
/// `--loopback-port=` read.  It belongs to the commands on the public
/// endpoint and needs a token file: over TCP nobody is the local user.
control_arguments parse(const std::vector<std::string>& arguments)
{
    const raw_arguments raw = read_words(arguments);
    control_arguments parsed = parse_command(raw);
    if (!parsed.valid || !raw.loopback_port)
        return parsed;
    const auto port = loopback_port_from(*raw.loopback_port);
    if (!port || is_administration(parsed.action) || !parsed.token_file)
        parsed.valid = false;
    else
        parsed.loopback_port = *port;
    return parsed;
}

const char* action_name(action_kind action) noexcept
{
    switch (action)
    {
    case action_kind::status:
        return "status";
    case action_kind::stop:
        return "stop";
    case action_kind::token_create:
        return "token create";
    case action_kind::token_list:
        return "token list";
    case action_kind::token_revoke:
        return "token revoke";
    case action_kind::watch:
        return "watch";
    case action_kind::sessions:
        return "sessions";
    case action_kind::kick:
        return "kick";
    case action_kind::port_create:
        return "port create";
    case action_kind::port_remove:
        return "port remove";
    case action_kind::link:
        return "link";
    case action_kind::send:
        return "send";
    case action_kind::expect:
        return "expect";
    case action_kind::run_begin:
        return "run begin";
    case action_kind::run_end:
        return "run end";
    case action_kind::run_record:
        return "run record";
    case action_kind::run_report:
        return "run report";
    case action_kind::none:
        break;
    }
    return "control";
}

int report(const char* what, const vss::error_code& error,
           const client* gateway)
{
    // The gateway's own words when it sent some; the code's otherwise.
    const std::string message =
        gateway != nullptr && !gateway->last_error_message().empty()
            ? gateway->last_error_message()
            : error.message();
    std::fprintf(stderr, "control %s failed: %s\n", what, message.c_str());
    return exit_refused;
}

/// `message`, and for a refusal of the gateway or of the engine its code name
/// too, as in `not allowed (forbidden)` or `the engine refused the request
/// (engine.not_found)`: the wording alone does not say which it was.
std::string with_code_name(std::string message, const vss::error_code& error)
{
    if (error.category() == vss::control::api_category())
    {
        message += " (";
        message += vss::control::code_name(
            static_cast<vss::control::api_errc>(error.value()));
        message += ')';
    }
    else if (error.category() == vss::error_category())
    {
        message += " (";
        message += vss::control::engine_error_name(error);
        message += ')';
    }
    return message;
}

/// As `report`, with the code name as `with_code_name` gives it.
int report_with_code(const char* what, const vss::error_code& error,
                     const client& gateway)
{
    std::string message = !gateway.last_error_message().empty()
                              ? gateway.last_error_message()
                              : error.message();
    message = with_code_name(std::move(message), error);
    std::fprintf(stderr, "control %s failed: %s\n", what, message.c_str());
    return exit_refused;
}

/// As `report` for a connection that was refused, with the code name as
/// `with_code_name` gives it: a token that is wrong, revoked or made for the
/// other transport is told `not authenticated (unauthenticated)`.
int report_refused(const char* what, const vss::error_code& error)
{
    const std::string message = with_code_name(error.message(), error);
    std::fprintf(stderr, "control %s failed: %s\n", what, message.c_str());
    return exit_refused;
}

/// `text` for a line that is read by a program: a letter, a digit, `.`, `_`,
/// `-` or `:` stays and anything else becomes `?`.  `-` for no text.  The
/// text comes from outside, so its shape is not trusted.
std::string plain_word(std::string_view text)
{
    if (text.empty())
        return "-";
    std::string word(text);
    for (char& letter : word)
    {
        const bool plain = (letter >= 'a' && letter <= 'z') ||
                           (letter >= 'A' && letter <= 'Z') ||
                           (letter >= '0' && letter <= '9') || letter == '.' ||
                           letter == '_' || letter == '-' || letter == ':';
        if (!plain)
            letter = '?';
    }
    return word;
}

/// `text` with every byte below 0x20 and 0x7f replaced by `?`.  Bytes of 0x80
/// and above stay, so that a name in UTF-8 reads as it was written.
std::string printable(std::string_view text)
{
    std::string shown(text);
    for (char& letter : shown)
    {
        const auto byte = static_cast<unsigned char>(letter);
        if (byte < 0x20 || byte == 0x7f)
            letter = '?';
    }
    return shown;
}

std::string join_names(const std::vector<std::string>& names,
                       std::string_view separator)
{
    std::string joined;
    for (const std::string& name : names)
    {
        if (!joined.empty())
            joined += separator;
        joined += name;
    }
    return joined;
}

/// The names of the scopes, or `nothing` when the connection holds none.
std::string join_scopes(const std::vector<std::string>& scopes)
{
    std::string joined = join_names(scopes, ", ");
    return joined.empty() ? "nothing" : joined;
}

/// The UTC date of a time given in milliseconds since 1970, as 2026-11-13.
std::string date_text(std::uint64_t since_epoch_ms)
{
    const std::uint64_t capped =
        since_epoch_ms > last_date_ms ? last_date_ms : since_epoch_ms;
    const std::chrono::sys_time<std::chrono::milliseconds> moment{
        std::chrono::milliseconds{
            static_cast<std::chrono::milliseconds::rep>(capped)}};
    const std::chrono::year_month_day day{
        std::chrono::floor<std::chrono::days>(moment)};
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%04d-%02u-%02u",
                  static_cast<int>(day.year()),
                  static_cast<unsigned>(day.month()),
                  static_cast<unsigned>(day.day()));
    return std::string(text.data());
}

int status(client& gateway)
{
    const auto state = gateway.status();
    if (!state)
        return report("status", state.error(), &gateway);
    const auto& hello = gateway.gateway();
    std::puts("Remote control: local only");
    std::printf("Gateway: %s, API %u\n", hello.version.c_str(), hello.api);
    std::printf("You are: %s\n", hello.principal.c_str());
    std::printf("You may: %s\n", join_scopes(hello.scopes).c_str());
    std::printf("Sessions: %llu\n",
                static_cast<unsigned long long>(state->sessions));
    if (state->agent_running)
        std::printf("Agent: running, %s\n", state->agent_version.c_str());
    else
        std::puts("Agent: not running");
    std::printf("Uptime: %llu s\n",
                static_cast<unsigned long long>(state->uptime_ms / 1000));
    return exit_ok;
}

int stop(client& gateway)
{
    const auto stopped = gateway.stop();
    if (!stopped)
        return report("stop", stopped.error(), &gateway);
    std::puts("Remote control stopped");
    return exit_ok;
}

/// Shows the token on the screen.  The place where a token that is not
/// written to a file leaves this program.
void print_token(const secret_string& token)
{
    const std::string_view text = token.expose();
    std::fprintf(stdout, "%.*s\n", static_cast<int>(text.size()), text.data());
}

/// Writes the token and a line ending to `path`, which only its owner may
/// read.  The place where a token that is asked for in a file leaves this
/// program.  False when the file cannot be written.
bool write_token_file(const std::string& path, const secret_string& token)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    // The file is empty yet, so the token is never readable by others.
    std::error_code ignored;
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace,
                                 ignored);
    file << token.expose() << '\n';
    file.close();
    return !file.fail();
}

int token_create(client& gateway, const control_arguments& args)
{
    const auto made = gateway.create_token(args.label, args.scopes, args.life_s,
                                           args.transports);
    if (!made)
        return report("token create", made.error(), &gateway);
    const vss::control::token_entry& entry = made->entry;
    std::printf("Token %s created for \"%s\"\n", entry.id.c_str(),
                entry.label.c_str());
    std::printf("Scopes: %s\n", join_names(entry.scopes, ", ").c_str());
    std::printf("Expires: %s\n", date_text(entry.expires_ms).c_str());

    int outcome = exit_ok;
    bool written = false;
    if (args.out_file)
    {
        written = write_token_file(*args.out_file, made->token);
        if (written)
        {
            std::printf("Token written to %s\n", args.out_file->c_str());
        }
        else
        {
            std::fprintf(stderr,
                         "control token create failed: cannot write the "
                         "token to %s\n",
                         args.out_file->c_str());
            outcome = exit_refused;
        }
    }
    // Without a file, or when it could not be written, this is the only
    // chance to see the token.
    if (!written)
    {
        std::puts("This is the only time the token is shown.  Keep it like a "
                  "password.");
        print_token(made->token);
    }
    return outcome;
}

int token_list(client& gateway)
{
    const auto listed = gateway.list_tokens();
    if (!listed)
        return report("token list", listed.error(), &gateway);
    if (listed->tokens.empty())
    {
        std::puts("No tokens");
        return exit_ok;
    }
    for (const vss::control::token_entry& entry : listed->tokens)
    {
        std::printf("%s  %s  %s  expires %s  %s", entry.id.c_str(),
                    entry.label.c_str(), join_names(entry.scopes, ",").c_str(),
                    date_text(entry.expires_ms).c_str(),
                    entry.revoked ? "revoked" : "active");
        if (entry.last_used_ms != 0)
            std::printf("  last used %s",
                        date_text(entry.last_used_ms).c_str());
        std::printf("  transports %s",
                    plain_word(join_names(entry.transports, ",")).c_str());
        std::puts("");
    }
    return exit_ok;
}

int token_revoke(client& gateway, const std::string& id)
{
    const auto revoked = gateway.revoke_token(id);
    if (!revoked)
    {
        if (revoked.error() == vss::control::api_errc::not_found)
        {
            std::fputs("control token revoke failed: no token has that id\n",
                       stderr);
            return exit_refused;
        }
        return report("token revoke", revoked.error(), &gateway);
    }
    std::printf("Token %s revoked\n", id.c_str());
    return exit_ok;
}

unsigned long long number_of(std::uint64_t value) noexcept
{
    return static_cast<unsigned long long>(value);
}

/// One line for an event, as `vss_cli control` is documented to print it.
/// No port name and no message: they are text from outside, and the line has
/// to stay in one piece for a program that reads it.
void print_event(std::uint64_t batch_seq,
                 const vss::control::event_entry& event)
{
    const std::string bytes =
        event.data.empty() ? std::string("-") : vss::to_hex(event.data, '\0');
    std::printf("%llu %llu %s %s %s %llu %s\n", number_of(batch_seq),
                number_of(event.mono_ns), plain_word(event.kind).c_str(),
                plain_word(event.port).c_str(),
                plain_word(event.direction).c_str(), number_of(event.size),
                bytes.c_str());
}

/// Prints what happens to the ports until `args.seconds` have passed or
/// `args.count` events are out, or, with neither, until the gateway ends the
/// session.
int watch(client& gateway, const control_arguments& args)
{
    vss::control::stream_options stream;
    stream.interval_ms = args.interval_ms;
    stream.ports = args.ports;
    const auto subscribed = args.data ? gateway.subscribe_traffic(stream)
                                      : gateway.subscribe_events(stream);
    if (!subscribed)
        return report_with_code("watch", subscribed.error(), gateway);

    const auto started = std::chrono::steady_clock::now();
    const std::chrono::seconds run_time{args.seconds};
    std::uint64_t printed = 0;
    for (;;)
    {
        std::chrono::milliseconds limit = watch_poll;
        if (args.seconds != 0)
        {
            const auto passed = std::chrono::steady_clock::now() - started;
            if (passed >= run_time)
                return exit_ok;
            limit =
                std::min(limit, std::chrono::ceil<std::chrono::milliseconds>(
                                    run_time - passed));
        }
        const auto batch = gateway.next_events(limit);
        if (!batch)
        {
            if (batch.error() == vss::errc::timeout)
                continue;
            return report_with_code("watch", batch.error(), gateway);
        }
        if (batch->dropped != 0)
            std::printf("# lost %llu events before batch %llu\n",
                        number_of(batch->dropped), number_of(batch->seq));
        for (const auto& event : batch->events)
        {
            print_event(batch->seq, event);
            ++printed;
            if (args.count != 0 && printed >= args.count)
            {
                std::fflush(stdout);
                return exit_ok;
            }
        }
        std::fflush(stdout);
    }
}

int sessions(client& gateway)
{
    const auto listed = gateway.list_sessions();
    if (!listed)
        return report_with_code("sessions", listed.error(), gateway);
    for (const vss::control::session_entry& entry : listed->sessions)
    {
        // The client name is free text and goes last.
        std::printf(
            "%llu %s %s %s %s %llu %llu %llu %llu %s\n",
            number_of(entry.number), plain_word(entry.principal).c_str(),
            plain_word(entry.transport).c_str(), entry.events ? "yes" : "no",
            entry.traffic ? "yes" : "no", number_of(entry.requests),
            number_of(entry.events_sent), number_of(entry.events_dropped),
            number_of(entry.process), printable(entry.client).c_str());
    }
    return exit_ok;
}

int kick(client& gateway, std::uint64_t number)
{
    const auto kicked = gateway.kick_session(number);
    if (!kicked)
    {
        if (kicked.error() == vss::control::api_errc::not_found)
        {
            std::fputs("control kick failed: no session has that number\n",
                       stderr);
            return exit_refused;
        }
        return report_with_code("kick", kicked.error(), gateway);
    }
    return exit_ok;
}

int port_create(client& gateway, const control_arguments& args)
{
    const auto made = gateway.create_port(args.name, args.kind);
    if (!made)
        return report_with_code("port create", made.error(), gateway);
    std::printf("%s\n", made->c_str());
    return exit_ok;
}

int port_remove(client& gateway, const std::string& port)
{
    const auto removed = gateway.remove_port(port);
    if (!removed)
        return report_with_code("port remove", removed.error(), gateway);
    return exit_ok;
}

int link_ports(client& gateway, const control_arguments& args)
{
    const auto made =
        gateway.connect_ports(args.port, args.other_port, args.one_way);
    if (!made)
        return report_with_code("link", made.error(), gateway);
    std::printf("%s\n", made->c_str());
    return exit_ok;
}

int send_bytes(client& gateway, const control_arguments& args)
{
    const auto sent = gateway.send(args.port, args.bytes);
    if (!sent)
        return report_with_code("send", sent.error(), gateway);
    return exit_ok;
}

/// Waits for the pattern and prints where it went by.  An answer that the
/// pattern did not come is an error, as any other the gateway gives.
int expect_bytes(client& gateway, const control_arguments& args)
{
    vss::control::expect_bytes_request wanted;
    wanted.port = args.port;
    wanted.pattern = args.pattern;
    if (args.tx)
        wanted.direction = "tx";
    wanted.timeout_ms = args.timeout_ms;
    wanted.send_port = args.send_port;
    wanted.send = args.send_data;
    wanted.label = args.label;
    const auto matched = gateway.expect_bytes(wanted);
    if (!matched)
        return report_with_code("expect", matched.error(), gateway);
    std::printf("matched %llu %llu %llu\n", number_of(matched->offset),
                number_of(matched->mono_ns), number_of(matched->events_lost));
    return exit_ok;
}

/// Opens the run and prints its id.
int run_begin(client& gateway, const control_arguments& args)
{
    const auto begun = gateway.begin_run(args.run_name);
    if (!begun)
        return report_with_code("run begin", begun.error(), gateway);
    std::printf("%s\n", plain_word(begun->run).c_str());
    return exit_ok;
}

/// Ends the run and prints its counts on one line.  The command reports and
/// does not judge: the exit code is 0 whatever the counts are.
int run_end(client& gateway)
{
    const auto ended = gateway.end_run();
    if (!ended)
        return report_with_code("run end", ended.error(), gateway);
    std::printf("%s tests=%llu failures=%llu errors=%llu skipped=%llu "
                "duration_ms=%llu\n",
                plain_word(ended->run).c_str(), number_of(ended->tests),
                number_of(ended->failures), number_of(ended->errors),
                number_of(ended->skipped), number_of(ended->duration_ms));
    return exit_ok;
}

int run_record(client& gateway, const control_arguments& args)
{
    vss::control::runs_record_request noted;
    noted.name = args.run_name;
    noted.outcome = args.outcome;
    noted.message = args.message;
    noted.duration_ms = args.duration_ms;
    const auto recorded = gateway.record_in_run(noted);
    if (!recorded)
        return report_with_code("run record", recorded.error(), gateway);
    return exit_ok;
}

/// Writes `text` to `path`, byte for byte.  The file is made or emptied
/// first.  False when it cannot be written.
bool write_report_file(const std::string& path, const std::string& text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();
    return !file.fail();
}

/// Gives the report of a run: to the file named by `--out`, or to the
/// screen.  Nothing else is printed, so that the output is the report.
int run_report(client& gateway, const control_arguments& args)
{
    const auto xml = gateway.report_junit(args.run_id);
    if (!xml)
        return report_with_code("run report", xml.error(), gateway);
    if (args.out_file)
    {
        if (!write_report_file(*args.out_file, *xml))
        {
            std::fprintf(stderr,
                         "control run report failed: cannot write the "
                         "report to %s\n",
                         args.out_file->c_str());
            return exit_refused;
        }
        return exit_ok;
    }
    const std::size_t written =
        std::fwrite(xml->data(), 1, xml->size(), stdout);
    if (written != xml->size() || std::fflush(stdout) != 0)
    {
        std::fputs("control run report failed: cannot write the report\n",
                   stderr);
        return exit_refused;
    }
    return exit_ok;
}

/// The token on the first line of the file at `path`, without its line
/// ending.  Nothing when the file cannot be read or holds no token.  The text
/// is read into a buffer that is wiped before this returns.
std::optional<secret_string> read_token_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    std::string buffer(token_file_read_limit, '\0');
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(file.gcount());

    std::optional<secret_string> token;
    if (!file.bad())
    {
        const std::string_view text(buffer.data(), count);
        std::size_t end = text.find('\n');
        // A first line that fills the buffer is too long to be a token.
        const bool complete =
            end != std::string_view::npos || count < buffer.size();
        if (end == std::string_view::npos)
            end = count;
        if (end > 0 && buffer[end - 1] == '\r')
            --end;
        if (complete && end > 0)
            token.emplace(text.substr(0, end));
    }
    vss::control::secure_wipe(buffer.data(), buffer.size());
    return token;
}

} // namespace

const char* control_usage() noexcept
{
    return "  vss_cli control status [--instance=<name>] "
           "[--token-file=<file>]\n"
           "  vss_cli control stop [--instance=<name>]\n"
           "      Ask this user's control gateway, which is `vss_gate` and is\n"
           "      started by hand for now.  status says who you are to it and\n"
           "      what you may do; stop closes its sessions and ends it, and\n"
           "      closes no port.  Exit code 3 means no gateway is running.\n"
           "      With --token-file status connects as the token whose first\n"
           "      line the file holds, not as this user.  A token is never\n"
           "      given on the command line, where other programs can read\n"
           "      it.\n"
           "\n"
           "  vss_cli control token create --label=<text>\n"
           "           (--role=monitor|control|control-apply |\n"
           "            --scope=<name>[,<name>...])\n"
           "           [--days=<n>] [--out=<file>] [--instance=<name>]\n"
           "           [--transport=local|loopback]...\n"
           "  vss_cli control token list [--instance=<name>]\n"
           "  vss_cli control token revoke <id> [--instance=<name>]\n"
           "      Make, list and revoke the tokens that let a program connect\n"
           "      as something other than this user.  create needs a label\n"
           "      and either a role or a list of scope names.  The token\n"
           "      lives --days days (1 to 365, 30 by default).  It is shown\n"
           "      once, on the screen or, with --out, in a file; keep it like\n"
           "      a password.  --transport says where the token is good: "
           "local\n"
           "      (the pipe or socket, which is the default), loopback (TCP,\n"
           "      see --loopback-port below), or both when it is given twice.\n"
           "      list shows the records, never the tokens, and where each is\n"
           "      good.  revoke ends a token and its sessions.\n"
           "\n"
           "  vss_cli control watch [--data] [--ports=<id>[,<id>...]]\n"
           "           [--interval=<ms>] [--seconds=<n>] [--count=<n>]\n"
           "           [--instance=<name>] [--token-file=<file>]\n"
           "  vss_cli control sessions [--instance=<name>]\n"
           "  vss_cli control kick <number> [--instance=<name>]\n"
           "      watch prints what happens to the ports, one line for each\n"
           "      event: batch number, time in ns on the steady clock of the\n"
           "      computer, kind, port id, direction, size and the bytes in\n"
           "      hex.  A - stands for what an event has not got.  With "
           "--data\n"
           "      the bytes are given, which needs the scope monitor.data;\n"
           "      without it none are.  --ports names the ports to hear about\n"
           "      (p_1,p_2), all of them by default.  --interval is the time\n"
           "      between batches in ms (250 by default).  A line that starts\n"
           "      with # says how many events were lost before a batch.  It\n"
           "      ends after --seconds or --count events, and otherwise when\n"
           "      the gateway ends the session, which is an error.  sessions\n"
           "      lists who is connected: number, who, how, events, traffic,\n"
           "      requests, events sent, events lost, process and the name\n"
           "      the program gave itself.  kick closes one session by its\n"
           "      number.\n"
           "\n"
           "  vss_cli control port create <name> [--kind=<kind>]\n"
           "           [--instance=<name>] [--token-file=<file>]\n"
           "  vss_cli control port remove <id> [--instance=<name>]\n"
           "           [--token-file=<file>]\n"
           "  vss_cli control link <id> <id> [--one-way] [--instance=<name>]\n"
           "           [--token-file=<file>]\n"
           "  vss_cli control send <id> --hex=<digits> [--instance=<name>]\n"
           "           [--token-file=<file>]\n"
           "      Make and remove ports, link them and send bytes to one.\n"
           "      port create prints the id of the new port (p_3), and link\n"
           "      prints the id of the link (l_2).  A link joins the two\n"
           "      ports both ways, or with --one-way from the first to the\n"
           "      second.  send takes the bytes as hex digits, two for each\n"
           "      byte and no spaces, as in --hex=00ff68690d0a.  It sends\n"
           "      64 KiB at most.  These need the scope control.ports, and\n"
           "      send needs control.data; the default role of this user\n"
           "      (monitor) has neither.  When the engine says no, the line\n"
           "      ends with the name of its error, as in (engine.not_found).\n"
           "\n"
           "  vss_cli control expect <id> --pattern=<digits> [--timeout=<ms>]\n"
           "           [--tx] [--send-to=<id> --send=<digits>]\n"
           "           [--instance=<name>] [--token-file=<file>]\n"
           "      Wait for bytes to go by on a port.  --pattern is 1 to 256\n"
           "      bytes as hex digits, as for send.  It waits --timeout ms (1\n"
           "      to 60000, 1000 by default) for what the port received, or\n"
           "      with --tx for what it sent.  With --send-to and --send the\n"
           "      gateway sends those bytes to that port once it is\n"
           "      listening, so a reply cannot be missed.  When the pattern\n"
           "      came it prints matched, the number of bytes that went by\n"
           "      before it, the time in ns on the steady clock of the\n"
           "      computer and the number of events lost, as in\n"
           "      matched 6 1234567890 0.  When it did not come the line\n"
           "      ends with (timeout) and the exit code is 1.  It needs the\n"
           "      scope monitor.data, and control.data as well to send; the\n"
           "      default role of this user (monitor) has neither.\n"
           "\n"
           "  vss_cli control expect ... --label=<text>\n"
           "  vss_cli control run begin <name> [--instance=<name>]\n"
           "           [--token-file=<file>]\n"
           "  vss_cli control run end [--instance=<name>] "
           "[--token-file=<file>]\n"
           "  vss_cli control run record <name>\n"
           "           --outcome=passed|failed|error|skipped\n"
           "           [--message=<text>] [--duration=<ms>]\n"
           "           [--instance=<name>] [--token-file=<file>]\n"
           "  vss_cli control run report [<run id>] [--out=<file>]\n"
           "           [--instance=<name>] [--token-file=<file>]\n"
           "      Keep a report of a test run.  run begin opens a run with a\n"
           "      name and prints its id (r_3).  One run is open for a user\n"
           "      or token at a time, and it stays open across commands.\n"
           "      While it is open, each expect of that user or token is\n"
           "      noted with how it ended, and --label is its name in the\n"
           "      report (expect and the port when there is none).  run\n"
           "      record adds a check of your own: a name, how it ended, a\n"
           "      message and how long it took in ms.  run end closes the run\n"
           "      and prints its id, tests, failures, errors, skipped and\n"
           "      duration_ms; the exit code is 0 whatever the counts.  run\n"
           "      report gives the run as JUnit XML, the latest one when no\n"
           "      id is given, on the screen or with --out in a file that is\n"
           "      made or emptied first.  Only the user or token that opened\n"
           "      a run can read it.  These need the scope monitor.\n"
           "\n"
           "  vss_cli control <command> ... --loopback-port=<port>\n"
           "           --token-file=<file>\n"
           "      status, watch, port, link, send, expect and run can connect\n"
           "      over TCP to 127.0.0.1 at the port (1 to 65535) that\n"
           "      `vss_gate --loopback-port=<port>` listens on, instead of "
           "the\n"
           "      pipe or socket.  Nobody is known by the system over TCP, so\n"
           "      --token-file is needed, and the token must have been made\n"
           "      with token create --transport=loopback: a token made for\n"
           "      one transport is refused on the other.  token, sessions,\n"
           "      kick and stop cannot be used over TCP.\n"
           "\n";
}

int command_control(const std::vector<std::string>& arguments)
{
    const control_arguments args = parse(arguments);
    if (!args.valid)
    {
        std::fputs(control_usage(), stderr);
        return exit_usage;
    }

    vss::control::client_options options;
    options.instance = args.instance;
    options.name = "vss_cli";
    // `status`, `watch`, `expect`, `run` and the four commands on ports are
    // for any caller; the rest are for this user on the administration
    // endpoint.
    options.admin = is_administration(args.action);
    options.loopback_port = args.loopback_port;
    if (args.token_file)
    {
        auto token = read_token_file(*args.token_file);
        if (!token)
        {
            std::fprintf(stderr, "control %s: cannot read a token from %s\n",
                         action_name(args.action), args.token_file->c_str());
            return exit_usage;
        }
        options.token = std::move(*token);
    }
    auto connected = client::connect(options);
    if (!connected)
    {
        if (connected.error() == vss::agent::protocol_errc::not_running)
        {
            std::puts("Remote control: off");
            return exit_not_running;
        }
        return report_refused(action_name(args.action), connected.error());
    }

    client& gateway = **connected;
    switch (args.action)
    {
    case action_kind::stop:
        return stop(gateway);
    case action_kind::token_create:
        return token_create(gateway, args);
    case action_kind::token_list:
        return token_list(gateway);
    case action_kind::token_revoke:
        return token_revoke(gateway, args.token_id);
    case action_kind::watch:
        return watch(gateway, args);
    case action_kind::sessions:
        return sessions(gateway);
    case action_kind::kick:
        return kick(gateway, args.session);
    case action_kind::port_create:
        return port_create(gateway, args);
    case action_kind::port_remove:
        return port_remove(gateway, args.port);
    case action_kind::link:
        return link_ports(gateway, args);
    case action_kind::send:
        return send_bytes(gateway, args);
    case action_kind::expect:
        return expect_bytes(gateway, args);
    case action_kind::run_begin:
        return run_begin(gateway, args);
    case action_kind::run_end:
        return run_end(gateway);
    case action_kind::run_record:
        return run_record(gateway, args);
    case action_kind::run_report:
        return run_report(gateway, args);
    case action_kind::status:
    case action_kind::none:
        break;
    }
    return status(gateway);
}

} // namespace vss_cli
