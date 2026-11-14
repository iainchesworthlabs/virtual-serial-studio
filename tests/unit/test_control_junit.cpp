//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The JUnit report of a run: its shape, its counts, its times, and above all
// that no name or message a client supplies can change the document's
// structure.
//

#include <vss/control/junit.hpp>

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace control = vss::control;

namespace
{

std::size_t count_of(std::string_view text, std::string_view piece)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(piece); at != std::string_view::npos;
         at = text.find(piece, at + piece.size()))
        ++count;
    return count;
}

} // namespace

TEST_CASE("control junit: a run is one suite with a case for each check")
{
    control::junit_suite suite;
    suite.name = "smoke";
    suite.started_ms = 1794562200123ull;
    suite.duration_ms = 61250;
    suite.cases.push_back({"banner", control::case_outcome::passed, "", 12});
    suite.cases.push_back({"reply", control::case_outcome::failed,
                           "the bytes did not arrive within 200 ms", 200});
    suite.cases.push_back(
        {"send", control::case_outcome::error, "engine.not_found", 0});
    suite.cases.push_back(
        {"later", control::case_outcome::skipped, "no rig", 1000});

    const std::string expected =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<testsuites>\n"
        "  <testsuite name=\"smoke\" tests=\"4\" failures=\"1\" errors=\"1\" "
        "skipped=\"1\" time=\"61.250\" timestamp=\"2026-11-13T09:30:00Z\">\n"
        "    <testcase name=\"banner\" classname=\"smoke\" time=\"0.012\"/>\n"
        "    <testcase name=\"reply\" classname=\"smoke\" time=\"0.200\">\n"
        "      <failure message=\"the bytes did not arrive within 200 "
        "ms\"/>\n"
        "    </testcase>\n"
        "    <testcase name=\"send\" classname=\"smoke\" time=\"0.000\">\n"
        "      <error message=\"engine.not_found\"/>\n"
        "    </testcase>\n"
        "    <testcase name=\"later\" classname=\"smoke\" time=\"1.000\">\n"
        "      <skipped message=\"no rig\"/>\n"
        "    </testcase>\n"
        "  </testsuite>\n"
        "</testsuites>\n";
    REQUIRE_EQ(control::write_junit(suite), expected);

    control::junit_suite empty;
    empty.name = "nothing";
    const std::string none = control::write_junit(empty);
    REQUIRE(
        none.find("tests=\"0\" failures=\"0\" errors=\"0\" skipped=\"0\"") !=
        std::string::npos);
    REQUIRE(none.find("timestamp=\"1970-01-01T00:00:00Z\"") !=
            std::string::npos);
    REQUIRE_EQ(count_of(none, "<testcase"), std::size_t{0});
}

TEST_CASE("control junit: no text of a client changes the document")
{
    // Each of these tries to end the attribute it is in and write markup of
    // its own.
    const std::string attacks[] = {
        "\"/><testcase name=\"forged\"/><x a=\"",
        "'/><failure message='forged'/>",
        "</testsuite></testsuites><!-- gone -->",
        "<![CDATA[ ]]> &forged; &#x3c;",
        "]]><script>alert(1)</script>",
        "<!DOCTYPE x [<!ENTITY e SYSTEM \"file:///etc/passwd\">]>&e;"};
    for (const std::string& attack : attacks)
    {
        control::junit_suite suite;
        suite.name = attack;
        suite.cases.push_back(
            {attack, control::case_outcome::failed, attack, 1});
        const std::string xml = control::write_junit(suite);
        // The structure is exactly that of one failed case: the only angle
        // brackets and quotes in the document are the writer's own.
        REQUIRE_EQ(count_of(xml, "<"), std::size_t{8});
        REQUIRE_EQ(count_of(xml, ">"), std::size_t{8});
        REQUIRE_EQ(count_of(xml, "<testcase"), std::size_t{1});
        REQUIRE_EQ(count_of(xml, "<failure"), std::size_t{1});
        REQUIRE_EQ(count_of(xml, "<testsuite "), std::size_t{1});
        // 2 in the declaration, 7 attributes of the suite, 3 of the case
        // and 1 of the failure, each with two quotes.
        REQUIRE_EQ(count_of(xml, "\""), std::size_t{4 + 14 + 6 + 2});
        REQUIRE_EQ(count_of(xml, "'"), std::size_t{0});
        // Every ampersand begins one of the writer's own references.
        for (std::size_t at = xml.find('&'); at != std::string::npos;
             at = xml.find('&', at + 1))
        {
            const std::string_view rest = std::string_view(xml).substr(at);
            REQUIRE((rest.starts_with("&amp;") || rest.starts_with("&lt;") ||
                     rest.starts_with("&gt;") || rest.starts_with("&quot;") ||
                     rest.starts_with("&apos;") || rest.starts_with("&#9;") ||
                     rest.starts_with("&#10;") || rest.starts_with("&#13;")));
        }
    }
}

TEST_CASE("control junit: text is made safe character by character")
{
    REQUIRE_EQ(control::xml_text("plain text 123"),
               std::string("plain text 123"));
    REQUIRE_EQ(control::xml_text("a<b>c&d\"e'f"),
               std::string("a&lt;b&gt;c&amp;d&quot;e&apos;f"));
    // An entity a client wrote is text, not an entity.
    REQUIRE_EQ(control::xml_text("&lt;"), std::string("&amp;lt;"));
    // Line ends and tabs survive an attribute as references.
    REQUIRE_EQ(control::xml_text("a\tb\nc\rd"),
               std::string("a&#9;b&#10;c&#13;d"));
    // Characters XML 1.0 does not allow are left out: the other control
    // characters, and the two code points at the end of the first plane.
    using namespace std::string_view_literals;
    REQUIRE_EQ(control::xml_text("a\0b\x01\x08\x0b\x0c\x1f"sv),
               std::string("ab"));
    REQUIRE_EQ(control::xml_text("x\xef\xbf\xbe\xef\xbf\xbfy"),
               std::string("xy"));
    // Valid text in more than ASCII is kept as it is.
    REQUIRE_EQ(
        control::xml_text("d\xc3\xa9j\xc3\xa0 \xe2\x82\xac \xf0\x9f\x98\x80"),
        std::string("d\xc3\xa9j\xc3\xa0 \xe2\x82\xac \xf0\x9f\x98\x80"));
    // Bytes that are not UTF-8 become question marks, one for each: a lone
    // continuation byte, a cut sequence, an overlong slash, a surrogate.
    REQUIRE_EQ(control::xml_text("a\x80z"), std::string("a?z"));
    REQUIRE_EQ(control::xml_text("a\xe2\x82"), std::string("a??"));
    REQUIRE_EQ(control::xml_text("\xc0\xaf"), std::string("??"));
    REQUIRE_EQ(control::xml_text("\xed\xa0\x80"), std::string("???"));
    REQUIRE_EQ(control::xml_text(""), std::string());
}

TEST_CASE("control junit: times are written the way a report reads them")
{
    REQUIRE_EQ(control::iso_time(0), std::string("1970-01-01T00:00:00Z"));
    REQUIRE_EQ(control::iso_time(999), std::string("1970-01-01T00:00:00Z"));
    REQUIRE_EQ(control::iso_time(951782400000ull),
               std::string("2000-02-29T00:00:00Z"));
    REQUIRE_EQ(control::iso_time(951868799000ull),
               std::string("2000-02-29T23:59:59Z"));
    REQUIRE_EQ(control::iso_time(1794562200123ull),
               std::string("2026-11-13T09:30:00Z"));
    REQUIRE_EQ(control::iso_time(4107542400000ull),
               std::string("2100-03-01T00:00:00Z"));
    REQUIRE_EQ(control::iso_time(253402300799000ull),
               std::string("9999-12-31T23:59:59Z"));

    REQUIRE_EQ(control::outcome_name(control::case_outcome::passed),
               std::string_view("passed"));
    REQUIRE_EQ(control::outcome_name(control::case_outcome::failed),
               std::string_view("failed"));
    REQUIRE_EQ(control::outcome_name(control::case_outcome::error),
               std::string_view("error"));
    REQUIRE_EQ(control::outcome_name(control::case_outcome::skipped),
               std::string_view("skipped"));
}
