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

TEST_CASE("debug redact: the scanners handle empty and one-character text")
{
    const auto redact = usual();
    REQUIRE_EQ(redact.apply(""), std::string());
    REQUIRE_EQ(redact.apply("a"), std::string("a"));
    REQUIRE_EQ(redact.apply(":"), std::string(":"));
}

TEST_CASE("debug redact: a name without a value is left alone")
{
    const auto redact = usual();
    for (const char* text :
         {"password", "password=", "password= ", "token:", "bearer", "bearer ",
          "Bearer =", "serial=", "serial", "api_", "api_key", "://", "http://",
          "http://:@", "http://user@host"})
    {
        REQUIRE_EQ(redact.apply(text), std::string(text));
    }
}

TEST_CASE("debug redact: two matches next to each other both go")
{
    const auto redact = usual();
    const std::string out =
        redact.apply("password=one;token=two secret=three&api-key=four");
    REQUIRE_EQ(out, std::string("password=<redacted>;token=<redacted> "
                                "secret=<redacted>&api-key=<redacted>"));

    const std::string urls = redact.apply("a://u:p@h b://v:q@i c://w:r@j");
    REQUIRE_EQ(urls, std::string("a://<redacted>@h b://<redacted>@i "
                                 "c://<redacted>@j"));
}

TEST_CASE("debug redact: a match at the very end of the text goes")
{
    const auto redact = usual();
    REQUIRE_EQ(redact.apply("x password=y"),
               std::string("x password=<redacted>"));
    REQUIRE_EQ(redact.apply("Authorization: Bearer abc=="),
               std::string("Authorization: Bearer <redacted>"));
    REQUIRE_EQ(redact.apply("serial=Z9"), std::string("serial=<redacted>"));
    REQUIRE_EQ(redact.apply("rfc2217://u:p@"),
               std::string("rfc2217://<redacted>@"));
}

TEST_CASE("debug redact: a value ends where the secret characters end")
{
    const auto redact = usual();
    REQUIRE_EQ(redact.apply("password=a,b"),
               std::string("password=<redacted>,b"));
    REQUIRE_EQ(redact.apply("token=\"abc\""), std::string("token=\"abc\""));
    REQUIRE_EQ(redact.apply("Bearer a.b/c~d+e-f=="),
               std::string("Bearer <redacted>"));
}

TEST_CASE("debug redact: a serial number needs a word boundary")
{
    const auto redact = usual();
    REQUIRE_EQ(redact.apply("observer=X1"), std::string("observer=X1"));
    REQUIRE_EQ(redact.apply("SerialNumber: A1"),
               std::string("SerialNumber: <redacted>"));
    REQUIRE_EQ(redact.apply("(ser=A1)"), std::string("(ser=<redacted>)"));
}

TEST_CASE("debug redact: a URL scheme is read from its first letter")
{
    const auto redact = usual();
    REQUIRE_EQ(redact.apply("1http://u:p@h"),
               std::string("1http://<redacted>@h"));
    REQUIRE_EQ(redact.apply("123://u:p@h"), std::string("123://u:p@h"));
    // A failed `://` does not hide a later one.
    REQUIRE_EQ(redact.apply("a://b c://u:p@h"),
               std::string("a://b c://<redacted>@h"));
}

TEST_CASE("debug redact: very long text is handled in a straight pass")
{
    const auto redact = usual();
    const std::string filler(1000000, 'a');
    REQUIRE_EQ(redact.apply(filler), filler);

    const std::string spaces(1000000, ' ');
    REQUIRE_EQ(redact.apply("password" + spaces), "password" + spaces);

    std::string repeated;
    for (int index = 0; index < 20000; ++index)
        repeated += "token=abc ";
    const std::string out = redact.apply(repeated);
    REQUIRE(!has(out, "abc"));
    REQUIRE_EQ(out.size(), std::size_t{20000} * 17);

    // Many scheme-like runs that never reach a `://`, and many that do not
    // carry a password.
    std::string schemes;
    for (int index = 0; index < 20000; ++index)
        schemes += "ab:// ";
    REQUIRE_EQ(redact.apply(schemes), schemes);

    const std::string password(1000000, 'p');
    const std::string url = "h://u:" + password + "@x";
    REQUIRE_EQ(redact.apply(url), std::string("h://<redacted>@x"));
}
