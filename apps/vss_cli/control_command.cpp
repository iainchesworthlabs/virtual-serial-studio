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
    kick
};

struct control_arguments
{
    action_kind action = action_kind::none;
    std::string instance;
    std::string label;
    std::vector<std::string> scopes;
    std::uint64_t life_s = 0;
    std::optional<std::string> out_file;
    std::optional<std::string> token_file;
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
    std::optional<std::string> ports;
    std::optional<std::string> interval;
    std::optional<std::string> seconds;
    std::optional<std::string> count;
    bool data = false;
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
            take_option(word, "--ports=", raw.ports, raw.valid) ||
            take_option(word, "--interval=", raw.interval, raw.valid) ||
            take_option(word, "--seconds=", raw.seconds, raw.valid) ||
            take_option(word, "--count=", raw.count, raw.valid))
            continue;
        if (word == "--data")
        {
            if (raw.data)
                raw.valid = false;
            raw.data = true;
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
    parsed.out_file = raw.out_file;
    return true;
}

/// True when none of the options that only `token create` takes is there.
bool no_create_options(const raw_arguments& raw)
{
    return !raw.label && !raw.role && !raw.scope && !raw.days && !raw.out_file;
}

/// True when none of the options that only `watch` takes is there.
bool no_watch_options(const raw_arguments& raw)
{
    return !raw.data && !raw.ports && !raw.interval && !raw.seconds &&
           !raw.count;
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

control_arguments parse(const std::vector<std::string>& arguments)
{
    control_arguments parsed;
    const raw_arguments raw = read_words(arguments);
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
        if (!no_create_options(raw) || !no_watch_options(raw) ||
            (!is_status && raw.token_file))
            return parsed;
        parsed.action = is_status ? action_kind::status : action_kind::stop;
        parsed.token_file = raw.token_file;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 1 && words[0] == "watch")
    {
        if (!no_create_options(raw))
            return parsed;
        parsed.action = action_kind::watch;
        parsed.token_file = raw.token_file;
        parsed.valid = read_watch_options(raw, parsed);
        return parsed;
    }
    if (words.size() == 1 && words[0] == "sessions")
    {
        if (!no_create_options(raw) || !no_watch_options(raw) || raw.token_file)
            return parsed;
        parsed.action = action_kind::sessions;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() == 2 && words[0] == "kick")
    {
        const auto number = whole_number<std::uint64_t>(words[1]);
        if (!number || !no_create_options(raw) || !no_watch_options(raw) ||
            raw.token_file)
            return parsed;
        parsed.action = action_kind::kick;
        parsed.session = *number;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() < 2 || words[0] != "token" || raw.token_file ||
        !no_watch_options(raw))
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

/// As `report`, and for a refusal of the gateway its code name too, as in
/// `not allowed (forbidden)`: the wording alone does not say which it was.
int report_with_code(const char* what, const vss::error_code& error,
                     const client& gateway)
{
    std::string message = !gateway.last_error_message().empty()
                              ? gateway.last_error_message()
                              : error.message();
    if (error.category() == vss::control::api_category())
    {
        message += " (";
        message += vss::control::code_name(
            static_cast<vss::control::api_errc>(error.value()));
        message += ")";
    }
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
    const auto made =
        gateway.create_token(args.label, args.scopes, args.life_s);
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
           "  vss_cli control token list [--instance=<name>]\n"
           "  vss_cli control token revoke <id> [--instance=<name>]\n"
           "      Make, list and revoke the tokens that let a program connect\n"
           "      as something other than this user.  create needs a label\n"
           "      and either a role or a list of scope names.  The token\n"
           "      lives --days days (1 to 365, 30 by default).  It is shown\n"
           "      once, on the screen or, with --out, in a file; keep it like\n"
           "      a password.  list shows the records, never the tokens.\n"
           "      revoke ends a token and its sessions.\n"
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
    // `status` and `watch` are for any caller; the rest are for this user on
    // the administration endpoint.
    options.admin =
        args.action != action_kind::status && args.action != action_kind::watch;
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
        return report(action_name(args.action), connected.error(), nullptr);
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
    case action_kind::status:
    case action_kind::none:
        break;
    }
    return status(gateway);
}

} // namespace vss_cli
