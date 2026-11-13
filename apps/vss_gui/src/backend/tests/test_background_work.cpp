//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "backend/background_work.hpp"

#include "qt_wait.hpp"
#include "test_framework.hpp"

#include <QCoreApplication>
#include <QThread>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using vss_test::wait_until;

TEST_CASE("background work: runs off the GUI thread and reports on it")
{
    vss::gui::BackgroundWork work;
    REQUIRE(!work.busy());

    struct outcome
    {
        QThread* worked = nullptr;
        QThread* reported = nullptr;
        bool finished = false;
    };
    const auto result = std::make_shared<outcome>();
    std::atomic<bool> release{false};

    REQUIRE(work.start(
        [result, &release]
        {
            result->worked = QThread::currentThread();
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        },
        [result]
        {
            result->reported = QThread::currentThread();
            result->finished = true;
        }));
    REQUIRE(work.busy());

    // The GUI thread is free while the work runs, and a second start (a double
    // click) is refused instead of queued.
    REQUIRE(!work.start([] {}, [] {}));
    QCoreApplication::processEvents();
    REQUIRE(!result->finished);

    release = true;
    REQUIRE(wait_until([&result] { return result->finished; }));
    REQUIRE(!work.busy());
    REQUIRE(result->worked != QCoreApplication::instance()->thread());
    REQUIRE(result->reported == QCoreApplication::instance()->thread());

    // It can be used again once the first job is done.
    bool again = false;
    REQUIRE(work.start([] {}, [&again] { again = true; }));
    REQUIRE(wait_until([&again] { return again; }));
}

TEST_CASE("background work: destroying it waits for the running job")
{
    std::atomic<bool> finished{false};
    bool reported = false;
    {
        vss::gui::BackgroundWork work;
        REQUIRE(work.start(
            [&finished]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                finished = true;
            },
            [&reported] { reported = true; }));
    }
    REQUIRE(finished.load());
    // The report is dropped with the object, never delivered to a dead owner.
    QCoreApplication::processEvents();
    REQUIRE(!reported);
}
