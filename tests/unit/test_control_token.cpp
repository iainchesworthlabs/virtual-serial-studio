//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tokens without a connection: the type that keeps a secret from being
// printed, the form of a token, and the store that makes, checks and revokes
// them (docs/remote-control.md, sections 4, 11 and 15).  The store is given
// its clock, so expiry and a clock that is set back are tested without
// waiting.
//

#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/secret.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/control/token_store.hpp>
#include <vss/core/bytes.hpp>
#include <vss/core/error.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace control = vss::control;
using namespace std::chrono_literals;

namespace
{

// -- what the secret type must not be able to do ------------------------------
//
// These are checked by the compiler: a change that lets a secret be printed
// or passed where a text is expected stops the build of this file.

template <class Type>
concept streamable =
    requires(std::ostream& out, const Type& value) { out << value; };

static_assert(!streamable<control::secret_string>);
static_assert(!std::is_convertible_v<control::secret_string, std::string>);
static_assert(!std::is_convertible_v<control::secret_string, std::string_view>);
static_assert(!std::is_convertible_v<control::secret_string, const char*>);
static_assert(!std::is_convertible_v<std::string, control::secret_string>);
static_assert(!std::is_convertible_v<std::string_view, control::secret_string>);
static_assert(!std::is_convertible_v<const char*, control::secret_string>);
static_assert(
    !std::is_constructible_v<std::string, const control::secret_string&>);

vss::error_code code(vss::errc value)
{
    return vss::make_error_code(value);
}

std::string hex_of(const control::token_digest& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t value : digest)
    {
        out.push_back(digits[static_cast<std::size_t>(value >> 4)]);
        out.push_back(digits[static_cast<std::size_t>(value & 0x0fu)]);
    }
    return out;
}

std::string bytes_of(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

/// A clock a test moves by hand.
struct fake_clock
{
    std::shared_ptr<control::wall_clock::time_point> now =
        std::make_shared<control::wall_clock::time_point>(
            control::wall_clock::time_point(
                std::chrono::milliseconds(1'800'000'000'000)));

    control::token_store::time_source source() const
    {
        return [shared = now] { return *shared; };
    }

    void advance(std::chrono::seconds by) const { *now += by; }
    void set_back(std::chrono::seconds by) const { *now -= by; }
};

/// A token store in a directory of its own, with a clock the test holds.
struct fixture
{
    fixture() { reopen(); }

    void reopen()
    {
        store.reset();
        auto opened = control::token_store::open(file, clock.source());
        REQUIRE(opened);
        store = std::move(*opened);
    }

    control::new_token make(control::scope_set scopes = control::role_monitor(),
                            std::chrono::seconds life = 3600s)
    {
        control::token_request request;
        request.label = "test";
        request.scopes = scopes;
        request.life = life;
        auto made = store->create(request);
        REQUIRE(made);
        return std::move(*made);
    }

    control::token_check check(const control::secret_string& token) const
    {
        return store->verify(token, control::transport::local).outcome;
    }

    vss_test_support::scratch_directory scratch;
    std::filesystem::path file = scratch.path() / "state" / "tokens.json";
    fake_clock clock;
    std::unique_ptr<control::token_store> store;
};

/// The token with one character changed at `index`.
control::secret_string altered(const control::secret_string& token,
                               std::size_t index)
{
    std::string text(token.expose());
    text[index] = text[index] == 'a' ? 'b' : 'a';
    return control::secret_string(text);
}

} // namespace

// -- the secret type ---------------------------------------------------------

TEST_CASE("control secret: it holds what it was given and gives it up only "
          "when asked")
{
    const control::secret_string empty;
    REQUIRE(empty.empty());
    REQUIRE_EQ(empty.size(), std::size_t{0});

    const control::secret_string secret(std::string_view("open sesame"));
    REQUIRE(!secret.empty());
    REQUIRE_EQ(secret.size(), std::size_t{11});
    REQUIRE_EQ(secret.expose(), std::string_view("open sesame"));

    const control::secret_string same(std::string_view("open sesame"));
    const control::secret_string other(std::string_view("open sesamf"));
    const control::secret_string shorter(std::string_view("open sesam"));
    REQUIRE(secret == same);
    REQUIRE(!(secret == other));
    REQUIRE(!(secret == shorter));
    REQUIRE(!(secret == empty));
}

TEST_CASE("control secret: moving and assigning leave nothing behind")
{
    control::secret_string first(std::string_view("first secret"));
    control::secret_string moved(std::move(first));
    // NOLINTNEXTLINE(bugprone-use-after-move): the point of the test.
    REQUIRE(first.empty());
    REQUIRE_EQ(moved.expose(), std::string_view("first secret"));

    control::secret_string target(std::string_view("old"));
    target = std::move(moved);
    // NOLINTNEXTLINE(bugprone-use-after-move): the point of the test.
    REQUIRE(moved.empty());
    REQUIRE_EQ(target.expose(), std::string_view("first secret"));

    const control::secret_string copy = target;
    REQUIRE(copy == target);
    target = control::secret_string();
    REQUIRE(target.empty());
    REQUIRE_EQ(copy.expose(), std::string_view("first secret"));
}

TEST_CASE("control secret: wiping writes zeros over every byte")
{
    std::string text = "wipe me, all of me";
    control::secure_wipe(text.data(), text.size());
    REQUIRE_EQ(text, std::string(18, '\0'));
    control::secure_wipe(nullptr, 0);
}

TEST_CASE("control secret: equal means every byte, wherever the difference "
          "is")
{
    const std::string base(64, 'k');
    REQUIRE(control::constant_time_equal(base, base));
    REQUIRE(control::constant_time_equal("", ""));
    for (std::size_t index = 0; index < base.size(); ++index)
    {
        std::string changed = base;
        changed[index] = 'j';
        REQUIRE(!control::constant_time_equal(base, changed));
        // One bit is enough.
        changed[index] = static_cast<char>(base[index] ^ 0x01);
        REQUIRE(!control::constant_time_equal(base, changed));
    }
    REQUIRE(!control::constant_time_equal(base, base.substr(1)));
    REQUIRE(!control::constant_time_equal(base, base + "k"));
}

// -- the form of a token -----------------------------------------------------

TEST_CASE("control token: base32 is the lower-case alphabet without padding")
{
    const auto encoded = [](std::string_view text)
    {
        return control::base32_encode(vss::bytes_view(
            reinterpret_cast<const vss::byte*>(text.data()), text.size()));
    };
    // The vectors of RFC 4648, in lower case and without the padding.
    REQUIRE_EQ(encoded(""), std::string(""));
    REQUIRE_EQ(encoded("f"), std::string("my"));
    REQUIRE_EQ(encoded("fo"), std::string("mzxq"));
    REQUIRE_EQ(encoded("foo"), std::string("mzxw6"));
    REQUIRE_EQ(encoded("foob"), std::string("mzxw6yq"));
    REQUIRE_EQ(encoded("fooba"), std::string("mzxw6ytb"));
    REQUIRE_EQ(encoded("foobar"), std::string("mzxw6ytboi"));

    // The sizes a token is made of.
    const vss::byte_buffer id(control::token_id_bytes, vss::byte{0xff});
    const vss::byte_buffer secret(control::token_secret_bytes, vss::byte{0xff});
    REQUIRE_EQ(control::base32_encode(id).size(), control::token_id_size);
    REQUIRE_EQ(control::base32_encode(secret).size(),
               control::token_secret_size);
    REQUIRE_EQ(control::token_size, std::size_t{72});
}

TEST_CASE("control token: only exactly the form of a token is one")
{
    const std::string id(control::token_id_size, 'a');
    const std::string secret(control::token_secret_size, '7');
    const std::string good = "vsst1_" + id + "_" + secret;
    REQUIRE_EQ(good.size(), control::token_size);

    const auto parts = control::parse_token(control::secret_string(good));
    REQUIRE(parts.has_value());
    REQUIRE_EQ(parts->id, id);
    REQUIRE_EQ(parts->secret.expose(), std::string_view(secret));
    REQUIRE(control::make_token(id, parts->secret) ==
            control::secret_string(good));
    REQUIRE(control::valid_token_id(id));

    const std::vector<std::string> bad{
        "", "vsst1", good.substr(0, good.size() - 1), good + "a", " " + good,
        good + "\n", "vsst2_" + id + "_" + secret, "VSST1_" + id + "_" + secret,
        "vsst1-" + id + "_" + secret, "vsst1_" + id + "-" + secret,
        // Upper case, and the digits base32 does not have.
        "vsst1_" + std::string(control::token_id_size, 'A') + "_" + secret,
        "vsst1_" + id + "_" + std::string(control::token_secret_size, 'A'),
        "vsst1_" + id + "_" + std::string(control::token_secret_size, '1'),
        "vsst1_" + std::string(control::token_id_size, '0') + "_" + secret,
        "vsst1_" + std::string(control::token_id_size, '_') + "_" + secret,
        // The right length with the separators in the wrong places.
        "vsst1_" + id + "a" + "_" + secret.substr(1),
        std::string(control::token_size, 'a'),
        std::string(control::token_size, '\0')};
    for (const std::string& text : bad)
    {
        REQUIRE(!control::parse_token(control::secret_string(text)));
    }
    REQUIRE(!control::valid_token_id(""));
    REQUIRE(!control::valid_token_id(id + "a"));
    REQUIRE(!control::valid_token_id(std::string(control::token_id_size, 'A')));
}

TEST_CASE("control token: the hash is SHA-256 of a label and the secret")
{
    const control::secret_string secret(
        std::string(control::token_secret_size, 'a'));
    // sha256("vsst1-token" + 52 * "a"), from another implementation.
    REQUIRE_EQ(hex_of(control::hash_token_secret(secret)),
               std::string("8dc8a966b923653d098cd418814cd96a"
                           "9db0aa904a9f57c2ef2ef1b711616d42"));

    const control::token_digest first = control::hash_token_secret(secret);
    control::token_digest second = first;
    REQUIRE(control::same_digest(first, second));
    for (std::size_t index = 0; index < second.size(); ++index)
    {
        second = first;
        second[index] = static_cast<std::uint8_t>(second[index] ^ 0x80u);
        REQUIRE(!control::same_digest(first, second));
    }
}

// -- the store ---------------------------------------------------------------

TEST_CASE("control tokens: a token that was made is accepted, with what it "
          "may do")
{
    fixture env;
    control::token_request request;
    request.label = "Lab rig 3";
    request.scopes = control::role_control();
    request.life = 7200s;
    const auto made = env.store->create(request);
    REQUIRE(made);

    const auto parts = control::parse_token(made->token);
    REQUIRE(parts.has_value());
    REQUIRE_EQ(parts->id, made->record.id);
    REQUIRE_EQ(made->record.label, std::string("Lab rig 3"));
    REQUIRE(made->record.scopes == control::role_control());
    REQUIRE(made->record.expires - made->record.created == 7200s);
    REQUIRE(!made->record.revoked);
    REQUIRE(!made->record.last_used.has_value());
    REQUIRE(made->record.hash == control::hash_token_secret(parts->secret));

    const auto verdict =
        env.store->verify(made->token, control::transport::local);
    REQUIRE(verdict.outcome == control::token_check::accepted);
    REQUIRE_EQ(verdict.id, made->record.id);
    REQUIRE(verdict.scopes == control::role_control());
    REQUIRE(verdict.expires == made->record.expires);
    REQUIRE(env.store->standing(made->record.id) ==
            control::token_check::accepted);

    const auto listed = env.store->list();
    REQUIRE_EQ(listed.size(), std::size_t{1});
    REQUIRE(listed[0].last_used.has_value());
}

TEST_CASE("control tokens: two tokens are never the same")
{
    fixture env;
    std::set<std::string> ids;
    std::set<std::string> secrets;
    for (int index = 0; index < 32; ++index)
    {
        const control::new_token made = env.make();
        const auto parts = control::parse_token(made.token);
        REQUIRE(parts.has_value());
        REQUIRE(ids.insert(parts->id).second);
        REQUIRE(secrets.insert(std::string(parts->secret.expose())).second);
    }
}

TEST_CASE("control tokens: every way of being wrong is refused")
{
    fixture env;
    const control::new_token first = env.make();
    const control::new_token second = env.make();
    const auto first_parts = control::parse_token(first.token);
    const auto second_parts = control::parse_token(second.token);
    REQUIRE(first_parts.has_value());
    REQUIRE(second_parts.has_value());
    const std::string text(first.token.expose());
    using control::token_check;

    // The right id with another token's secret, and the other way round.
    REQUIRE(
        env.check(control::make_token(first_parts->id, second_parts->secret)) ==
        token_check::wrong_secret);
    REQUIRE(
        env.check(control::make_token(second_parts->id, first_parts->secret)) ==
        token_check::wrong_secret);
    // One character of the secret changed, first and last.
    REQUIRE(env.check(altered(first.token, text.size() - 1)) ==
            token_check::wrong_secret);
    REQUIRE(env.check(altered(first.token,
                              text.size() - control::token_secret_size)) ==
            token_check::wrong_secret);
    // One character of the id changed: no such token.
    REQUIRE(env.check(altered(first.token, control::token_prefix.size() + 1)) ==
            token_check::unknown);
    // Cut short, with something after it, in the wrong case, empty.
    for (const std::string& bad :
         {text.substr(0, text.size() - 1), text + "a", text + " ",
          std::string("VSST1") + text.substr(5), std::string(),
          std::string(text.size(), 'a')})
    {
        REQUIRE(env.check(control::secret_string(bad)) ==
                token_check::malformed);
    }
    // And the real ones still work.
    REQUIRE(env.check(first.token) == token_check::accepted);
    REQUIRE(env.check(second.token) == token_check::accepted);
}

TEST_CASE("control tokens: every refusal costs one hash, as an acceptance "
          "does")
{
    fixture env;
    const control::new_token good = env.make();
    const control::new_token revoked = env.make();
    const control::new_token ended = env.make(control::role_monitor(), 60s);
    REQUIRE(env.store->revoke(revoked.record.id));
    env.clock.advance(120s);

    const std::string text(good.token.expose());
    const std::vector<control::secret_string> presented{
        good.token,
        revoked.token,
        ended.token,
        altered(good.token, text.size() - 1),
        altered(good.token, control::token_prefix.size() + 1),
        control::secret_string(std::string_view("not a token")),
        control::secret_string(),
        control::secret_string(std::string(4096, 'x'))};
    for (const control::secret_string& token : presented)
    {
        const std::uint64_t before = env.store->hashes_computed();
        static_cast<void>(env.store->verify(token, control::transport::local));
        REQUIRE_EQ(env.store->hashes_computed() - before, std::uint64_t{1});
    }
}

TEST_CASE("control tokens: a token ends when it says, and setting the clock "
          "back does not bring it back")
{
    fixture env;
    const control::new_token made = env.make(control::role_monitor(), 3600s);
    using control::token_check;

    env.clock.advance(3599s);
    REQUIRE(env.check(made.token) == token_check::accepted);
    env.clock.advance(1s);
    REQUIRE(env.check(made.token) == token_check::expired);
    REQUIRE(env.store->standing(made.record.id) == token_check::expired);

    // The clock goes back a day: the store goes by the latest time it saw.
    env.clock.set_back(86400s);
    REQUIRE(env.check(made.token) == token_check::expired);

    // Also after a restart, because the time is in the file.
    env.reopen();
    REQUIRE(env.check(made.token) == token_check::expired);
    REQUIRE(env.store->standing(made.record.id) == token_check::expired);
}

TEST_CASE("control tokens: revoking is at once, for good, and survives a "
          "restart")
{
    fixture env;
    const control::new_token kept = env.make();
    const control::new_token gone = env.make();
    using control::token_check;

    REQUIRE(env.store->revoke(gone.record.id));
    REQUIRE(env.check(gone.token) == token_check::revoked);
    REQUIRE(env.store->standing(gone.record.id) == token_check::revoked);
    REQUIRE(env.check(kept.token) == token_check::accepted);
    // Twice is not an error; an id nobody has is.
    REQUIRE(env.store->revoke(gone.record.id));
    const auto missing = env.store->revoke("aaaaaaaaaaaaa");
    REQUIRE(!missing);
    REQUIRE_EQ(missing.error(), code(vss::errc::not_found));
    REQUIRE(env.store->standing("aaaaaaaaaaaaa") == token_check::unknown);

    env.reopen();
    REQUIRE(env.check(gone.token) == token_check::revoked);
    REQUIRE(env.check(kept.token) == token_check::accepted);
    const auto listed = env.store->list();
    REQUIRE_EQ(listed.size(), std::size_t{2});
    REQUIRE(!listed[0].revoked);
    REQUIRE(listed[1].revoked);
}

TEST_CASE("control tokens: a token is for the transports it names")
{
    fixture env;
    const control::new_token made = env.make();
    using control::token_check;
    REQUIRE(env.store->verify(made.token, control::transport::local).outcome ==
            token_check::accepted);
    for (const control::transport via :
         {control::transport::loopback, control::transport::tls,
          control::transport::local_admin})
    {
        REQUIRE(env.store->verify(made.token, via).outcome ==
                token_check::wrong_transport);
    }
}

TEST_CASE("control tokens: what no token may be is refused when it is made")
{
    fixture env;
    const auto refused = [&](const control::token_request& request)
    {
        const auto made = env.store->create(request);
        return !made && made.error() == code(vss::errc::invalid_argument);
    };
    control::token_request good;
    good.label = "ok";
    good.scopes = control::role_monitor();
    REQUIRE(env.store->create(good));

    control::token_request request = good;
    request.scopes = {};
    REQUIRE(refused(request));
    // Admin, alone or among others: never in a token.
    request.scopes = control::scope_set{control::scope::admin};
    REQUIRE(refused(request));
    request.scopes = control::every_scope();
    REQUIRE(refused(request));

    request = good;
    request.life = 0s;
    REQUIRE(refused(request));
    request.life = -5s;
    REQUIRE(refused(request));
    request.life = control::longest_token_life + 1s;
    REQUIRE(refused(request));
    request.life = control::longest_token_life;
    REQUIRE(env.store->create(request));

    request = good;
    request.transports.clear();
    REQUIRE(refused(request));
    request.transports = {control::transport::local_admin};
    REQUIRE(refused(request));

    request = good;
    request.label = std::string(control::max_token_label_size + 1, 'x');
    REQUIRE(refused(request));
    request.label = "two\nlines";
    REQUIRE(refused(request));
    request.label = std::string("bad\xff");
    REQUIRE(refused(request));
    request.label = "";
    REQUIRE(env.store->create(request));
}

TEST_CASE("control tokens: there is a most, and ended tokens make room")
{
    fixture env;
    for (std::size_t index = 0; index < control::max_tokens; ++index)
        static_cast<void>(env.make(control::role_monitor(), 60s));
    control::token_request request;
    request.label = "one more";
    request.scopes = control::role_monitor();
    const auto over = env.store->create(request);
    REQUIRE(!over);
    REQUIRE_EQ(over.error(), code(vss::errc::queue_full));

    // A week after they ended, their records are dropped.
    env.clock.advance(60s + 24h * 7);
    REQUIRE(env.store->create(request));
    REQUIRE_EQ(env.store->list().size(), std::size_t{1});
}

TEST_CASE("control tokens: the file holds no token, only what a token "
          "hashes to")
{
    fixture env;
    const control::new_token made = env.make();
    REQUIRE(env.check(made.token) == control::token_check::accepted);
    const auto parts = control::parse_token(made.token);
    REQUIRE(parts.has_value());

    const std::string stored = bytes_of(env.file);
    REQUIRE(!stored.empty());
    REQUIRE(stored.find(made.record.id) != std::string::npos);
    REQUIRE(stored.find(hex_of(made.record.hash)) != std::string::npos);
    REQUIRE(stored.find(std::string(parts->secret.expose())) ==
            std::string::npos);
    REQUIRE(stored.find(std::string(made.token.expose())) == std::string::npos);
    REQUIRE(stored.find("vsst1_") == std::string::npos);
    // Nothing else is left beside it.
    std::size_t files = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(env.file.parent_path()))
    {
        static_cast<void>(entry);
        ++files;
    }
    REQUIRE_EQ(files, std::size_t{1});
}

TEST_CASE("control tokens: a file that cannot be trusted is not opened and "
          "not replaced")
{
    fixture env;
    const control::new_token made = env.make();
    REQUIRE(env.store->revoke(made.record.id));
    env.store.reset();

    const std::string good = bytes_of(env.file);
    for (const std::string& damaged :
         {std::string("{}"), std::string("not json"), std::string(),
          good.substr(0, good.size() / 2),
          // A revoked token set back to not revoked by hand is still a
          // document the store reads; what must never happen is that a
          // damaged file is taken for an empty one.
          std::string("{\"version\":2,\"latest_seen\":0,\"tokens\":[]}")})
    {
        {
            std::ofstream out(env.file, std::ios::binary | std::ios::trunc);
            out << damaged;
        }
        const auto opened =
            control::token_store::open(env.file, env.clock.source());
        REQUIRE(!opened);
        // The damaged file is still there for a person to look at.
        REQUIRE_EQ(bytes_of(env.file), damaged);
    }

    {
        std::ofstream out(env.file, std::ios::binary | std::ios::trunc);
        out << good;
    }
    env.reopen();
    REQUIRE(env.check(made.token) == control::token_check::revoked);
}

TEST_CASE("control tokens: a store needs a file in a directory")
{
    REQUIRE(!control::token_store::open({}));
    REQUIRE(!control::token_store::open("tokens.json"));
}

TEST_CASE("control tokens: the names of the checks, for the audit log")
{
    using control::token_check;
    std::set<std::string_view> names;
    for (const token_check value :
         {token_check::accepted, token_check::malformed, token_check::unknown,
          token_check::wrong_secret, token_check::revoked, token_check::expired,
          token_check::wrong_transport})
    {
        REQUIRE(names.insert(control::token_check_name(value)).second);
    }
    REQUIRE_EQ(control::token_check_name(token_check::wrong_secret),
               std::string_view("wrong_secret"));
}
