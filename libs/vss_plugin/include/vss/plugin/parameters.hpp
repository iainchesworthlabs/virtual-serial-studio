//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The values a user gives to an instance of a plugin, checked against what the
// plugin's manifest declares (docs/plugins.md, sections 7.8 and 13), and the
// text they are kept in.  The same text is the `params` of a plugin binding in
// a setup file (`vss::config::plugin_binding::params`, which holds one JSON
// object as text) and the `params` member of the plugin API messages
// `instance.create`, `params.changed`, `generator.start`, `fixture.create`,
// `modifier.create` and `decoder.create`.  Nothing here touches the disk, the
// network or a plugin.
//
// Reading is done in two steps, because JSON does not say what a value is for.
// `from_json` gives raw values: a boolean, a whole number, a number or text.
// `validate` reads each one as the type its parameter declares.
//

#pragma once

#include <vss/plugin/manifest.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::plugin
{

/// The most bytes the text of a set of values may take.  It is the limit
/// `vss::config` puts on the `params` of a binding.
inline constexpr std::size_t max_parameters_json_size = std::size_t{64} * 1024;
/// How deep that text may nest.  A set is an object of plain values, so a
/// deeper one is refused whatever it holds.
inline constexpr std::size_t max_parameters_json_depth = 4;
/// The most bytes of text one value may take in the JSON.  The longest hex
/// text of `bytes` is below this.
inline constexpr std::size_t max_parameter_text_size = 16 * 1024;
/// The most bytes a `path` value may have.
inline constexpr std::size_t max_path_parameter_length = 1024;
/// The longest a `duration` value may be: one day, in nanoseconds.  It is the
/// limit of the manifest reader.
inline constexpr std::int64_t max_parameter_duration_ns = 86'400'000'000'000;

// -- raw values ---------------------------------------------------------------

/// A value as the JSON carries it.  A `duration` is text such as `3ms` (units
/// `ns`, `us`, `ms` and `s`), `bytes` are hexadecimal text such as `02 31 03`
/// or `023103`, an `enum` is the option's value, a `port` or a `link` is the
/// name it has in the setup, and a `path` is text.
using raw_value = std::variant<bool, std::int64_t, double, std::string>;

struct raw_parameter
{
    std::string id;
    raw_value value;

    friend bool operator==(const raw_parameter&,
                           const raw_parameter&) = default;
};

/// Raw values in the order given.  `from_json` makes them in the order of the
/// text.
using raw_parameters = std::vector<raw_parameter>;

// -- checked values -----------------------------------------------------------

/// Where a `path` value lets a plugin in, for the sandbox to grant.  The path
/// is absolute and normal, is not a root or a drive, has no `..` and no
/// trailing separator.  For `kind == path_kind::directory` it is the folder
/// to grant; for a file it is the one file.
struct path_scope
{
    std::string path;
    path_kind kind = path_kind::file;
    path_access access = path_access::read;
    /// What the manifest says.  Whether the path exists is for the host to
    /// check, as this library does not look at the disk.
    bool must_exist = false;

    friend bool operator==(const path_scope&, const path_scope&) = default;
};

/// One value that passed `validate`.  `value` holds, by `type`: `boolean` a
/// `bool`; `integer` an `int64_t`; `number` a `double`; `duration`
/// nanoseconds as an `int64_t`; `bytes` the bytes themselves; and a
/// `choice`, `string`, `secret`, `port`, `link` or `path` its text (for a
/// path, the normal form).  `value` is never empty (`std::monostate`).
struct checked_parameter
{
    std::string id;
    parameter_type type = parameter_type::boolean;
    parameter_value value;
    /// Set for a `path` only.
    std::optional<path_scope> scope;
    /// True when the user gave nothing and the manifest's default was used.
    bool defaulted = false;

    friend bool operator==(const checked_parameter&,
                           const checked_parameter&) = default;
};

// -- refusals -----------------------------------------------------------------

/// Why one value was refused.
enum class parameter_errc
{
    /// The parameter has no default and was not given.
    missing_required,
    /// The name is not a parameter of this extension point.
    unknown_name,
    /// The name was given more than once.
    repeated_name,
    /// The value is not of the type the parameter declares.
    wrong_type,
    /// Below `min`.
    below_min,
    /// Above `max`.
    above_max,
    /// Longer than `max_length` (or the limit of a name or a path).
    too_long,
    /// Right type, wrong shape: text that is not plain, hex that is not hex,
    /// a duration without a unit, a number that is not finite.
    bad_format,
    /// Not one of the options of an `enum`.
    not_an_option,
    /// A path that is not absolute, is a root or a drive, has `..`, names a
    /// folder where a file is wanted, or is a network or device path.
    bad_path,
    /// A secret was found in a setup, which never keeps one.
    stored_secret
};

/// The code as it is written in logs: the enumerator's name.
std::string_view to_string(parameter_errc value) noexcept;

struct parameter_error
{
    /// The parameter's id, or the name that was given when there is no such
    /// parameter.
    std::string id;
    parameter_errc code = parameter_errc::bad_format;
    /// One line for a person, such as `"address" must be at most 247.`  It
    /// never repeats a value that was given, except a name cut short.
    std::string line;

    friend bool operator==(const parameter_error&,
                           const parameter_error&) = default;
};

/// Where the given values come from.
enum class value_source
{
    /// What a user typed into the form just now.  A secret may be given and
    /// is required when it has no default (it never has one).
    user,
    /// What a setup file holds.  A setup never keeps a secret, so one is
    /// refused when it is there and is not asked for when it is not.
    setup
};

/// What `validate` found: one value for each declared parameter, in the order
/// of the declaration, or the refusals.  `values` is empty when `errors` is
/// not, and the other way round.  At most one refusal for each parameter and
/// each name that was given.
struct validation
{
    std::vector<checked_parameter> values;
    std::vector<parameter_error> errors;

    bool ok() const noexcept { return errors.empty(); }
};

/// Checks the values given against the parameters an extension point (or a
/// bundle section) declares.  Each declared parameter takes the given value
/// or else its default.  A parameter with no default must be given.  The
/// limits are those of docs/plugins.md, section 7.8:
///
/// - `bool`: true or false.
/// - `int`: a whole number inside `min` and `max` when they are declared.
///   `step` is for the form and is not checked.
/// - `number`: a finite number, or a whole number, inside `min` and `max`.
/// - `duration`: whole units of `ns`, `us`, `ms` or `s` as text, from 0 to
///   one day and inside `min` and `max`.
/// - `enum`: the value of one of the options.
/// - `string`: plain text of at most `max_length` bytes.
/// - `bytes`: hexadecimal text of at most `max_length` bytes.
/// - `port`, `link`: a name of 1 to `max_name_length` bytes of plain text.
///   Whether such a port or link exists is for the host to check.
/// - `path`: see `path_scope`.  Both kinds of separator are read as the
///   system reads them.
/// - `secret`: plain text of at most `max_length` bytes.
validation validate(std::span<const parameter> declared,
                    std::span<const raw_parameter> given,
                    value_source source = value_source::user);

// -- text ---------------------------------------------------------------------

/// Writes the values as one compact JSON object with the keys in order of
/// their ids, so the same values give the same text.  Spelling: `bool`,
/// `int` and `number` as JSON; a `duration` in the largest unit that holds it
/// exactly (`3ms`, `1750us`, `0s`); `bytes` as lower-case hex with no
/// spaces; the rest as text.  A secret is not written when `source` is
/// `value_source::setup` and is when it is `value_source::user` (the text of
/// the plugin API messages).  The text reads back through `from_json` and
/// `validate` to the same values.
std::string to_json(std::span<const checked_parameter> values,
                    value_source source);

/// Reads one JSON object of plain values: at most `max_parameters_json_size`
/// bytes, `max_parameters_json_depth` deep, at most `max_parameters` members,
/// no key twice, values that are a boolean, a number or text of at most
/// `max_parameter_text_size` bytes.  Anything else is refused and `problem`
/// says why, as the end of a sentence, without repeating what was sent.
std::optional<raw_parameters> from_json(std::string_view text,
                                        std::string& problem);

/// The `params` of a plugin binding in a setup file: `from_json`, except that
/// empty text (or only white space) is the empty set, which is how a binding
/// without settings is written.
std::optional<raw_parameters> from_binding_params(std::string_view params,
                                                  std::string& problem);

/// What to put in `plugin_binding::params`: `to_json` for a setup, and empty
/// text when no value is left to write.
std::string to_binding_params(std::span<const checked_parameter> values);

/// A text of the values for comparing and hashing: the same values give the
/// same text, whatever the order they were given in.  It is `to_json` for a
/// setup, so it leaves a secret out.
std::string stable_text(std::span<const checked_parameter> values);

} // namespace vss::plugin
