//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `vss_cli debug ...` asks a running Studio, over its debug pipe, what its
// engine, transports and Windows are doing.  It makes no studio of its own.
// The person at the computer approves it in Studio's window first.
//

#include "debug_command.hpp"

#include <vss/agent/proto/error.hpp>
#include <vss/core/error.hpp>
#include <vss/debug/client.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss_cli
{
namespace
{

namespace json = boost::json;
namespace dbg = vss::debug;

constexpr int exit_ok = 0;
constexpr int exit_refused = 1;
constexpr int exit_usage = 2;
constexpr int exit_not_running = 3;

constexpr std::chrono::milliseconds follow_poll{500};

struct options
{
    std::string verb;
    std::vector<std::string> rest;
    std::string instance;
    std::filesystem::path instance_directory;
    bool as_json = false;
    bool follow = false;
    bool failed_only = false;
    std::uint64_t endpoint = 0;
    std::size_t last = 0;
    std::chrono::milliseconds timeout{70000};
    std::filesystem::path out_file;
    std::string args_json;
    std::string window;
};

std::optional<std::uint64_t> whole(std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

void pretty(const json::value& value, int depth, std::string& out)
{
    const std::string pad(static_cast<std::size_t>(depth) * 2, ' ');
    const std::string inner(static_cast<std::size_t>(depth + 1) * 2, ' ');
    if (const auto* object = value.if_object())
    {
        if (object->empty())
        {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& member : *object)
        {
            if (!first)
                out += ",\n";
            first = false;
            out += inner + json::serialize(json::value(member.key())) + ": ";
            pretty(member.value(), depth + 1, out);
        }
        out += "\n" + pad + "}";
    }
    else if (const auto* list = value.if_array())
    {
        if (list->empty())
        {
            out += "[]";
            return;
        }
        out += "[\n";
        bool first = true;
        for (const auto& item : *list)
        {
            if (!first)
                out += ",\n";
            first = false;
            out += inner;
            pretty(item, depth + 1, out);
        }
        out += "\n" + pad + "]";
    }
    else
    {
        out += json::serialize(value);
    }
}

void print_json(const std::string& text, bool compact)
{
    if (compact)
    {
        std::printf("%s\n", text.c_str());
        return;
    }
    boost::system::error_code ec;
    const json::value value = json::parse(text, ec);
    if (ec)
    {
        std::printf("%s\n", text.c_str());
        return;
    }
    std::string out;
    pretty(value, 0, out);
    std::printf("%s\n", out.c_str());
}

std::string text_of(const json::object& object, std::string_view key)
{
    if (const auto* found = object.if_contains(key))
    {
        if (found->is_string())
            return std::string(found->as_string());
        return json::serialize(*found);
    }
    return {};
}

/// One recorder entry as a line a person reads.
std::string entry_line(const json::object& entry)
{
    std::string line = text_of(entry, "wall") + "  #" + text_of(entry, "seq");
    line += "  " + text_of(entry, "call");
    if (const auto args = text_of(entry, "args"); !args.empty())
        line += " (" + args + ")";
    if (const auto operation = text_of(entry, "operation"); !operation.empty())
        line += "  in " + operation;
    if (const auto endpoint = text_of(entry, "endpoint"); endpoint != "0")
        line += "  port " + endpoint;
    if (text_of(entry, "result") == "failed")
    {
        line += "  FAILED " + text_of(entry, "category") + ":" +
                text_of(entry, "code") + " " + text_of(entry, "message");
        if (const auto cause = text_of(entry, "cause"); !cause.empty())
            line += "  (from #" + cause + ")";
    }
    line += "  " + text_of(entry, "duration_us") + " us";
    return line;
}

void print_entries(const json::array& entries, bool as_json)
{
    for (const auto& item : entries)
    {
        if (!item.is_object())
            continue;
        if (as_json)
            std::printf("%s\n", json::serialize(item).c_str());
        else
            std::printf("%s\n", entry_line(item.as_object()).c_str());
    }
}

const json::object* result_object(const json::value& value)
{
    return value.if_object();
}

bool read_options(const std::vector<std::string>& arguments, options& out,
                  std::string& problem)
{
    // arguments[0] is "debug".
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string& word = arguments[index];
        const auto value_of =
            [&](std::string_view name) -> std::optional<std::string>
        {
            if (word == name)
            {
                if (index + 1 >= arguments.size())
                {
                    problem = std::string(name) + " needs a value";
                    return std::nullopt;
                }
                return arguments[++index];
            }
            const std::string prefix = std::string(name) + "=";
            if (word.starts_with(prefix))
                return word.substr(prefix.size());
            return std::nullopt;
        };
        if (!problem.empty())
            return false;
        if (word == "--json")
            out.as_json = true;
        else if (word == "--follow" || word == "-f")
            out.follow = true;
        else if (word == "--failed")
            out.failed_only = true;
        else if (const auto text = value_of("--pid"))
            out.instance = *text;
        else if (const auto text2 = value_of("--instance"))
            out.instance = *text2;
        else if (const auto text3 = value_of("--instance-dir"))
            out.instance_directory = *text3;
        else if (const auto text4 = value_of("--out"))
            out.out_file = *text4;
        else if (const auto text5 = value_of("--args"))
            out.args_json = *text5;
        else if (const auto text6 = value_of("--window"))
            out.window = *text6;
        else if (const auto text7 = value_of("--endpoint"))
        {
            const auto number = whole(*text7);
            if (!number)
            {
                problem = "--endpoint wants a whole number";
                return false;
            }
            out.endpoint = *number;
        }
        else if (const auto text8 = value_of("--last"))
        {
            const auto number = whole(*text8);
            if (!number)
            {
                problem = "--last wants a whole number";
                return false;
            }
            out.last = static_cast<std::size_t>(*number);
        }
        else if (const auto text9 = value_of("--timeout"))
        {
            const auto number = whole(*text9);
            if (!number)
            {
                problem = "--timeout wants seconds";
                return false;
            }
            out.timeout = std::chrono::seconds(*number);
        }
        else if (!problem.empty())
            return false;
        else if (word.starts_with("--"))
        {
            problem = "unknown option " + word;
            return false;
        }
        else if (out.verb.empty())
            out.verb = word;
        else
            out.rest.push_back(word);
    }
    return true;
}

dbg::scope_set scopes_for(const std::string& verb)
{
    dbg::scope_set wanted = dbg::bit(dbg::scope::read);
    if (verb == "run")
        wanted |= dbg::bit(dbg::scope::act);
    if (verb == "gui" || verb == "screenshot")
        wanted |= dbg::bit(dbg::scope::gui);
    if (verb == "traffic")
        wanted |= dbg::bit(dbg::scope::data);
    return wanted;
}

/// The operation a verb calls, or empty for one that is handled apart.
std::string operation_for(const std::string& verb)
{
    if (verb == "status")
        return "debug.status";
    if (verb == "snapshot")
        return "debug.snapshot";
    if (verb == "com")
        return "debug.com_numbers";
    if (verb == "driver")
        return "debug.driver";
    if (verb == "plugins")
        return "debug.plugins";
    if (verb == "traffic")
        return "debug.traffic";
    if (verb == "run")
        return "debug.run";
    if (verb == "gui")
        return "debug.gui.state";
    if (verb == "screenshot")
        return "debug.gui.screenshot";
    if (verb == "audit-verify")
        return "debug.audit.verify";
    return {};
}

int list_instances(const options& given)
{
    std::filesystem::path directory = given.instance_directory;
    if (directory.empty())
    {
        const auto found = dbg::default_instance_directory();
        if (!found)
        {
            std::fprintf(stderr, "vss_cli: %s\n",
                         found.error().message().c_str());
            return exit_refused;
        }
        directory = *found;
    }
    const auto instances = dbg::find_instances(directory);
    if (given.as_json)
    {
        json::array list;
        for (const auto& item : instances)
        {
            json::object row;
            row["instance"] = item.instance;
            row["process"] = item.process_id;
            row["address"] = item.address;
            list.push_back(std::move(row));
        }
        std::printf("%s\n", json::serialize(list).c_str());
        return instances.empty() ? exit_not_running : exit_ok;
    }
    if (instances.empty())
    {
        std::printf("No Studio has debug access on.\n");
        return exit_not_running;
    }
    for (const auto& item : instances)
        std::printf("%s  process %llu\n", item.instance.c_str(),
                    static_cast<unsigned long long>(item.process_id));
    return exit_ok;
}

/// The instance to use: the one named, or the only one there is.
std::optional<std::string> choose_instance(const options& given, int& code)
{
    if (!given.instance.empty())
        return given.instance;
    std::filesystem::path directory = given.instance_directory;
    if (directory.empty())
    {
        const auto found = dbg::default_instance_directory();
        if (found)
            directory = *found;
    }
    const auto instances = dbg::find_instances(directory);
    if (instances.empty())
    {
        std::fprintf(stderr,
                     "vss_cli: no Studio has debug access on.  Turn it on in "
                     "Studio: Settings, Performance, Debug access.\n");
        code = exit_not_running;
        return std::nullopt;
    }
    if (instances.size() > 1)
    {
        std::fprintf(stderr,
                     "vss_cli: more than one Studio has debug access on; "
                     "name one with --pid (vss_cli debug list shows them)\n");
        code = exit_usage;
        return std::nullopt;
    }
    return instances.front().instance;
}

std::string build_args(const options& given)
{
    if (!given.args_json.empty())
        return given.args_json;
    json::object args;
    if (given.endpoint != 0)
        args["endpoint"] = given.endpoint;
    if (given.failed_only)
        args["failures_only"] = true;
    if (given.last != 0)
        args["limit"] = given.last;
    if (given.verb == "run" && !given.rest.empty())
    {
        args["op"] = json::string(given.rest.front());
        // key=value words after the operation name.
        json::object settings;
        for (std::size_t index = 1; index < given.rest.size(); ++index)
        {
            const std::string& word = given.rest[index];
            const auto equals = word.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = word.substr(0, equals);
            const std::string value = word.substr(equals + 1);
            json::value stored = json::string(value);
            if (const auto number = whole(value))
                stored = *number;
            if (key == "baud_rate" || key == "data_bits" || key == "parity")
                settings[key] = std::move(stored);
            else
                args[key] = std::move(stored);
        }
        if (!settings.empty())
            args["settings"] = std::move(settings);
    }
    if (given.verb == "traffic" && given.endpoint != 0)
    {
        args["endpoints"] = json::array{given.endpoint};
        args.erase("endpoint");
    }
    if (given.verb == "screenshot" && !given.window.empty())
        args["window"] = json::string(given.window);
    return json::serialize(args);
}

int follow_trace(dbg::client& session, const options& given)
{
    json::object args;
    args["backlog"] = given.last != 0 ? given.last : 20;
    if (given.endpoint != 0)
        args["endpoint"] = given.endpoint;
    if (given.failed_only)
        args["failures_only"] = true;
    const auto started =
        session.call("debug.trace.subscribe", json::serialize(args));
    if (!started)
    {
        std::fprintf(stderr, "vss_cli: %s\n",
                     started.error().message().c_str());
        return exit_refused;
    }
    if (!started->ok)
    {
        std::fprintf(stderr, "vss_cli: %s\n", started->message.c_str());
        return exit_refused;
    }
    for (;;)
    {
        const auto event = session.next_event(follow_poll);
        if (!event)
        {
            if (event.error() == vss::make_error_code(vss::errc::timeout))
                continue;
            // The studio went away or turned debug access off.
            return exit_ok;
        }
        boost::system::error_code ec;
        const json::value body = json::parse(*event, ec);
        if (ec || !body.is_object())
            continue;
        if (const auto* entries = body.as_object().if_contains("entries");
            entries != nullptr && entries->is_array())
        {
            print_entries(entries->as_array(), given.as_json);
            std::fflush(stdout);
        }
    }
}

} // namespace

const char* debug_usage() noexcept
{
    return "  vss_cli debug list\n"
           "  vss_cli debug status|snapshot|com|driver|plugins [--pid=N] "
           "[--json]\n"
           "  vss_cli debug trace [--follow] [--failed] [--endpoint=N] "
           "[--last=N] [--json]\n"
           "  vss_cli debug run <operation> [port=NAME] [key=value ...]\n"
           "  vss_cli debug traffic [--endpoint=N]   (needs the traffic "
           "switch)\n"
           "  vss_cli debug gui | screenshot [--out=file.png]\n"
           "  vss_cli debug audit-verify\n"
           "      Ask a running Studio what its engine, ports and Windows are\n"
           "      doing.  Studio must have debug access on (Settings, "
           "Performance)\n"
           "      and the person at the computer approves each program.  This\n"
           "      computer only; nothing is sent anywhere.\n"
           "\n";
}

int command_debug(const std::vector<std::string>& arguments)
{
    // Read the command line. A bad option is a usage error.
    options given;
    std::string problem;
    if (!read_options(arguments, given, problem))
    {
        std::fprintf(stderr, "vss_cli: debug: %s\n", problem.c_str());
        return exit_usage;
    }
    if (given.verb.empty())
    {
        std::fprintf(stderr, "vss_cli: debug wants a command; see --help\n");
        return exit_usage;
    }
    // Listing the running Studios needs no connection, so it is answered
    // before one is made.
    if (given.verb == "list")
        return list_instances(given);

    // Refuse an unknown command before a person is asked to approve access.
    const bool known =
        given.verb == "trace" || !operation_for(given.verb).empty();
    if (!known)
    {
        std::fprintf(stderr, "vss_cli: debug: unknown command %s\n",
                     given.verb.c_str());
        return exit_usage;
    }
    if (given.verb == "run" && given.rest.empty() && given.args_json.empty())
    {
        std::fprintf(stderr, "vss_cli: debug run wants an operation name\n");
        return exit_usage;
    }

    // Pick which running Studio to talk to. `code` carries the exit status
    // when none can be chosen.
    int code = exit_ok;
    const auto instance = choose_instance(given, code);
    if (!instance)
        return code;

    // Connect with only the scopes this command needs.
    dbg::client_options connect;
    connect.instance = *instance;
    connect.scopes = scopes_for(given.verb);
    connect.timeout = given.timeout;
    auto session = dbg::client::connect(connect);
    if (!session)
    {
        // Give the two common failures a plain explanation: the person
        // said no, or Studio is not listening.
        const auto& error = session.error();
        if (error == vss::make_error_code(vss::errc::permission_denied))
            std::fprintf(stderr,
                         "vss_cli: the person at this computer did not allow "
                         "this program in Studio's debug access.\n");
        else if (error == vss::agent::make_error_code(
                              vss::agent::protocol_errc::not_running))
            std::fprintf(stderr,
                         "vss_cli: that Studio is not listening; debug access "
                         "may have been turned off.\n");
        else
            std::fprintf(stderr, "vss_cli: %s\n", error.message().c_str());
        return exit_not_running;
    }

    // The trace command has its own path: it can follow the trace live, or
    // fetch one batch and print the entries as text.
    if (given.verb == "trace")
    {
        if (given.follow)
            return follow_trace(**session, given);
        const auto done = (*session)->call("debug.trace", build_args(given));
        if (!done)
        {
            std::fprintf(stderr, "vss_cli: %s\n",
                         done.error().message().c_str());
            return exit_refused;
        }
        if (!done->ok)
        {
            std::fprintf(stderr, "vss_cli: %s\n", done->message.c_str());
            return exit_refused;
        }
        // With --json the reply is printed as it came; otherwise the entries
        // are printed one per line. An unparsable reply prints nothing.
        boost::system::error_code ec;
        const json::value body = json::parse(done->json, ec);
        if (given.as_json)
        {
            print_json(done->json, true);
            return exit_ok;
        }
        if (const auto* object = ec ? nullptr : result_object(body))
        {
            if (const auto* entries = object->if_contains("entries");
                entries != nullptr && entries->is_array())
                print_entries(entries->as_array(), false);
        }
        return exit_ok;
    }

    // Every other command is one call to the operation named for the verb.
    const auto done =
        (*session)->call(operation_for(given.verb), build_args(given));
    if (!done)
    {
        std::fprintf(stderr, "vss_cli: %s\n", done.error().message().c_str());
        return exit_refused;
    }
    if (!done->ok)
    {
        std::fprintf(stderr, "vss_cli: %s (%s)\n", done->message.c_str(),
                     done->code.c_str());
        return exit_refused;
    }
    // A binary reply (a picture) cannot go to the terminal, so it must be
    // written to the file given with --out.
    if (!done->blob.empty())
    {
        if (given.out_file.empty())
        {
            std::fprintf(stderr,
                         "vss_cli: the reply is a picture; say where to save "
                         "it with --out=file.png\n");
            return exit_usage;
        }
        std::ofstream file(given.out_file, std::ios::binary);
        file.write(reinterpret_cast<const char*>(done->blob.data()),
                   static_cast<std::streamsize>(done->blob.size()));
        if (!file)
        {
            std::fprintf(stderr, "vss_cli: cannot write %s\n",
                         given.out_file.string().c_str());
            return exit_refused;
        }
        std::printf("%zu bytes written to %s\n", done->blob.size(),
                    given.out_file.string().c_str());
        return exit_ok;
    }
    print_json(done->json, given.as_json);
    return exit_ok;
}

} // namespace vss_cli
