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

enum class action_kind
{
    none,
    status,
    stop,
    token_create,
    token_list,
    token_revoke
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
            take_option(word, "--token-file=", raw.token_file, raw.valid))
            continue;
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
        // `token create`.
        if (!no_create_options(raw) || (!is_status && raw.token_file))
            return parsed;
        parsed.action = is_status ? action_kind::status : action_kind::stop;
        parsed.token_file = raw.token_file;
        parsed.valid = true;
        return parsed;
    }
    if (words.size() < 2 || words[0] != "token" || raw.token_file)
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
    options.admin = args.action != action_kind::status;
    if (args.token_file)
    {
        auto token = read_token_file(*args.token_file);
        if (!token)
        {
            std::fprintf(stderr,
                         "control status: cannot read a token from %s\n",
                         args.token_file->c_str());
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
    case action_kind::status:
    case action_kind::none:
        break;
    }
    return status(gateway);
}

} // namespace vss_cli
