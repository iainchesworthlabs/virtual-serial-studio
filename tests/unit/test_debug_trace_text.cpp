//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The text of the support bundle's call trace: what it keeps, what it leaves
// out, and that it holds no traffic.
//

#include <vss/debug/trace_text.hpp>
#include <vss/trace/recorder.hpp>

#include "test_framework.hpp"

#include <boost/system/error_code.hpp>

#include <string>

namespace
{

bool has(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

boost::system::error_code denied()
{
    return boost::system::error_code(5, boost::system::system_category());
}

} // namespace

TEST_CASE("debug trace text: an empty recorder says nothing has been recorded")
{
    vss::trace::recorder log;
    const std::string text = vss::debug::failure_context_text(log);
    REQUIRE(has(text, "No call has failed"));
    REQUIRE(has(text, "Nothing has been recorded yet."));
}

TEST_CASE("debug trace text: with no failure the last calls are listed")
{
    vss::trace::recorder log;
    vss::trace::call_scope("CreateFileW", 1, "COM5", log).ok();
    const std::string text = vss::debug::failure_context_text(log);
    REQUIRE(has(text, "No call has failed"));
    REQUIRE(has(text, "CreateFileW (COM5)"));
}

TEST_CASE("debug trace text: a failure is listed with its code and message")
{
    vss::trace::recorder log;
    {
        vss::trace::operation_scope scope("reopen", 4);
        vss::trace::call_scope call("SetCommState", 0, "COM5", log);
        call.fail(denied());
    }
    const std::string text = vss::debug::failure_context_text(log);
    REQUIRE(!has(text, "No call has failed"));
    REQUIRE(has(text, "SetCommState (COM5)"));
    REQUIRE(has(text, "in reopen"));
    REQUIRE(has(text, "port 4"));
    REQUIRE(has(text, "FAILED system:5"));
}

TEST_CASE("debug trace text: the entry line of a cause names the entry")
{
    vss::trace::recorder log;
    vss::trace::call_scope first("CreateFileW", 0, "", log);
    const auto cause = first.fail(denied());
    vss::trace::call_scope second("studio.reopen", 0, "", log);
    second.fail(denied(), cause);
    const std::string text = vss::debug::failure_context_text(log);
    REQUIRE(has(text, "(from #" + std::to_string(cause) + ")"));
}
