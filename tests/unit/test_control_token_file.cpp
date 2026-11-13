//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The token store's file: what is written, that it reads back equal, and that
// every document which is not exactly what `save_token_file` writes is refused
// with `errc::config_error`.  A bad document is made by saving a good file
// (so that the directory and the file are private) and then changing its
// content with an ordinary stream.
//

#include "token_file.hpp"

#include <vss/control/proto/policy.hpp>
#include <vss/control/proto/token.hpp>
#include <vss/control/token_store.hpp>
#include <vss/core/error.hpp>

#include "scratch_directory.hpp"
#include "test_framework.hpp"

#include <boost/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace control = vss::control;
namespace json = boost::json;

namespace
{

constexpr std::string_view id_alphabet = "abcdefghijklmnopqrstuvwxyz234567";

control::wall_clock::time_point at(std::int64_t millis)
{
    return control::wall_clock::time_point{} +
           std::chrono::milliseconds(millis);
}

control::token_digest digest_of(unsigned seed)
{
    control::token_digest digest{};
    for (std::size_t position = 0; position < digest.size(); ++position)
        digest[position] =
            static_cast<std::uint8_t>((seed * 31 + position * 7) & 0xffu);
    return digest;
}

std::string hex_of(const control::token_digest& digest)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : digest)
    {
        out.push_back(digits[static_cast<std::size_t>(byte >> 4)]);
        out.push_back(digits[static_cast<std::size_t>(byte & 0x0fu)]);
    }
    return out;
}

/// Three records that differ in every field: a revoked one, one that was used,
/// different scopes and transports.
control::token_file_content three_records()
{
    control::token_record first;
    first.id = "abcdefghijklm";
    first.label = "Lab rig 3";
    first.scopes = {control::scope::monitor, control::scope::monitor_data};
    first.transports = {control::transport::local};
    first.created = at(1794562200123);
    first.expires = at(1797154200123);
    first.hash = digest_of(1);

    control::token_record second;
    second.id = "nopqrstuvwxyz";
    second.label = "";
    second.scopes = control::role_control();
    second.transports = {control::transport::local,
                         control::transport::loopback, control::transport::tls};
    second.created = at(1794562300000);
    second.expires = at(1795000000000);
    second.last_used = at(1794600000456);
    second.hash = digest_of(2);

    control::token_record third;
    third.id = "2233445566abc";
    third.label = "Gone \"quoted\" \\ label";
    third.scopes = {control::scope::control_apply, control::scope::record};
    third.transports = {control::transport::local_admin};
    third.created = at(1794562400999);
    third.expires = at(1794900000001);
    third.revoked = true;
    third.hash = digest_of(3);

    control::token_file_content content;
    content.tokens = {first, second, third};
    content.latest_seen = at(1794600000999);
    return content;
}

std::string read_file(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string text_of(const json::value& item)
{
    REQUIRE(item.is_string());
    return std::string(item.get_string().data(), item.get_string().size());
}

/// Replaces the content of a file that exists.
void overwrite_file(const std::filesystem::path& file, const std::string& bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(out));
}

void save_good(const std::filesystem::path& file,
               const control::token_file_content& content)
{
    const vss::error_code saved = control::save_token_file(file, content);
    REQUIRE(!saved);
}

vss::error_code config_error()
{
    return vss::make_error_code(vss::errc::config_error);
}

void require_refused(const std::filesystem::path& file)
{
    const auto loaded = control::load_token_file(file);
    REQUIRE(!loaded);
    REQUIRE_EQ(loaded.error(), config_error());
}

/// Saves the three records, changes the document with `change` and expects the
/// file to be refused.
using edit = std::function<void(json::object&)>;

void require_refused_after(const std::filesystem::path& file,
                           const edit& change)
{
    save_good(file, three_records());
    json::value document = json::parse(read_file(file));
    REQUIRE(document.is_object());
    change(document.as_object());
    overwrite_file(file, json::serialize(document));
    require_refused(file);
}

/// The same for a change to the first record.
void require_refused_after_record(const std::filesystem::path& file,
                                  const edit& change)
{
    require_refused_after(
        file, [&](json::object& root)
        { change(root["tokens"].as_array()[0].as_object()); });
}

/// Saves the three records, changes the document with `change` and expects the
/// file to load back as `expected`.
void require_loaded_after(const std::filesystem::path& file, const edit& change,
                          const control::token_file_content& expected)
{
    save_good(file, three_records());
    json::value document = json::parse(read_file(file));
    REQUIRE(document.is_object());
    change(document.as_object());
    overwrite_file(file, json::serialize(document));
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE(*loaded == expected);
}

/// A 13 character id that is valid and different for each number below 1024.
std::string id_number(std::size_t number)
{
    std::string id(11, 'a');
    id.push_back(id_alphabet[(number / 32) % 32]);
    id.push_back(id_alphabet[number % 32]);
    return id;
}

control::token_file_content many_records(std::size_t count)
{
    control::token_file_content content = three_records();
    const control::token_record model = content.tokens.front();
    content.tokens.clear();
    for (std::size_t number = 0; number < count; ++number)
    {
        control::token_record record = model;
        record.id = id_number(number);
        content.tokens.push_back(record);
    }
    return content;
}

} // namespace

TEST_CASE("control token file: three records save and load back equal")
{
    const vss_test_support::scratch_directory scratch;
    // The directory does not exist yet: saving makes it.
    const auto file = scratch.path() / "state" / "tokens.json";
    const control::token_file_content content = three_records();

    save_good(file, content);
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE_EQ(loaded->tokens.size(), std::size_t{3});
    REQUIRE(loaded->latest_seen == content.latest_seen);
    REQUIRE(*loaded == content);

    // The fields that the comparison could hide behind a default.
    REQUIRE(!loaded->tokens[0].last_used.has_value());
    REQUIRE(loaded->tokens[1].last_used.has_value());
    REQUIRE(*loaded->tokens[1].last_used == at(1794600000456));
    REQUIRE(!loaded->tokens[1].revoked);
    REQUIRE(loaded->tokens[2].revoked);
    REQUIRE_EQ(loaded->tokens[2].label,
               std::string("Gone \"quoted\" \\ label"));
}

TEST_CASE("control token file: times below a millisecond are cut off")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";
    control::token_file_content content = three_records();
    content.latest_seen = at(1794600000999) + std::chrono::microseconds(789);

    save_good(file, content);
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE(loaded->latest_seen == at(1794600000999));
}

TEST_CASE("control token file: a file that is missing is an empty store")
{
    const vss_test_support::scratch_directory scratch;

    // Neither the file nor its directory exists.
    const auto loaded =
        control::load_token_file(scratch.path() / "state" / "tokens.json");
    REQUIRE(loaded);
    REQUIRE(loaded->tokens.empty());
    REQUIRE(loaded->latest_seen == control::wall_clock::time_point{});
    REQUIRE(*loaded == control::token_file_content{});
}

TEST_CASE("control token file: an empty store saves and loads back")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";
    control::token_file_content content;
    content.latest_seen = at(1794600000000);

    save_good(file, content);
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE(*loaded == content);
}

TEST_CASE("control token file: saving twice leaves one file and no temp file")
{
    const vss_test_support::scratch_directory scratch;
    const auto directory = scratch.path() / "state";
    const auto file = directory / "tokens.json";

    control::token_file_content first = three_records();
    save_good(file, first);
    control::token_file_content second = three_records();
    second.tokens.pop_back();
    second.latest_seen = at(1794700000000);
    save_good(file, second);

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        ++files;
        REQUIRE_EQ(entry.path().filename().string(), "tokens.json");
    }
    REQUIRE_EQ(files, std::size_t{1});
    REQUIRE(!std::filesystem::exists(directory / "tokens.json.tmp"));

    // The second document replaced the first.
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE(*loaded == second);
}

TEST_CASE("control token file: the text holds the hash as hex and is JSON")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";
    const control::token_file_content content = three_records();
    save_good(file, content);

    const std::string text = read_file(file);
    for (const control::token_record& record : content.tokens)
    {
        REQUIRE(text.find(hex_of(record.hash)) != std::string::npos);
        REQUIRE(text.find(record.id) != std::string::npos);
    }

    const json::value document = json::parse(text);
    REQUIRE(document.is_object());
    const json::object& root = document.as_object();
    REQUIRE_EQ(root.at("version").as_int64(), std::int64_t{1});
    REQUIRE_EQ(root.at("latest_seen").as_int64(), std::int64_t{1794600000999});
    const json::array& records = root.at("tokens").as_array();
    REQUIRE_EQ(records.size(), std::size_t{3});

    const json::object& first = records[0].as_object();
    REQUIRE_EQ(text_of(first.at("id")), "abcdefghijklm");
    REQUIRE_EQ(text_of(first.at("label")), "Lab rig 3");
    REQUIRE(first.at("last_used").is_null());
    REQUIRE(first.at("revoked").is_bool());
    REQUIRE(!first.at("revoked").as_bool());
    REQUIRE_EQ(first.at("created").as_int64(), std::int64_t{1794562200123});
    REQUIRE_EQ(first.at("expires").as_int64(), std::int64_t{1797154200123});
    REQUIRE_EQ(text_of(first.at("hash")), hex_of(content.tokens[0].hash));

    const json::array& scopes = first.at("scopes").as_array();
    REQUIRE_EQ(scopes.size(), std::size_t{2});
    REQUIRE_EQ(text_of(scopes[0]), "monitor");
    REQUIRE_EQ(text_of(scopes[1]), "monitor.data");
    const json::array& transports = first.at("transports").as_array();
    REQUIRE_EQ(transports.size(), std::size_t{1});
    REQUIRE_EQ(text_of(transports[0]), "local");

    REQUIRE_EQ(records[1].as_object().at("last_used").as_int64(),
               std::int64_t{1794600000456});
    REQUIRE(records[2].as_object().at("revoked").as_bool());
    REQUIRE_EQ(text_of(records[2].as_object().at("transports").as_array()[0]),
               "local-admin");
}

TEST_CASE("control token file: a document that is not JSON is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";
    save_good(file, three_records());

    const std::string cases[] = {
        "this is not json", "{\"version\": 1,", "[]", "null", "42", "\"text\"",
        // Text after the document.
        read_file(file) + " x",
        // Nested deeper than a token file ever is.
        "{\"version\":1,\"latest_seen\":0,\"tokens\":[],"
        "\"extra\":[[[[[[[[[[1]]]]]]]]]]}"};
    for (const std::string& text : cases)
    {
        overwrite_file(file, text);
        require_refused(file);
    }
}

TEST_CASE("control token file: an empty file is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";
    save_good(file, three_records());

    overwrite_file(file, "");
    require_refused(file);
}

TEST_CASE("control token file: another version or no version is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    require_refused_after(file, [](json::object& root)
                          { root["version"] = json::value(2); });
    require_refused_after(file, [](json::object& root)
                          { root["version"] = json::value(0); });
    require_refused_after(file, [](json::object& root)
                          { root["version"] = json::value("1"); });
    require_refused_after(file, [](json::object& root)
                          { root["version"] = json::value(1.5); });
    require_refused_after(file,
                          [](json::object& root) { root.erase("version"); });
}

TEST_CASE("control token file: a bad top level is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    require_refused_after(file, [](json::object& root)
                          { root.erase("latest_seen"); });
    require_refused_after(file, [](json::object& root)
                          { root["latest_seen"] = json::value(-1); });
    require_refused_after(file, [](json::object& root)
                          { root["latest_seen"] = json::value("soon"); });
    require_refused_after(file, [](json::object& root)
                          { root["latest_seen"] = json::value(1.5); });
    require_refused_after(file,
                          [](json::object& root) { root.erase("tokens"); });
    require_refused_after(file, [](json::object& root)
                          { root["tokens"] = json::value(json::object()); });
    require_refused_after(file, [](json::object& root)
                          { root["tokens"] = json::value(7); });
    require_refused_after(
        file, [](json::object& root)
        { root["tokens"].as_array().push_back(json::value(7)); });
}

TEST_CASE("control token file: a record with a field missing is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    const char* const keys[] = {"id",         "label",   "scopes",
                                "transports", "created", "expires",
                                "last_used",  "revoked", "hash"};
    for (const char* const key : keys)
    {
        require_refused_after_record(file, [key](json::object& record)
                                     { record.erase(key); });
    }
}

TEST_CASE("control token file: a field of the wrong type is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    const char* const keys[] = {"id",         "label",   "scopes",
                                "transports", "created", "expires",
                                "last_used",  "revoked", "hash"};
    for (const char* const key : keys)
    {
        // An object is the wrong type for every field.
        require_refused_after_record(
            file, [key](json::object& record)
            { record[key] = json::value(json::object()); });
    }
    require_refused_after_record(file, [](json::object& record)
                                 { record["id"] = json::value(7); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["label"] = json::value(7); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["created"] = json::value("1"); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["created"] = json::value(1.5); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["expires"] = json::value(-5); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["last_used"] = json::value("now"); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["revoked"] = json::value("false"); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["revoked"] = json::value(0); });
    require_refused_after_record(
        file, [](json::object& record)
        { record["scopes"] = json::value("monitor"); });
    require_refused_after_record(
        file, [](json::object& record)
        { record["transports"] = json::value("local"); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["hash"] = json::value(7); });
}

TEST_CASE("control token file: an id of the wrong form is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    const char* const ids[] = {
        "", "abc",
        // One character too many.
        "abcdefghijklmn",
        // Upper case, and characters outside the alphabet (0, 1, 8, 9, -).
        "ABCDEFGHIJKLM", "abcdefghijkl0", "abcdefghijkl1", "abcdefghijkl8",
        "abcdefghijkl-"};
    for (const char* const id : ids)
    {
        require_refused_after_record(file, [id](json::object& record)
                                     { record["id"] = json::value(id); });
    }
}

TEST_CASE("control token file: a hash that is not 64 lower-case hex is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";
    const std::string good = hex_of(three_records().tokens[0].hash);
    REQUIRE_EQ(good.size(), std::size_t{64});

    std::string upper = good;
    for (char& letter : upper)
    {
        if (letter >= 'a' && letter <= 'f')
            letter = static_cast<char>(letter - 'a' + 'A');
    }
    // The digits happen to include a letter, or this case would test nothing.
    REQUIRE_NE(upper, good);

    std::string odd_letter = good;
    odd_letter[10] = 'g';

    const std::string hashes[] = {"",         good.substr(0, 63),
                                  good + "0", upper,
                                  odd_letter, "0x" + good.substr(2)};
    for (const std::string& hash : hashes)
    {
        require_refused_after_record(
            file,
            [&hash](json::object& record)
            {
                record["hash"] =
                    json::value(json::string(hash.data(), hash.size()));
            });
    }
}

TEST_CASE("control token file: scopes and transports must be known and kept")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    const auto list_of = [](std::initializer_list<const char*> names)
    {
        json::array list;
        for (const char* const name : names)
            list.push_back(json::value(name));
        return list;
    };

    // Empty, unknown, wrong case, repeated, not text.
    require_refused_after_record(file, [](json::object& record)
                                 { record["scopes"] = json::array(); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["transports"] = json::array(); });
    require_refused_after_record(
        file, [&](json::object& record)
        { record["scopes"] = list_of({"monitor", "teleport"}); });
    require_refused_after_record(file, [&](json::object& record)
                                 { record["scopes"] = list_of({"Monitor"}); });
    require_refused_after_record(
        file, [&](json::object& record)
        { record["scopes"] = list_of({"monitor", "monitor"}); });
    require_refused_after_record(
        file, [&](json::object& record)
        { record["transports"] = list_of({"carrier-pigeon"}); });
    require_refused_after_record(
        file, [&](json::object& record)
        { record["transports"] = list_of({"Local"}); });
    require_refused_after_record(
        file, [&](json::object& record)
        { record["transports"] = list_of({"local", "local"}); });
    require_refused_after_record(file, [](json::object& record)
                                 { record["scopes"] = json::array{7}; });
    require_refused_after_record(file, [](json::object& record)
                                 { record["transports"] = json::array{7}; });
}

TEST_CASE("control token file: a token may never hold the admin scope")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    require_refused_after_record(file,
                                 [](json::object& record)
                                 {
                                     record["scopes"] =
                                         json::array{json::value("monitor"),
                                                     json::value("admin")};
                                 });
    require_refused_after_record(
        file, [](json::object& record)
        { record["scopes"] = json::array{json::value("admin")}; });
}

TEST_CASE("control token file: two records with one id are refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    require_refused_after(file,
                          [](json::object& root)
                          {
                              json::array& records = root["tokens"].as_array();
                              records[1].as_object()["id"] =
                                  records[0].as_object()["id"];
                          });
}

TEST_CASE("control token file: the most tokens is 256 and one more is refused")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    const control::token_file_content full = many_records(control::max_tokens);
    REQUIRE_EQ(full.tokens.size(), control::max_tokens);
    save_good(file, full);
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE(*loaded == full);

    // Written by hand: a store never saves more than it may hold.
    const control::token_file_content too_many =
        many_records(control::max_tokens + 1);
    save_good(file, too_many);
    require_refused(file);
}

TEST_CASE("control token file: a label may be 128 bytes and not more")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    control::token_file_content content = three_records();
    content.tokens[0].label = std::string(control::max_token_label_size, 'x');
    save_good(file, content);
    const auto loaded = control::load_token_file(file);
    REQUIRE(loaded);
    REQUIRE(*loaded == content);

    content.tokens[0].label =
        std::string(control::max_token_label_size + 1, 'x');
    save_good(file, content);
    require_refused(file);
}

TEST_CASE("control token file: a key that is not known is let through")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    require_loaded_after(
        file,
        [](json::object& root)
        {
            root["comment"] = json::value("added by a later version");
            root["tokens"].as_array()[1].as_object()["colour"] =
                json::value("green");
        },
        three_records());
}

TEST_CASE("control token file: a time before the epoch is not saved")
{
    const vss_test_support::scratch_directory scratch;
    const auto file = scratch.path() / "state" / "tokens.json";

    control::token_file_content content = three_records();
    content.tokens[1].expires = at(-1000);
    const vss::error_code saved = control::save_token_file(file, content);
    REQUIRE(saved);
    REQUIRE_EQ(saved, vss::make_error_code(vss::errc::invalid_argument));
    // Nothing was written.
    REQUIRE(!std::filesystem::exists(file));
}
