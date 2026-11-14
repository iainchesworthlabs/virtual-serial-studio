//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/signature.hpp>

#include <vss/license/signed_file.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/policy.hpp>

#include "reader.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <utility>

namespace vss::plugin
{

namespace
{

namespace json = boost::json;
namespace sf = vss::license::signed_file;
using namespace std::string_view_literals;

constexpr std::array list_keys{"key_list_version"sv, "serial"sv, "issued"sv,
                               "keys"sv, "revoked"sv};

constexpr std::array entry_keys{"key_id"sv,     "public_key"sv, "purpose"sv,
                                "name"sv,       "valid_from"sv, "valid_to"sv};

constexpr std::array payload_keys{"plugin_id"sv, "version"sv,
                                  "manifest_sha512"sv};

/// How deep the JSON of a key list or of a signature's payload may nest.
constexpr std::size_t max_depth = 4;

std::string_view view_of(const json::string& text) noexcept
{
    return {text.data(), text.size()};
}

/// The payload of an envelope as text.
std::string text_of(const std::vector<std::uint8_t>& payload)
{
    std::string out;
    out.reserve(payload.size());
    for (const std::uint8_t byte : payload)
        out += static_cast<char>(byte);
    return out;
}

void set_detail(std::string* explain, std::string text)
{
    if (explain != nullptr)
        *explain = std::move(text);
}

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

/// The object, when `value` is one and has no key outside `known`.
template <std::size_t N>
const json::object* object_of(reading& state, const json::value& value,
                              std::string_view where,
                              const std::array<std::string_view, N>& known)
{
    if (!value.is_object())
    {
        state.fail(where, "must be an object");
        return nullptr;
    }
    const json::object& object = value.get_object();
    for (const auto& entry : object)
    {
        const std::string_view key{entry.key().data(), entry.key().size()};
        if (std::ranges::find(known, key) == known.end())
        {
            state.fail(where, "unknown key " + detail::quoted(key));
            return nullptr;
        }
    }
    return &object;
}

/// The member as a string; an error when it is missing or something else.
std::optional<std::string_view> text_member(reading& state,
                                            const json::object& object,
                                            std::string_view key,
                                            std::string_view where)
{
    const json::value* const value = object.if_contains(key);
    if (value == nullptr || !value->is_string())
    {
        state.fail(where, "must be a string");
        return std::nullopt;
    }
    return view_of(value->get_string());
}

std::optional<std::chrono::year_month_day>
date_member(reading& state, const json::object& object, std::string_view key,
            std::string_view where)
{
    const auto text = text_member(state, object, key, where);
    if (!text)
        return std::nullopt;
    const auto date = detail::date_from_text(*text);
    if (!date)
        state.fail(where, "must be a date such as 2026-11-13");
    return date;
}

std::optional<official_key> read_key(reading& state, const json::value& value)
{
    const json::object* const entry =
        object_of(state, value, "keys", entry_keys);
    if (entry == nullptr)
        return std::nullopt;

    official_key key;
    const auto id = text_member(state, *entry, "key_id", "keys.key_id");
    const auto bytes =
        text_member(state, *entry, "public_key", "keys.public_key");
    const auto purpose = text_member(state, *entry, "purpose", "keys.purpose");
    const auto name = text_member(state, *entry, "name", "keys.name");
    const auto from =
        date_member(state, *entry, "valid_from", "keys.valid_from");
    const auto to = date_member(state, *entry, "valid_to", "keys.valid_to");
    if (!id || !bytes || !purpose || !name || !from || !to)
        return std::nullopt;

    if (!is_key_id(*id))
    {
        state.fail("keys.key_id", "cannot be " + detail::quoted(*id));
        return std::nullopt;
    }
    if (!detail::hex_to_bytes(*bytes, key.public_key))
    {
        state.fail("keys.public_key", "must be 64 lower-case hex characters");
        return std::nullopt;
    }
    // Only the two purposes of plugins.  A key for anything else, a licence
    // key say, has no place in this list.
    if (*purpose != plugin_purpose && *purpose != revocations_purpose)
    {
        state.fail("keys.purpose", "cannot be " + detail::quoted(*purpose));
        return std::nullopt;
    }
    if (name->empty() || name->size() > max_name_length ||
        !detail::clean_text(*name))
    {
        state.fail("keys.name", "must be a short plain text");
        return std::nullopt;
    }
    if (std::chrono::sys_days{*to} < std::chrono::sys_days{*from})
    {
        state.fail("keys.valid_to", "is before valid_from");
        return std::nullopt;
    }
    key.key_id = std::string(*id);
    key.purpose = std::string(*purpose);
    key.name = std::string(*name);
    key.valid_from = *from;
    key.valid_to = *to;
    return key;
}

/// Whether the envelope's signature verifies with `key`, as a key with this
/// id for this purpose.
bool verifies(const sf::envelope& file, std::string_view purpose,
              std::string_view key_id, const key_bytes& key)
{
    sf::trusted_key trusted;
    trusted.key_id = std::string(key_id);
    trusted.purpose = std::string(purpose);
    trusted.key = key;
    return sf::verify(file, purpose, std::span<const sf::trusted_key>(
                                         &trusted, 1))
        .has_value();
}

template <class T>
result<T> not_signed(std::string* explain, std::string text)
{
    set_detail(explain, std::move(text));
    return make_error_code(plugin_errc::signature_invalid);
}

signature_finding invalid(std::string* explain, std::string text)
{
    set_detail(explain, std::move(text));
    signature_finding found;
    found.state = signature_state::invalid;
    return found;
}

signature_finding holds(std::string key_id, key_origin origin)
{
    signature_finding found;
    found.state = signature_state::valid;
    found.key_id = std::move(key_id);
    found.origin = origin;
    return found;
}

/// What the payload of a `plugin.sig` says, or nothing when it is not one.
struct signed_claim
{
    std::string plugin_id;
    std::string version;
    std::string manifest_sha512;
};

std::optional<signed_claim> read_claim(const std::string& payload)
{
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(payload, max_depth, what);
    if (!root)
        return std::nullopt;
    reading state;
    const json::object* const object =
        object_of(state, *root, "payload", payload_keys);
    if (object == nullptr)
        return std::nullopt;
    const auto id = text_member(state, *object, "plugin_id", "plugin_id");
    const auto version = text_member(state, *object, "version", "version");
    const auto digest =
        text_member(state, *object, "manifest_sha512", "manifest_sha512");
    if (!id || !version || !digest)
        return std::nullopt;
    return signed_claim{std::string(*id), std::string(*version),
                        std::string(*digest)};
}

const approved_signer* find_signer(std::span<const approved_signer> keys,
                                   std::string_view key_id) noexcept
{
    const auto found = std::ranges::find(keys, key_id,
                                         &approved_signer::key_id);
    return found == keys.end() ? nullptr : &*found;
}

} // namespace

const official_key* key_list::find(std::string_view key_id,
                                   std::string_view purpose) const noexcept
{
    const auto found =
        std::ranges::find_if(keys, [&](const official_key& key) {
            return key.key_id == key_id && key.purpose == purpose;
        });
    return found == keys.end() ? nullptr : &*found;
}

bool key_list::is_revoked(std::string_view key_id) const noexcept
{
    return std::ranges::find(revoked, key_id) != revoked.end();
}

bool counts_on(const official_key& key, std::chrono::sys_days day) noexcept
{
    return std::chrono::sys_days{key.valid_from} <= day &&
           day <= std::chrono::sys_days{key.valid_to};
}

result<key_list> parse_key_list(std::string_view payload, std::string* explain)
{
    reading state;
    const auto failure = [&]() -> result<key_list> {
        set_detail(explain, state.problem);
        return make_error_code(errc::config_error);
    };
    std::string what;
    const std::optional<json::value> root =
        detail::parse_strict(payload, max_depth, what);
    if (!root)
    {
        state.fail("key list", what);
        return failure();
    }
    const json::object* const object =
        object_of(state, *root, "key list", list_keys);
    if (object == nullptr)
        return failure();

    const json::value* const version = object->if_contains("key_list_version");
    if (version == nullptr || !version->is_int64() ||
        version->get_int64() != std::int64_t{key_list_format_version})
    {
        state.fail("key_list_version",
                   "must be " + std::to_string(key_list_format_version));
        return failure();
    }

    key_list out;
    const json::value* const serial = object->if_contains("serial");
    if (serial == nullptr || !serial->is_int64() || serial->get_int64() < 1 ||
        static_cast<std::uint64_t>(serial->get_int64()) >
            max_revocations_serial)
    {
        state.fail("serial", "must be a whole number from 1 to " +
                                 std::to_string(max_revocations_serial));
        return failure();
    }
    out.serial = static_cast<std::uint64_t>(serial->get_int64());

    if (const auto issued = date_member(state, *object, "issued", "issued"))
        out.issued = *issued;
    if (state.failed())
        return failure();

    if (const json::value* const keys = object->if_contains("keys"))
    {
        if (!keys->is_array() || keys->get_array().size() > max_official_keys)
        {
            state.fail("keys", "must be a list of at most " +
                                   std::to_string(max_official_keys));
            return failure();
        }
        for (const json::value& item : keys->get_array())
        {
            std::optional<official_key> key = read_key(state, item);
            if (!key)
                return failure();
            // One id is one key, whatever its purpose: a signature names
            // its key by id alone.
            if (std::ranges::find(out.keys, key->key_id,
                                  &official_key::key_id) != out.keys.end())
            {
                state.fail("keys", "holds " + detail::quoted(key->key_id) +
                                       " twice");
                return failure();
            }
            out.keys.push_back(std::move(*key));
        }
    }

    if (const json::value* const revoked = object->if_contains("revoked"))
    {
        if (!revoked->is_array() ||
            revoked->get_array().size() > max_revoked_keys)
        {
            state.fail("revoked", "must be a list of at most " +
                                      std::to_string(max_revoked_keys));
            return failure();
        }
        for (const json::value& item : revoked->get_array())
        {
            if (!item.is_string() || !is_key_id(view_of(item.get_string())))
            {
                state.fail("revoked", "holds something that is not a key id");
                return failure();
            }
            out.revoked.emplace_back(view_of(item.get_string()));
        }
    }

    set_detail(explain, std::string{});
    return out;
}

result<key_list> read_key_list(std::string_view file_text,
                               std::span<const root_key> roots,
                               std::string* explain)
{
    const auto file = sf::parse(file_text);
    if (file.has_error())
        return not_signed<key_list>(explain, "the key list is not a signed "
                                             "file");
    if (file->purpose != key_list_purpose)
        return not_signed<key_list>(explain,
                                    "the file is not signed as a key list");
    const auto root = std::ranges::find(roots, file->key_id,
                                        &root_key::key_id);
    if (root == roots.end())
        return not_signed<key_list>(
            explain, "the key list is not signed with a root key");
    if (!verifies(*file, key_list_purpose, root->key_id, root->public_key))
        return not_signed<key_list>(
            explain, "the signature of the key list does not hold");
    return parse_key_list(text_of(file->payload), explain);
}

result<revocation_list> read_revocations(std::string_view file_text,
                                         const key_list& official,
                                         std::string* explain)
{
    using list = revocation_list;
    const auto file = sf::parse(file_text);
    if (file.has_error())
        return not_signed<list>(explain, "the revocation list is not a "
                                         "signed file");
    if (file->purpose != revocations_purpose)
        return not_signed<list>(explain, "the file is not signed as a "
                                         "revocation list");
    const official_key* const key =
        official.find(file->key_id, revocations_purpose);
    if (key == nullptr)
        return not_signed<list>(explain, "the revocation list is not signed "
                                         "with an official key for "
                                         "revocation lists");
    if (official.is_revoked(key->key_id))
        return not_signed<list>(explain, "the key that signed the revocation "
                                         "list was withdrawn");
    if (!verifies(*file, revocations_purpose, key->key_id, key->public_key))
        return not_signed<list>(explain, "the signature of the revocation "
                                         "list does not hold");

    result<revocation_list> parsed =
        parse_revocations(text_of(file->payload), explain);
    if (parsed.has_error())
        return parsed;
    if (!counts_on(*key, std::chrono::sys_days{parsed->issued_at}))
        return not_signed<list>(explain, "the key that signed the revocation "
                                         "list did not count on the day the "
                                         "list was issued");
    return parsed;
}

signature_finding check_signature(const signature_input& input,
                                  std::string* explain)
{
    set_detail(explain, std::string{});
    if (!input.signature_text)
        return signature_finding{};
    if (input.declared == nullptr)
        return invalid(explain, "no manifest was given");

    const auto file = sf::parse(*input.signature_text);
    if (file.has_error())
        return invalid(explain, "plugin.sig is not a signed file");
    if (file->purpose != plugin_purpose)
        return invalid(explain, "plugin.sig is not signed as a plugin");
    if (!is_key_id(file->key_id))
        return invalid(explain, "plugin.sig names no usable key");

    // What was signed must be this plugin: its id, its version and the
    // exact bytes of its manifest.
    const std::optional<signed_claim> claim =
        read_claim(text_of(file->payload));
    if (!claim)
        return invalid(explain, "the signed part of plugin.sig is not as it "
                                "should be");
    if (claim->plugin_id != input.declared->id)
        return invalid(explain, "plugin.sig was made for another plugin");
    if (claim->version != to_string(input.declared->version))
        return invalid(explain, "plugin.sig was made for another version");
    if (claim->manifest_sha512 != to_hex(sha512_of(input.manifest_bytes)))
        return invalid(explain, "plugin.json is not the one that was signed");

    // The key, by its id.  The first place that knows the id decides.
    if (input.official != nullptr)
    {
        const key_list& official = *input.official;
        if (official.is_revoked(file->key_id))
            return invalid(explain, "its signing key was withdrawn");
        const auto known = std::ranges::find(official.keys, file->key_id,
                                             &official_key::key_id);
        if (known != official.keys.end())
        {
            if (known->purpose != plugin_purpose)
                return invalid(explain, "its signing key is not a key for "
                                        "plugins");
            if (!counts_on(*known, input.today))
                return invalid(explain, "its signing key does not count "
                                        "today");
            if (!verifies(*file, plugin_purpose, known->key_id,
                          known->public_key))
                return invalid(explain, "the signature does not hold");
            return holds(file->key_id, key_origin::official);
        }
    }
    if (const approved_signer* const signer =
            find_signer(input.administrator_keys, file->key_id))
    {
        if (!verifies(*file, plugin_purpose, signer->key_id,
                      signer->public_key))
            return invalid(explain, "the signature does not hold");
        return holds(file->key_id, key_origin::administrator);
    }
    if (const approved_signer* const signer =
            find_signer(input.user_keys, file->key_id))
    {
        if (!verifies(*file, plugin_purpose, signer->key_id,
                      signer->public_key))
            return invalid(explain, "the signature does not hold");
        return holds(file->key_id, key_origin::user);
    }

    signature_finding found;
    found.state = signature_state::unknown_key;
    found.key_id = file->key_id;
    return found;
}

} // namespace vss::plugin
