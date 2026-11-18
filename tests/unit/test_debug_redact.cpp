//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the debug API removes from every string it sends.
//

#include <vss/debug/redact.hpp>

#include "test_framework.hpp"

#include <string>

namespace
{

vss::debug::redactor usual()
{
    return vss::debug::redactor::for_user("C:\\Users\\iain", "iain",
                                          "STUDIO-PC");
}

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("debug redact: the home folder, user and computer names go")
{
    const auto redact = usual();
    const std::string out = redact.apply(
        "opened C:\\Users\\iain\\Documents\\setup.json on STUDIO-PC as iain");
    REQUIRE(!has(out, "iain"));
    REQUIRE(!has(out, "STUDIO-PC"));
    REQUIRE(has(out, "<home>"));
    REQUIRE(has(out, "<computer>"));
}

TEST_CASE("debug redact: a path matches whichever way its slashes lean")
{
    const auto redact = usual();
    const std::string out = redact.apply("C:/Users/iain/AppData");
    REQUIRE(!has(out, "iain"));
    REQUIRE(has(out, "<home>"));
}

TEST_CASE("debug redact: case does not hide a name")
{
    const auto redact = usual();
    REQUIRE(!has(redact.apply("c:\\users\\IAIN\\x"), "IAIN"));
    REQUIRE(!has(redact.apply("studio-pc"), "studio-pc"));
}

TEST_CASE("debug redact: a literal shorter than three characters is ignored")
{
    vss::debug::redactor redact;
    redact.add_literal("ab", "<x>");
    REQUIRE_EQ(redact.apply("a ab b"), std::string("a ab b"));
}

TEST_CASE("debug redact: a URL with a password loses the password")
{
    const auto redact = usual();
    const std::string out =
        redact.apply("rfc2217://admin:hunter2@192.0.2.10:4000");
    REQUIRE(!has(out, "hunter2"));
    REQUIRE(!has(out, "admin"));
    REQUIRE(has(out, "192.0.2.10:4000"));
}

TEST_CASE("debug redact: named secrets and bearer tokens go")
{
    const auto redact = usual();
    for (const char* text :
         {"password=swordfish", "Token: abc123def", "secret = s3cr3t",
          "api_key=KEY123", "apikey:KEY123",
          "Authorization: Bearer abc.def-ghi"})
    {
        const std::string out = redact.apply(text);
        REQUIRE(has(out, "<redacted>"));
        REQUIRE(!has(out, "swordfish"));
        REQUIRE(!has(out, "abc123def"));
        REQUIRE(!has(out, "s3cr3t"));
        REQUIRE(!has(out, "KEY123"));
        REQUIRE(!has(out, "abc.def-ghi"));
    }
}

TEST_CASE("debug redact: a device serial number goes")
{
    const auto redact = usual();
    const std::string out =
        redact.apply("USB\\VID_0403&PID_6001 serial=A50285BI");
    REQUIRE(!has(out, "A50285BI"));
}

TEST_CASE("debug redact: ordinary text and COM names are left alone")
{
    const auto redact = usual();
    const std::string text =
        "CreateFileW \\\\.\\COM5 failed: Access is denied (5)";
    REQUIRE_EQ(redact.apply(text), text);
}
