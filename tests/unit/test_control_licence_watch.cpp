//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The watch that keeps the licence for a gateway's `status`: one thread
// reads, and whoever asks is given the last answer without a reading.
//

#include <vss/control/licence_watch.hpp>
#include <vss/license/license.hpp>

#include "licence_test_files.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <thread>

namespace control = vss::control;
namespace license = vss::license;

namespace
{

using namespace std::chrono_literals;

/// Waits up to ten seconds for `done` to say yes.
bool before_long(const std::function<bool()>& done)
{
    const auto give_up = std::chrono::steady_clock::now() + 10s;
    while (!done())
    {
        if (std::chrono::steady_clock::now() >= give_up)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

license::status with_state(license::state state)
{
    license::status value;
    value.state = state;
    return value;
}

} // namespace

TEST_CASE("control licence watch: asking never reads, and the first reading "
          "is done before anyone asks")
{
    std::atomic<int> readings{0};
    const control::licence_watch watch(
        [&readings]
        {
            ++readings;
            return with_state(license::state::expiring);
        },
        std::chrono::hours(1));
    REQUIRE_EQ(readings.load(), 1);
    for (int asked = 0; asked < 1000; ++asked)
        REQUIRE(watch.latest().state == license::state::expiring);
    REQUIRE_EQ(readings.load(), 1);
    REQUIRE_EQ(watch.failures(), std::uint64_t{0});
}

TEST_CASE("control licence watch: a later reading replaces the answer")
{
    std::atomic<bool> ended{false};
    const control::licence_watch watch(
        [&ended]
        {
            return with_state(ended ? license::state::expired
                                    : license::state::valid);
        },
        10ms);
    REQUIRE(watch.latest().state == license::state::valid);
    ended = true;
    REQUIRE(before_long(
        [&watch] { return watch.latest().state == license::state::expired; }));
}

TEST_CASE("control licence watch: a reading that throws leaves the last "
          "answer, and the watch reads on")
{
    std::atomic<int> readings{0};
    const control::licence_watch watch(
        [&readings]
        {
            const int number = ++readings;
            if (number == 2 || number == 3)
                throw std::runtime_error("the folder cannot be read");
            return with_state(number == 1 ? license::state::grace
                                          : license::state::valid);
        },
        10ms);
    REQUIRE(watch.latest().state == license::state::grace);
    REQUIRE(before_long(
        [&watch] { return watch.latest().state == license::state::valid; }));
    REQUIRE_EQ(watch.failures(), std::uint64_t{2});

    // A first reading that throws is no licence, and not the end of the
    // program.
    const control::licence_watch broken(
        []() -> license::status { throw std::runtime_error("no"); },
        std::chrono::hours(1));
    REQUIRE(broken.latest().state == license::state::none);
    REQUIRE_EQ(broken.failures(), std::uint64_t{1});
}

TEST_CASE("control licence watch: it stops at once, however long it would "
          "wait for the next reading")
{
    const auto started = std::chrono::steady_clock::now();
    {
        const control::licence_watch watch(
            [] { return license::no_licence(); }, std::chrono::hours(24));
    }
    REQUIRE(std::chrono::steady_clock::now() - started < 5s);
}

TEST_CASE("control licence watch: over licence folders it finds the licence "
          "in use and writes nothing")
{
    namespace files = vss_test_support::licence_files;
    files::fixture folders;

    // No folder at all is the Community edition, and none is made.
    {
        const auto watch =
            control::watch_licence_folders(folders.options(), 1h);
        REQUIRE(watch->latest().state == license::state::none);
        REQUIRE(!std::filesystem::exists(folders.user()));
        REQUIRE(!std::filesystem::exists(folders.machine()));
    }

    files::write_file(folders.user(), "VSS-2026-000123.vsslic",
                      files::licence_file(files::annual()));
    const auto watch = control::watch_licence_folders(folders.options(), 1h);
    const license::status found = watch->latest();
    REQUIRE(found.state == license::state::valid);
    REQUIRE(found.value.has_value());
    REQUIRE(found.value->edition == license::edition::pro);
    REQUIRE(!std::filesystem::exists(folders.clock_file()));
}
