//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The call recorder: what an entry keeps, the two rings, the filters, the
// operation scopes and the cause of an error, and that nothing but the text
// the caller gave is held.
//

#include <vss/trace/recorder.hpp>

#include "test_framework.hpp"

#include <boost/system/error_code.hpp>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace
{

boost::system::error_code win32_denied()
{
    return boost::system::error_code(5, boost::system::system_category());
}

} // namespace

TEST_CASE("trace recorder: an entry keeps the call, its arguments and its time")
{
    vss::trace::recorder log;
    vss::trace::call_scope scope("CreateFileW", 7, "COM5 baud=9600", log);
    scope.ok();

    const auto found = log.read();
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].seq, std::uint64_t{1});
    REQUIRE_EQ(std::string(found[0].call_text()), std::string("CreateFileW"));
    REQUIRE_EQ(std::string(found[0].args_text()),
               std::string("COM5 baud=9600"));
    REQUIRE_EQ(found[0].endpoint, std::uint64_t{7});
    REQUIRE(!found[0].failed);
    REQUIRE(found[0].wall_ms > 0);
    REQUIRE(found[0].mono_ns > 0);
}

TEST_CASE("trace recorder: a failure keeps the raw code, its category and text")
{
    vss::trace::recorder log;
    vss::trace::call_scope scope("SetCommState", 3, "COM5", log);
    const auto seq = scope.fail(win32_denied());

    REQUIRE_EQ(seq, std::uint64_t{1});
    const auto failure = log.last_failure(3);
    REQUIRE(failure.has_value());
    REQUIRE(failure->failed);
    REQUIRE_EQ(failure->code, 5);
    REQUIRE_EQ(std::string(failure->category_text()), std::string("system"));
    REQUIRE(!failure->message_text().empty());
    REQUIRE(!log.last_failure(99).has_value());
    REQUIRE(log.last_failure().has_value());
}

TEST_CASE("trace recorder: a scope that is not finished is kept as a success")
{
    vss::trace::recorder log;
    {
        vss::trace::call_scope scope("CloseHandle", 1, "", log);
    }
    const auto found = log.read();
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE(!found[0].failed);
}

TEST_CASE("trace recorder: text that does not fit is cut and never overruns")
{
    vss::trace::recorder log;
    const std::string long_name(500, 'x');
    vss::trace::call_scope scope(long_name, 0, long_name, log);
    scope.ok();
    const auto found = log.read();
    REQUIRE_EQ(found.size(), std::size_t{1});
    REQUIRE_EQ(found[0].call_text().size(), vss::trace::entry::call_size_ - 1);
    REQUIRE_EQ(found[0].args_text().size(), vss::trace::entry::args_size_ - 1);
}

TEST_CASE("trace recorder: a failure outlives the calls that push it out")
{
    vss::trace::recorder log(32, 8);
    {
        vss::trace::call_scope scope("CreateFileW", 4, "COM5", log);
        scope.fail(win32_denied());
    }
    for (int index = 0; index < 100; ++index)
        vss::trace::call_scope("ReadFile", 4, "", log).ok();

    const auto everything = log.read();
    // The busy ring has long forgotten the failure; the read still has it,
    // before the calls that followed it.
    REQUIRE(!everything.empty());
    REQUIRE(everything.front().failed);
    REQUIRE_EQ(std::string(everything.front().call_text()),
               std::string("CreateFileW"));
    REQUIRE(log.overwritten() > 0);

    vss::trace::filter failures;
    failures.failures_only = true;
    REQUIRE_EQ(log.read(failures).size(), std::size_t{1});
}

TEST_CASE("trace recorder: filters by number, port, failure and count")
{
    vss::trace::recorder log;
    vss::trace::call_scope("a", 1, "", log).ok();
    {
        vss::trace::call_scope scope("b", 2, "", log);
        scope.fail(win32_denied());
    }
    vss::trace::call_scope("c", 2, "", log).ok();

    vss::trace::filter after;
    after.after_seq = 1;
    REQUIRE_EQ(log.read(after).size(), std::size_t{2});

    vss::trace::filter port;
    port.endpoint = 2;
    REQUIRE_EQ(log.read(port).size(), std::size_t{2});

    vss::trace::filter newest;
    newest.limit = 1;
    const auto last = log.read(newest);
    REQUIRE_EQ(last.size(), std::size_t{1});
    REQUIRE_EQ(std::string(last[0].call_text()), std::string("c"));
    REQUIRE_EQ(log.last_seq(), std::uint64_t{3});
}

TEST_CASE("trace recorder: an operation scope names the calls made inside it")
{
    vss::trace::recorder log;
    {
        vss::trace::operation_scope outer("restart_port", 9);
        vss::trace::call_scope("SetCommState", 0, "", log).ok();
        {
            vss::trace::operation_scope inner("open", 0);
            vss::trace::call_scope("CreateFileW", 0, "", log).ok();
        }
        vss::trace::call_scope("CloseHandle", 0, "", log).ok();
    }
    vss::trace::call_scope("after", 0, "", log).ok();

    const auto found = log.read();
    REQUIRE_EQ(found.size(), std::size_t{4});
    REQUIRE_EQ(std::string(found[0].operation_text()),
               std::string("restart_port"));
    REQUIRE_EQ(found[0].endpoint, std::uint64_t{9});
    REQUIRE_EQ(std::string(found[1].operation_text()), std::string("open"));
    // The inner scope named no port, so the calls have none.
    REQUIRE_EQ(found[1].endpoint, std::uint64_t{0});
    REQUIRE_EQ(std::string(found[2].operation_text()),
               std::string("restart_port"));
    REQUIRE_EQ(found[2].endpoint, std::uint64_t{9});
    REQUIRE(found[3].operation_text().empty());
    REQUIRE_EQ(found[3].endpoint, std::uint64_t{0});
}

TEST_CASE("trace recorder: a failed operation names the call it came from")
{
    auto& global = vss::trace::recorder::global();
    const auto before = global.last_seq();

    const auto failed = vss::trace::traced("studio.reopen", 12,
                                           []() -> boost::system::result<void>
                                           {
                                               vss::trace::call_scope scope(
                                                   "CreateFileW", 0, "COM5");
                                               scope.fail(win32_denied());
                                               return win32_denied();
                                           });
    REQUIRE(!failed);

    vss::trace::filter what;
    what.after_seq = before;
    const auto found = global.read(what);
    REQUIRE_EQ(found.size(), std::size_t{2});
    REQUIRE_EQ(std::string(found[0].call_text()), std::string("CreateFileW"));
    REQUIRE_EQ(found[0].endpoint, std::uint64_t{12});
    REQUIRE_EQ(std::string(found[1].call_text()), std::string("studio.reopen"));
    REQUIRE_EQ(found[1].cause, found[0].seq);
    REQUIRE(found[1].failed);
}

TEST_CASE("trace recorder: a success of an operation records nothing extra")
{
    auto& global = vss::trace::recorder::global();
    const auto before = global.last_seq();
    const auto done =
        vss::trace::traced("studio.start_port", 1,
                           []() -> boost::system::result<void> { return {}; });
    REQUIRE(done.has_value());
    REQUIRE_EQ(global.last_seq(), before);
}

TEST_CASE("trace recorder: several threads record without losing a number")
{
    vss::trace::recorder log(8192, 64);
    constexpr int threads = 4;
    constexpr int each = 500;
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (int index = 0; index < threads; ++index)
    {
        workers.emplace_back(
            [&log]
            {
                for (int count = 0; count < each; ++count)
                    vss::trace::call_scope("w", 0, "", log).ok();
            });
    }
    for (auto& worker : workers)
        worker.join();

    const auto found = log.read();
    REQUIRE_EQ(found.size(), static_cast<std::size_t>(threads * each));
    for (std::size_t index = 1; index < found.size(); ++index)
        REQUIRE_EQ(found[index].seq, found[index - 1].seq + 1);
}

TEST_CASE("trace recorder: clear empties the rings and the numbers go on")
{
    vss::trace::recorder log;
    vss::trace::call_scope("a", 0, "", log).ok();
    log.clear();
    REQUIRE(log.read().empty());
    vss::trace::call_scope("b", 0, "", log).ok();
    REQUIRE_EQ(log.read()[0].seq, std::uint64_t{2});
}
