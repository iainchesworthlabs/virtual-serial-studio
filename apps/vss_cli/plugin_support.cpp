//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "plugin_support.hpp"

#include <vss/plugin/error.hpp>
#include <vss/plugin/licence.hpp>
#include <vss/plugin/locations.hpp>
#include <vss/ports/permissions.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vss_cli::plugin_support
{
namespace
{

namespace fs = std::filesystem;
namespace json = boost::json;
namespace plug = vss::plugin;

/// How many bytes the UTF-8 character at the start of `text` has, or 0 when
/// the bytes there are not a character: a stray or missing continuation, a
/// longer form than needed, a surrogate or a value above U+10FFFF.
std::size_t utf8_length(std::string_view text)
{
    constexpr std::size_t not_a_character = 0;
    const auto at = [&text](std::size_t index)
    { return static_cast<unsigned>(static_cast<unsigned char>(text[index])); };
    const auto continuation = [&](std::size_t index, unsigned low,
                                  unsigned high)
    {
        return index < text.size() && at(index) >= low && at(index) <= high;
    };

    const unsigned lead = at(0);
    if (lead < 0x80)
        return 1;
    if (lead >= 0xc2 && lead <= 0xdf)
        return continuation(1, 0x80, 0xbf) ? 2 : not_a_character;
    if (lead >= 0xe0 && lead <= 0xef)
    {
        const unsigned low = lead == 0xe0 ? 0xa0 : 0x80;
        const unsigned high = lead == 0xed ? 0x9f : 0xbf;
        if (continuation(1, low, high) && continuation(2, 0x80, 0xbf))
            return 3;
        return not_a_character;
    }
    if (lead >= 0xf0 && lead <= 0xf4)
    {
        const unsigned low = lead == 0xf0 ? 0x90 : 0x80;
        const unsigned high = lead == 0xf4 ? 0x8f : 0xbf;
        if (continuation(1, low, high) && continuation(2, 0x80, 0xbf) &&
            continuation(3, 0x80, 0xbf))
            return 4;
        return not_a_character;
    }
    return not_a_character;
}

std::string_view licence_reason(vss::license::reason why) noexcept
{
    using vss::license::reason;
    switch (why)
    {
    case reason::ok:
        return "ok";
    case reason::no_licence:
        return "there is no licence on this computer";
    case reason::expired:
        return "the licence has expired";
    case reason::not_yet_valid:
        return "the licence does not start yet";
    case reason::clock_suspect:
        return "the clock of this computer looks wrong";
    case reason::malformed:
        return "the licence file cannot be read";
    case reason::newer_version:
        return "the licence is from a newer version of Studio";
    case reason::bad_signature:
        return "the licence is not signed correctly";
    case reason::revoked:
        return "the licence was withdrawn";
    case reason::wrong_computer:
        return "the licence is for another computer";
    case reason::not_in_licence:
        return "the licence does not name this plugin";
    }
    return "unknown";
}

std::string_view level_word(plug::trust_level level) noexcept
{
    switch (level)
    {
    case plug::trust_level::official:
        return "official";
    case plug::trust_level::third_party_signed:
        return "third_party_signed";
    case plug::trust_level::signed_unknown_key:
        return "signed_unknown_key";
    case plug::trust_level::unsigned_plugin:
        return "unsigned";
    }
    return "unknown";
}

std::string_view state_word(plug::signature_state state) noexcept
{
    switch (state)
    {
    case plug::signature_state::absent:
        return "absent";
    case plug::signature_state::invalid:
        return "invalid";
    case plug::signature_state::valid:
        return "valid";
    case plug::signature_state::unknown_key:
        return "unknown_key";
    }
    return "unknown";
}

/// The name of the signer of a third-party plugin: the name the user saw when
/// they approved the key, and otherwise what the manifest says.
std::string author_of(const plug::inspection& found,
                      const plug::inspect_context& context)
{
    if (context.trust.has_value())
    {
        if (const plug::approved_key* key =
                context.trust->find_key(found.signature.key_id))
            return plain(key->name);
    }
    if (found.declared.has_value() && !found.declared->author.name.empty())
        return plain(found.declared->author.name);
    return "an unnamed author";
}

void add_line(std::string& out, std::string_view label, std::string_view value)
{
    out += "  ";
    out += label;
    out += value;
    out += '\n';
}

} // namespace

std::string plain(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        out += byte >= 0x20 && byte <= 0x7e ? c : '?';
    }
    return out;
}

std::string valid_utf8(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size())
    {
        const std::size_t length = utf8_length(text.substr(index));
        if (length == 0)
        {
            out += '?';
            ++index;
        }
        else
        {
            out.append(text.substr(index, length));
            index += length;
        }
    }
    return out;
}

std::string path_text(const fs::path& path)
{
    const std::u8string text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

fs::path program_directory()
{
    const std::string program = vss::permissions::this_program();
    if (program.empty())
        return {};
    // The text is UTF-8 on every system.
    const std::u8string text(program.begin(), program.end());
    return fs::path(text).parent_path();
}

json::value text_value(std::string_view text)
{
    const std::string cleaned = valid_utf8(text);
    return json::value(json::string(cleaned.data(), cleaned.size()));
}

std::string code_text(const vss::error_code& problem)
{
    if (problem.category() == plug::plugin_category())
    {
        return std::string(
            plug::to_string(static_cast<plug::plugin_errc>(problem.value())));
    }
    return problem.message();
}

computer this_computer(bool read_policy)
{
    computer out;
    const fs::path programs = program_directory();
    plug::computer_options options;
    options.program_directory = programs;
    if (read_policy)
    {
        out.policy_file = plug::plugin_locations_for(programs).policy_file;
        options.policy_file = out.policy_file;
    }
    out.context = plug::context_for_this_computer(options, &out.notes);
    return out;
}

std::string trust_line(const plug::inspection& found,
                       const plug::inspect_context& context)
{
    if (!found.level.has_value())
        return {};
    switch (*found.level)
    {
    case plug::trust_level::official:
        return "Official";
    case plug::trust_level::third_party_signed:
        return "Signed by " + author_of(found, context);
    case plug::trust_level::signed_unknown_key:
        return "Signed by an author you have not approved";
    case plug::trust_level::unsigned_plugin:
        return "Unsigned";
    }
    return {};
}

std::string signature_line(const plug::inspection& found,
                           const plug::inspect_context& context)
{
    switch (found.signature.state)
    {
    case plug::signature_state::absent:
        return "Not signed. Studio cannot tell who made it or whether it was "
               "changed.";
    case plug::signature_state::invalid:
        return "The signature does not hold: " +
               plain(found.signature_detail);
    case plug::signature_state::unknown_key:
        return "Signed with a key that Studio does not have (" +
               plain(found.signature.key_id) +
               "), so the signature could not be checked.";
    case plug::signature_state::valid:
        break;
    }
    if (found.signature.origin == plug::key_origin::official)
        return "Signed by Virtual Serial Studio.";
    return "Signed by " + author_of(found, context) +
           ". Not verified by Virtual Serial Studio.";
}

std::string run_line(const plug::inspection& found)
{
    if (!found.level.has_value())
        return "no, the plugin cannot be used";
    if (!found.run.failed())
        return "yes";
    std::string out = "no, " + code_text(found.run);
    if (!found.run_detail.empty())
    {
        out += ": ";
        out += plain(found.run_detail);
    }
    return out;
}

std::string licence_line(const plug::inspection& found,
                         const plug::inspect_context& context)
{
    if (!found.declared.has_value() || !found.level.has_value())
        return "not checked";
    if (!plug::needs_licence(*found.level, *found.declared))
        return "none needed";
    if (!context.licence_known)
        return "not checked: this command does not read licence files";
    if (!found.licence_problem.failed())
        return "covered";
    return std::string("not covered, ") +
           std::string(licence_reason(found.licence.reason));
}

void add_verdict_lines(std::string& out, const plug::inspection& found,
                       const plug::inspect_context& context)
{
    add_line(out, "Signature:    ", signature_line(found, context));
    const std::string trust = trust_line(found, context);
    add_line(out, "Trust:        ", trust.empty() ? "not worked out" : trust);
    add_line(out, "Key:          ",
             found.signature.key_id.empty() ? "none"
                                            : plain(found.signature.key_id));
    add_line(out, "May run:      ", run_line(found));
    add_line(out, "Licence:      ", licence_line(found, context));
}

void add_verdict_json(json::object& item, const plug::inspection& found,
                      const plug::inspect_context& context)
{
    item["signature_state"] = std::string(state_word(found.signature.state));
    if (found.level.has_value())
    {
        item["trust"] = std::string(level_word(*found.level));
        item["trust_line"] = text_value(trust_line(found, context));
    }
    else
    {
        item["trust"] = nullptr;
        item["trust_line"] = nullptr;
    }
    if (found.signature.key_id.empty())
        item["key_id"] = nullptr;
    else
        item["key_id"] = text_value(found.signature.key_id);

    if (!found.level.has_value())
    {
        item["may_run"] = nullptr;
        item["run_reason"] = nullptr;
        item["run_detail"] = nullptr;
    }
    else if (found.run.failed())
    {
        item["may_run"] = false;
        item["run_reason"] = text_value(code_text(found.run));
        item["run_detail"] = text_value(found.run_detail);
    }
    else
    {
        item["may_run"] = true;
        item["run_reason"] = nullptr;
        item["run_detail"] = nullptr;
    }

    json::object licence;
    licence["checked"] = found.licence_checked;
    if (found.licence_checked)
    {
        licence["covered"] = !found.licence_problem.failed();
        licence["detail"] =
            found.licence_problem.failed()
                ? text_value(licence_reason(found.licence.reason))
                : json::value(nullptr);
    }
    else
    {
        licence["covered"] = nullptr;
        licence["detail"] = text_value(licence_line(found, context));
    }
    item["licence"] = std::move(licence);
}

} // namespace vss_cli::plugin_support
