//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The timer rules of docs/updates.md, section 6.2, with a clock the test
// moves. No test waits: `poll` is what the service's own timer calls.
//

#include "fixture.hpp"

using namespace vss::gui::updates;
using namespace vss::gui::updates::testing;

namespace
{

constexpr qint64 kHour = 3600;
constexpr qint64 kDay = 24 * kHour;

QByteArray current_feed(std::uint64_t serial = 5)
{
    return feed_body(serial, {make_release("0.2.0")});
}

/// A service with the automatic check on, started at time 0.
void start_automatic(harness& h)
{
    h.create();
    h.service->set_automatic(true);
    h.service->start();
}

/// Time `seconds` after the start: polls, and answers the request that the
/// poll made with `error`, if there is one. True when a request was made.
bool poll_and_fail(harness& h, qint64 seconds,
                   transport_error error = transport_error::network)
{
    const int before = h.transport.feed_requests;
    h.set_time(seconds);
    if (h.transport.feed_requests == before)
        return false;
    h.transport.reply_feed(error);
    return true;
}

} // namespace

TEST_CASE("update timer: nothing is asked for before a minute has passed")
{
    harness h;
    start_automatic(h);
    h.set_time(0);
    h.set_time(30);
    h.set_time(59);
    REQUIRE_EQ(h.transport.feed_requests, 0);
    h.set_time(60);
    REQUIRE_EQ(h.transport.feed_requests, 1);
}

TEST_CASE("update timer: nothing is asked for before the window is shown")
{
    harness h;
    h.create();
    h.service->set_automatic(true);
    h.set_time(10 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 0);
}

TEST_CASE("update timer: nothing is asked for when the check is not "
          "automatic")
{
    harness h;
    h.create();
    h.service->start();
    h.set_time(60);
    h.set_time(10 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 0);
    REQUIRE_EQ(h.state().state, check_state::idle);
}

TEST_CASE("update timer: one check is made when it is due")
{
    harness h;
    start_automatic(h);
    h.set_time(60);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    h.set_time(61);
    h.set_time(100);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    h.transport.reply_feed(transport_error::none, current_feed());
    REQUIRE_EQ(h.state().state, check_state::current);
}

TEST_CASE("update timer: a feed that was accepted is not asked for again "
          "within a day, and is after a day while Studio stays open")
{
    harness h;
    start_automatic(h);
    h.set_time(60);
    h.transport.reply_feed(transport_error::none, current_feed());
    REQUIRE(h.state().last_checked == start_time().addSecs(60));

    h.set_time(60 + kDay - 1);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    h.set_time(60 + kDay);
    REQUIRE_EQ(h.transport.feed_requests, 2);
    h.transport.reply_feed(transport_error::none, current_feed(6));

    h.set_time(60 + 2 * kDay - 1);
    REQUIRE_EQ(h.transport.feed_requests, 2);
    h.set_time(60 + 2 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 3);
}

TEST_CASE("update timer: a check by hand counts as the last check")
{
    harness h;
    start_automatic(h);
    h.set_time(100);
    h.check_with(current_feed());
    h.set_time(100 + kDay - 1);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    h.set_time(100 + kDay);
    REQUIRE_EQ(h.transport.feed_requests, 2);
}

TEST_CASE("update timer: the last check of an earlier run is respected")
{
    harness h;
    QSettings().setValue(
        QStringLiteral("updates/lastCheck"),
        start_time().addSecs(-10 * kHour).toString(Qt::ISODate));
    start_automatic(h);
    h.set_time(60);
    h.set_time(13 * kHour);
    REQUIRE_EQ(h.transport.feed_requests, 0);
    h.set_time(14 * kHour);
    REQUIRE_EQ(h.transport.feed_requests, 1);
}

TEST_CASE("update timer: a clock set back does not hold the check off for "
          "good")
{
    harness h;
    QSettings().setValue(QStringLiteral("updates/lastCheck"),
                         start_time().addSecs(30 * kDay).toString(Qt::ISODate));
    start_automatic(h);
    h.set_time(60);
    REQUIRE_EQ(h.transport.feed_requests, 1);
}

TEST_CASE("update timer: a failed timer check is silent and keeps the state "
          "that was shown")
{
    harness h;
    start_automatic(h);
    REQUIRE(poll_and_fail(h, 60));
    REQUIRE_EQ(h.state().state, check_state::idle);
    REQUIRE_EQ(h.state().failure, check_failure::none);
    REQUIRE(!h.state().last_checked.isValid());
    REQUIRE(!QSettings().contains(QStringLiteral("updates/lastCheck")));
    REQUIRE(!h.service->chip_due());
    REQUIRE(!h.service->notice_due());
}

TEST_CASE("update timer: a feed that is not trusted is silent on the timer "
          "too")
{
    harness h;
    start_automatic(h);
    h.set_time(60);
    h.transport.reply_feed(transport_error::none, QByteArray("not a feed"));
    REQUIRE_EQ(h.state().state, check_state::idle);
    REQUIRE_EQ(h.state().failure, check_failure::none);
    // And it is tried again in an hour.
    h.set_time(60 + kHour);
    REQUIRE_EQ(h.transport.feed_requests, 2);
}

TEST_CASE("update timer: a silent failure puts back what was on show")
{
    harness h;
    start_automatic(h);
    h.check_with(feed_body(5, {make_release("0.3.0")}));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE(poll_and_fail(h, kDay));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));
    REQUIRE(h.service->chip_due());

    // And a failure that a check by hand showed stays shown.
    harness failed;
    start_automatic(failed);
    failed.check_failing(transport_error::tls);
    REQUIRE_EQ(failed.state().state, check_state::failed);
    REQUIRE(poll_and_fail(failed, 60));
    REQUIRE_EQ(failed.state().state, check_state::failed);
    REQUIRE_EQ(failed.state().failure, check_failure::unreachable);
}

TEST_CASE("update timer: a failed check is tried again after an hour, three "
          "times, and then not until a day after the first failure")
{
    harness h;
    start_automatic(h);
    REQUIRE(poll_and_fail(h, 60));
    REQUIRE_EQ(h.transport.feed_requests, 1);

    // Nothing within the hour.
    REQUIRE(!poll_and_fail(h, 60 + kHour - 1));

    REQUIRE(poll_and_fail(h, 60 + kHour));
    REQUIRE_EQ(h.transport.feed_requests, 2);
    REQUIRE(!poll_and_fail(h, 60 + 2 * kHour - 1));
    REQUIRE(poll_and_fail(h, 60 + 2 * kHour));
    REQUIRE_EQ(h.transport.feed_requests, 3);
    REQUIRE(!poll_and_fail(h, 60 + 3 * kHour - 1));
    REQUIRE(poll_and_fail(h, 60 + 3 * kHour));
    REQUIRE_EQ(h.transport.feed_requests, 4);

    // Three retries have been made: now nothing for the rest of the day.
    REQUIRE(!poll_and_fail(h, 60 + 4 * kHour));
    REQUIRE(!poll_and_fail(h, 60 + 12 * kHour));
    REQUIRE(!poll_and_fail(h, 60 + kDay - 1));
    REQUIRE_EQ(h.transport.feed_requests, 4);

    // A day after the first failure it starts again.
    REQUIRE(poll_and_fail(h, 60 + kDay));
    REQUIRE_EQ(h.transport.feed_requests, 5);
    REQUIRE(poll_and_fail(h, 60 + kDay + kHour));
    REQUIRE_EQ(h.transport.feed_requests, 6);
}

TEST_CASE("update timer: a retry that works ends the retries")
{
    harness h;
    start_automatic(h);
    REQUIRE(poll_and_fail(h, 60));
    h.set_time(60 + kHour);
    REQUIRE_EQ(h.transport.feed_requests, 2);
    h.transport.reply_feed(transport_error::none, current_feed());
    REQUIRE_EQ(h.state().state, check_state::current);

    h.set_time(60 + 2 * kHour);
    h.set_time(60 + kHour + kDay - 1);
    REQUIRE_EQ(h.transport.feed_requests, 2);
    h.set_time(60 + kHour + kDay);
    REQUIRE_EQ(h.transport.feed_requests, 3);
}

TEST_CASE("update timer: a check by hand that works ends the retries")
{
    harness h;
    start_automatic(h);
    REQUIRE(poll_and_fail(h, 60));
    h.set_time(100);
    h.check_with(current_feed());
    h.set_time(60 + kHour);
    REQUIRE_EQ(h.transport.feed_requests, 2);
    h.set_time(100 + kDay);
    REQUIRE_EQ(h.transport.feed_requests, 3);
}

TEST_CASE("update timer: a check by hand that fails does not touch the "
          "retries")
{
    harness h;
    start_automatic(h);
    REQUIRE(poll_and_fail(h, 60));
    h.set_time(120);
    h.check_failing(transport_error::network);
    REQUIRE_EQ(h.state().state, check_state::failed);
    REQUIRE_EQ(h.transport.feed_requests, 2);
    // The retry is still an hour after the failure of the timer check.
    h.set_time(60 + kHour);
    REQUIRE_EQ(h.transport.feed_requests, 3);
}

TEST_CASE("update timer: nothing is started while a check runs")
{
    harness h;
    start_automatic(h);
    h.set_time(100);
    h.service->check_now();
    REQUIRE(h.transport.feed_pending());
    h.set_time(100 + 2 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 1);
}

TEST_CASE("update timer: nothing is started while a download runs")
{
    harness h;
    start_automatic(h);
    h.check_with(feed_body(5, {make_release("0.3.0")}));
    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::downloading);
    h.set_time(3 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 1);

    // Nor while the download is being checked.
    h.transport.send_chunk(QByteArrayView(payload_bytes(1000)));
    h.transport.finish_download(transport_error::none);
    REQUIRE_EQ(h.state().download, download_state::verifying);
    h.set_time(4 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 1);

    // When it is ready the timer may check again.
    h.pump_download();
    REQUIRE_EQ(h.state().download, download_state::ready);
    h.set_time(5 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 2);
}

TEST_CASE("update timer: a developer build never checks on the timer")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.2.0-dev");
    start_automatic(h);
    h.set_time(60);
    h.set_time(10 * kDay);
    REQUIRE_EQ(h.transport.feed_requests, 0);
    // A check by hand still works.
    h.check_with(feed_body(5, {make_release("0.3.0")}));
    REQUIRE_EQ(h.state().state, check_state::current);
}

TEST_CASE("update timer: no check is started under a policy or with no key")
{
    {
        harness h;
        h.settings.disabled_by_policy = true;
        start_automatic(h);
        h.set_time(10 * kDay);
        REQUIRE_EQ(h.transport.feed_requests, 0);
    }
    {
        harness h;
        h.settings.keys.clear();
        start_automatic(h);
        h.set_time(10 * kDay);
        REQUIRE_EQ(h.transport.feed_requests, 0);
    }
}

TEST_CASE("update timer: a timer check shows the checking state and ends in "
          "the result")
{
    harness h;
    start_automatic(h);
    h.set_time(60);
    REQUIRE_EQ(h.state().state, check_state::checking);
    h.transport.reply_feed(transport_error::none,
                           feed_body(5, {make_release("0.3.0")}));
    REQUIRE_EQ(h.state().state, check_state::available);
}

TEST_CASE("update timer: pressing Check now during a timer check makes its "
          "failure show")
{
    harness h;
    start_automatic(h);
    h.set_time(60);
    h.service->check_now();
    REQUIRE_EQ(h.transport.feed_requests, 1);
    h.transport.reply_feed(transport_error::network);
    REQUIRE_EQ(h.state().state, check_state::failed);
    REQUIRE_EQ(h.state().failure, check_failure::unreachable);
}

TEST_CASE("update timer: start twice keeps the first time")
{
    harness h;
    h.create();
    h.service->set_automatic(true);
    h.service->start();
    h.now = h.now.addSecs(50);
    h.service->start();
    h.set_time(60);
    REQUIRE_EQ(h.transport.feed_requests, 1);
}
