//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the readers and writers of the extension point messages share: the
// strict reading of one member of a header, and the two functions that every
// message goes through, `decode_with` and `encode_with`.  protocol.cpp has its
// own copies of the member readers, in its unnamed namespace, so that file
// stays as it was written; these are the same rules.  This header is not
// installed.
//

#pragma once

#include <vss/plugin/api_message.hpp>

#include "reader.hpp"

#include <boost/json.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::plugin::detail
{

namespace json = boost::json;
using namespace std::string_view_literals;

// -- the reader's state
// --------------------------------------------------------

/// The reader stops at the first thing that is wrong and keeps the reason.
struct reading
{
    std::string problem;

    bool failed() const noexcept { return !problem.empty(); }

    void fail(std::string_view where, std::string_view what)
    {
        if (failed())
            return;
        problem.assign(where);
        problem += ": ";
        problem += what;
    }
};

std::string_view view_of(const json::string& text) noexcept;

/// `message.key`, or `key` alone when `where` is empty.
std::string member(std::string_view where, std::string_view key);

/// `where[index]`, for an entry of a list.
std::string entry(std::string_view where, std::size_t index);

// -- one member
// ----------------------------------------------------------------

/// The member as a whole number from `least` to `most`.  Nothing when it is
/// absent or not one; an absent member is an error only when `required`.
std::optional<std::uint64_t>
number_of(reading& state, const json::object& object, std::string_view where,
          std::string_view key, std::uint64_t least, std::uint64_t most,
          bool required);

/// The member as a number that JSON can write, from `least` to `most`.  It
/// may be a whole number or a fraction.
std::optional<double> real_of(reading& state, const json::object& object,
                              std::string_view where, std::string_view key,
                              double least, double most, bool required);

std::optional<bool> bool_of(reading& state, const json::object& object,
                            std::string_view where, std::string_view key,
                            bool required);

/// A plain text of at most `most` bytes.  It may be empty unless
/// `not_empty`.  Empty when the member is absent or wrong.
std::string text_of(reading& state, const json::object& object,
                    std::string_view where, std::string_view key,
                    std::size_t most, bool required, bool not_empty = false);

/// A name of 1 to `most` lower-case letters, digits, dots, hyphens and
/// underscores.  Empty when the member is absent or wrong.
std::string token_of(reading& state, const json::object& object,
                     std::string_view where, std::string_view key,
                     std::size_t most, bool required);

/// The member as a list of at most `most` entries.  Null when it is absent
/// or something else; an absent member is an error only when `required`.
const json::array* list_of(reading& state, const json::object& object,
                           std::string_view where, std::string_view key,
                           std::size_t most, bool required);

/// The member as an object.  Null when it is absent or something else.
const json::object* object_of(reading& state, const json::object& object,
                              std::string_view where, std::string_view key,
                              bool required);

/// An entry of a list that must be an object.  Null, and a failure, when
/// it is something else.
const json::object* entry_object(reading& state, const json::value& value,
                                 std::string_view where);

/// A JSON object kept as one compact text, as the "settings" of a
/// "hello_ack" are: it holds no number that JSON cannot write, and the text
/// reads back as itself.  "{}" when the member is absent and not `required`.
std::string object_text_of(reading& state, const json::object& object,
                           std::string_view where, std::string_view key,
                           bool required);

/// What a failed `parse_strict` says, without anything the plugin sent: a
/// repeated key is quoted in the reason that the parser gives.
std::string json_problem(const std::string& what);

/// A plain text of 1 to `max_api_name` bytes: the name of a place.  Empty when
/// the member is absent or wrong.
std::string place_of(reading& state, const json::object& object,
                     std::string_view where, std::string_view key,
                     bool required);

/// A list of at most `most` names of places (`place_of`), none twice.  Empty
/// when the member is absent or wrong; an absent member is an error only when
/// `required`, and an empty list only when `not_empty` is false.
std::vector<std::string> place_list_of(reading& state,
                                       const json::object& object,
                                       std::string_view where,
                                       std::string_view key, std::size_t most,
                                       bool required, bool not_empty);

/// A path inside the plugin's data directory, by the rules of
/// `check_relative_path`, of 1 to `most` bytes.  Empty when the member is
/// absent or wrong.
std::string relative_path_of(reading& state, const json::object& object,
                             std::string_view where, std::string_view key,
                             std::size_t most, bool required);

/// Whether two names are the same when the case of ASCII letters is ignored.
bool same_name_ignoring_case(std::string_view first,
                             std::string_view second) noexcept;

/// The member as one of `names`, and which one.  `what` completes the
/// sentence "must be ".
template <class Enum, std::size_t N>
std::optional<Enum>
choice_of(reading& state, const json::object& object, std::string_view where,
          std::string_view key,
          const std::array<std::pair<std::string_view, Enum>, N>& names,
          std::string_view what, bool required)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr)
    {
        if (required)
            state.fail(member(where, key), "is required");
        return std::nullopt;
    }
    if (value->is_string())
    {
        const std::string_view text = view_of(value->get_string());
        for (const auto& [name, one] : names)
        {
            if (name == text)
                return one;
        }
    }
    state.fail(member(where, key), std::string("must be ") + std::string(what));
    return std::nullopt;
}

/// The name that `names` gives to `value`.
template <class Enum, std::size_t N>
std::string_view
name_of(const std::array<std::pair<std::string_view, Enum>, N>& names,
        Enum value) noexcept
{
    for (const auto& [name, one] : names)
    {
        if (one == value)
            return name;
    }
    return "unknown";
}

// -- the blob
// ------------------------------------------------------------------

/// The blob must be empty.
void no_blob(reading& state, std::string_view where, const byte_buffer& blob);

/// The blob must have `least` to `most` bytes.
void blob_between(reading& state, std::string_view where,
                  const byte_buffer& blob, std::size_t least, std::size_t most);

// -- writing
// -------------------------------------------------------------------

json::string text_value(std::string_view text);

/// A member that is left out when the text is empty.
void put_optional(json::object& header, std::string_view key,
                  const std::string& text);

/// The compact JSON object `text` as a member.  False when it is not one.
bool put_object_text(json::object& header, std::string_view key,
                     const std::string& text);

// -- the two functions every message goes through
// -------------------------------

/// Opens the header of `message` for the message `op` with the `role`.  Null,
/// and a failure, when the message is not that one.
std::optional<json::value> open_message(reading& state,
                                        const api_message& message,
                                        std::string_view op, api_role role);

/// The `api_message` for `header`, an object that has its "op" and its
/// number already.
result<api_message> close_message(std::string_view op, api_role role,
                                  std::uint64_t number, json::object header,
                                  byte_buffer blob);

template <class M>
concept has_bytes = requires(const M& body) {
    { body.data } -> std::convertible_to<const byte_buffer&>;
};

/// `Read` is `void(reading&, const json::object&, M&)`.
template <class M, class Read>
result<M> decode_with(const api_message& message, std::string* explain,
                      Read read)
{
    try
    {
        reading state;
        M out;
        const std::optional<json::value> root =
            open_message(state, message, M::op(), M::role());
        if (root && !state.failed())
        {
            if constexpr (has_bytes<M>)
                out.data = message.blob;
            else
                no_blob(state, M::op(), message.blob);
            if (!state.failed())
                read(state, root->get_object(), out);
        }
        if (state.failed())
        {
            if (explain != nullptr)
                *explain = state.problem;
            return make_error_code(api_errc::bad_request);
        }
        if (explain != nullptr)
            explain->clear();
        return out;
    }
    catch (...)
    {
        // Only running out of memory gets here.  The message was not wrong.
        return make_error_code(errc::internal);
    }
}

/// `Write` is `bool(json::object&, const M&)`.  It returns false for a value
/// that cannot be written.  What is written is read back, and the value read
/// must be the one written: whatever the reader would refuse, or would read
/// as something else, is refused here.  The header is held to the limit of
/// `max_header_size`, as `encode_message` holds it.
template <class M, class Write>
result<api_message> encode_with(const M& body, std::uint64_t number,
                                Write write)
{
    try
    {
        if (M::role() == api_role::notification && number != 0)
            return make_error_code(api_errc::bad_request);
        json::object header;
        if (!write(header, body))
            return make_error_code(api_errc::bad_request);
        byte_buffer blob;
        if constexpr (has_bytes<M>)
            blob = body.data;
        result<api_message> out = close_message(
            M::op(), M::role(), number, std::move(header), std::move(blob));
        if (out.has_error())
            return out;
        if (std::get<other_message>(out.value().body).header.size() >
            max_header_size)
            return make_error_code(api_errc::bad_request);
        const auto again = decode_as<M>(out.value());
        if (again.has_error())
            return again.error();
        if (!(again.value() == body))
            return make_error_code(api_errc::bad_request);
        return out;
    }
    catch (...)
    {
        return make_error_code(errc::internal);
    }
}

} // namespace vss::plugin::detail
