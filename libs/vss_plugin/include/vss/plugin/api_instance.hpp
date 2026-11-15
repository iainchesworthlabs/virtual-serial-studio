//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages about an instance of an extension point and its parameters:
// making one, stopping it, and telling it that a parameter changed.
// docs/plugins.md, sections 7.3 and 7.8.  The message that makes an instance
// of one kind (`generator.start`, `fixture.create`, `modifier.create`,
// `decoder.create`) is in the header of that kind.
//
// The design has no message that saves or restores the state of an instance.
// The run state is the plugin's own files in its data directory, and the
// binding is in the setup (section 7.8), so nothing travels.
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include <cstdint>
#include <string>

namespace vss::plugin
{

/// The longest id of an extension point or of a parameter, in bytes.
inline constexpr std::size_t max_api_id = 64;

/// Makes an instance of an extension point the manifest declares.  Section
/// 7.3 names it; the sections of the kinds each have a message of their own
/// for the same thing, and this one is the generic form.
struct instance_create_message
{
    static constexpr std::string_view op() noexcept
    {
        return "instance.create";
    }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    /// The number the host gives the instance.  The plugin uses it in every
    /// message about the instance.
    std::uint64_t instance = 0;
    extension_kind kind = extension_kind::generator;
    /// The "id" of the extension point in the manifest.
    std::string type;
    /// The parameters as one JSON object, already checked against the
    /// manifest.  "{}" when there are none.
    std::string params = "{}";

    friend bool operator==(const instance_create_message&,
                           const instance_create_message&) = default;
};

/// Stops an instance.  The plugin answers "ok" when it has let go of
/// everything the instance had.
struct instance_stop_message
{
    static constexpr std::string_view op() noexcept { return "instance.stop"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;

    friend bool operator==(const instance_stop_message&,
                           const instance_stop_message&) = default;
};

/// A parameter of a running instance has a new value.  The plugin answers
/// "ok", or an "error" whose message says why it will not take the value,
/// which Studio shows beside the parameter.
struct params_changed_message
{
    static constexpr std::string_view op() noexcept { return "params.changed"; }
    static constexpr api_side from() noexcept { return api_side::host; }
    static constexpr api_role role() noexcept { return api_role::request; }

    std::uint64_t instance = 0;
    /// The parameters that changed, as one JSON object, already checked.
    std::string params = "{}";

    friend bool operator==(const params_changed_message&,
                           const params_changed_message&) = default;
};

template <>
result<instance_create_message>
decode_as<instance_create_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<instance_create_message>(const instance_create_message&,
                                   std::uint64_t);
template <>
result<instance_stop_message>
decode_as<instance_stop_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<instance_stop_message>(const instance_stop_message&, std::uint64_t);
template <>
result<params_changed_message>
decode_as<params_changed_message>(const api_message&, std::string*);
template <>
result<api_message>
encode_as<params_changed_message>(const params_changed_message&, std::uint64_t);

} // namespace vss::plugin
