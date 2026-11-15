//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vss_cli permissions - what the operating system may stop Studio doing.
//
//   check               read-only: says what is blocked or missing
//   firewall-allow ...  the elevated helper step the GUI starts through the
//   firewall-remove ... operating system's own elevation prompt
//
// `check` changes nothing. The two firewall steps change one Windows Firewall
// rule, need administrator rights, and refuse anything that is not a program of
// ours next to this one. They are what the GUI runs after the user has read the
// rule and agreed; run by hand they do the same and print the same.
//

#include "permissions_command.hpp"

#include <vss/ports/permissions.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vss_cli
{
namespace
{

namespace perm = vss::permissions;

std::string_view topic_name(perm::topic what) noexcept
{
    switch (what)
    {
    case perm::topic::windows_elevation:
        return "windows_elevation";
    case perm::topic::windows_firewall:
        return "windows_firewall";
    case perm::topic::serial_access:
        return "serial_access";
    case perm::topic::confinement:
        return "confinement";
    case perm::topic::linux_firewall:
        return "linux_firewall";
    case perm::topic::macos_firewall:
        return "macos_firewall";
    case perm::topic::macos_local_network:
        return "macos_local_network";
    }
    return "unknown";
}

std::string_view state_name(perm::state status) noexcept
{
    switch (status)
    {
    case perm::state::ok:
        return "ok";
    case perm::state::info:
        return "info";
    case perm::state::attention:
        return "attention";
    case perm::state::unknown:
        return "unknown";
    case perm::state::not_applicable:
        return "not_applicable";
    }
    return "unknown";
}

std::string json_text(std::string_view text)
{
    std::string out = "\"";
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += ' ';
            else
                out += c;
        }
    }
    out += '"';
    return out;
}

int command_check(const std::vector<std::string>& arguments)
{
    bool json = false;
    perm::context ask;
    for (std::size_t index = 2; index < arguments.size(); ++index)
    {
        const std::string& argument = arguments[index];
        if (argument == "--json")
        {
            json = true;
        }
        else if (argument.rfind("--device=", 0) == 0)
        {
            ask.device_paths.push_back(argument.substr(9));
        }
        else if (argument.rfind("--listen=", 0) == 0)
        {
            perm::listener each;
            if (!perm::parse_listen_uri(argument.substr(9), each))
            {
                std::fprintf(stderr, "permissions: cannot read %s\n",
                             argument.c_str());
                return 2;
            }
            ask.listeners.push_back(each);
        }
        else
        {
            std::fprintf(stderr, "permissions check: unknown option %s\n",
                         argument.c_str());
            return 2;
        }
    }

    const std::vector<perm::fact> facts = perm::probe(ask);
    if (json)
    {
        std::printf("{\"host\":%s,\"facts\":[",
                    json_text(perm::host_name()).c_str());
        bool first = true;
        for (const perm::fact& each : facts)
        {
            std::printf(
                "%s{\"topic\":%s,\"state\":%s,\"reason\":%s,\"subject\":%s,"
                "\"remedy\":%s,\"can_act\":%s}",
                first ? "" : ",", json_text(topic_name(each.what)).c_str(),
                json_text(state_name(each.status)).c_str(),
                json_text(each.why).c_str(), json_text(each.subject).c_str(),
                json_text(each.remedy).c_str(),
                each.can_act ? "true" : "false");
            first = false;
        }
        std::printf("]}\n");
        return 0;
    }

    std::printf("Host: %s\n", std::string(perm::host_name()).c_str());
    for (const perm::fact& each : facts)
    {
        std::printf(
            "%-18s %-10s %s", std::string(topic_name(each.what)).c_str(),
            std::string(state_name(each.status)).c_str(), each.why.c_str());
        if (!each.subject.empty())
            std::printf("  (%s)", each.subject.c_str());
        std::printf("\n");
        if (!each.remedy.empty())
            std::printf("    %s\n", each.remedy.c_str());
    }
    if (facts.empty())
        std::printf("Nothing to report on this system yet.\n");
    return 0;
}

int command_firewall_step(const std::vector<std::string>& arguments)
{
    const auto step = perm::parse_elevated_arguments(arguments);
    if (!step)
    {
        std::fprintf(stderr,
                     "permissions: the arguments are not a firewall step\n");
        return perm::exit_invalid;
    }
    if (const std::string why = perm::validate(step->spec); !why.empty())
    {
        std::fprintf(stderr, "permissions: %s\n", why.c_str());
        return perm::exit_invalid;
    }
    if (const std::string why =
            perm::program_is_ours(step->spec, perm::this_program());
        !why.empty())
    {
        std::fprintf(stderr, "permissions: %s\n", why.c_str());
        return perm::exit_invalid;
    }
    std::error_code ignored;
    if (!std::filesystem::exists(std::filesystem::path(step->spec.program),
                                 ignored))
    {
        std::fprintf(stderr, "permissions: the program does not exist\n");
        return perm::exit_invalid;
    }

    const perm::step_result result =
        perm::apply_firewall_rule(step->spec, step->remove);
    switch (result.status)
    {
    case perm::step_status::done:
        std::printf("%s: %s\n", perm::firewall_rule_name(step->spec).c_str(),
                    result.detail.c_str());
        return perm::exit_done;
    case perm::step_status::not_elevated:
        std::fprintf(stderr,
                     "permissions: this step needs administrator rights. The "
                     "graphical interface starts it for you; to run it by "
                     "hand, open an elevated terminal.\n");
        return perm::exit_not_elevated;
    case perm::step_status::invalid:
        std::fprintf(stderr, "permissions: %s\n", result.detail.c_str());
        return perm::exit_invalid;
    case perm::step_status::unsupported:
    case perm::step_status::declined:
    case perm::step_status::failed:
        break;
    }
    std::fprintf(stderr, "permissions: %s\n", result.detail.c_str());
    return perm::exit_failed;
}

} // namespace

const char* permissions_usage() noexcept
{
    return "  vss_cli permissions check [--json] "
           "[--listen=<tcp|udp|rfc2217>-listen://host:port]\n"
           "      Say what the operating system may stop Studio doing and the\n"
           "      smallest step to fix it. Reads only; changes nothing.\n"
           "\n"
           "  vss_cli permissions firewall-allow|firewall-remove "
           "--program=<path>\n"
           "      --protocol=tcp|udp --port=N [--scope=local-subnet|any]\n"
           "      --profiles=domain,private,public (one or two of them; never\n"
           "      all three)\n"
           "      Windows only, needs administrator rights. The graphical\n"
           "      interface runs this after you agree; it adds or removes one\n"
           "      Windows Firewall rule for one of our programs.\n"
           "\n";
}

int command_permissions(const std::vector<std::string>& arguments)
{
    if (arguments.size() < 2)
    {
        std::fputs(permissions_usage(), stderr);
        return 2;
    }
    const std::string& verb = arguments[1];
    if (verb == "check")
        return command_check(arguments);
    if (verb == "firewall-allow" || verb == "firewall-remove")
        return command_firewall_step(arguments);
    std::fprintf(stderr, "permissions: unknown step %s\n", verb.c_str());
    std::fputs(permissions_usage(), stderr);
    return 2;
}

} // namespace vss_cli
