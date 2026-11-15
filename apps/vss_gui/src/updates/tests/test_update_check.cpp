//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The check: what a reply becomes, what is stored, what is offered and what
// the notices say. Every reply is a feed signed in the test with the
// throwaway test key.
//

#include "fixture.hpp"

#include <QTimer>

using namespace vss::gui::updates;
using namespace vss::gui::updates::testing;

namespace
{

constexpr const char* kLastCheck = keys::kLastCheck;
constexpr const char* kLastSerial = keys::kLastSerial;

/// A feed with one stable release.
QByteArray stable_feed(const std::string& version, std::uint64_t serial = 5)
{
    return feed_body(serial, {make_release(version)});
}

bool stored(QAnyStringView key)
{
    return QSettings().contains(key);
}

QString text_of(QAnyStringView key)
{
    return QSettings().value(key).toString();
}

} // namespace

// -- the first state ----------------------------------------------------------

TEST_CASE("update service: it starts idle and makes no request")
{
    harness h;
    h.create();
    REQUIRE_EQ(h.state().state, check_state::idle);
    REQUIRE_EQ(h.state().failure, check_failure::none);
    REQUIRE(!h.state().offer.has_value());
    REQUIRE(!h.state().developer_build);
    REQUIRE(!h.state().last_checked.isValid());
    REQUIRE(!h.state().install_kind.has_value());
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE(h.service->available());
    REQUIRE(!h.service->automatic());
    REQUIRE(!h.service->include_prerelease());
    REQUIRE_EQ(h.transport.feed_requests, 0);
    REQUIRE_EQ(h.probe->calls, 0);
}

TEST_CASE("update service: a policy that turns the check off is the state")
{
    harness h;
    h.settings.disabled_by_policy = true;
    h.create();
    REQUIRE_EQ(h.state().state, check_state::disabled_by_policy);
    REQUIRE(!h.service->available());
}

TEST_CASE("update service: a build with no key cannot check")
{
    harness h;
    h.settings.keys.clear();
    h.create();
    REQUIRE_EQ(h.state().state, check_state::no_keys);
    REQUIRE(!h.service->available());
}

TEST_CASE("update service: the policy comes before the missing key")
{
    harness h;
    h.settings.disabled_by_policy = true;
    h.settings.keys.clear();
    h.create();
    REQUIRE_EQ(h.state().state, check_state::disabled_by_policy);
}

TEST_CASE("update service: the time of the last check is read at the start")
{
    harness h;
    QSettings().setValue(kLastCheck, QStringLiteral("2026-11-30T08:15:00Z"));
    h.create();
    REQUIRE(h.state().last_checked.isValid());
    REQUIRE(h.state().last_checked ==
            QDateTime(QDate(2026, 11, 30), QTime(8, 15), QTimeZone::UTC));
    REQUIRE_EQ(h.state().state, check_state::idle);
}

TEST_CASE("update service: a stored time that is not a time is no time")
{
    harness h;
    QSettings().setValue(kLastCheck, QStringLiteral("yesterday"));
    h.create();
    REQUIRE(!h.state().last_checked.isValid());
}

TEST_CASE("update service: a developer build knows it is one")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.2.0-dev");
    h.create();
    REQUIRE(h.state().developer_build);

    harness release_build;
    release_build.settings.running_version = QStringLiteral("0.2.0-rc.1");
    release_build.create();
    REQUIRE(!release_build.state().developer_build);
}

// -- a check by hand ----------------------------------------------------------

TEST_CASE("update service: a check by hand finds the running version current")
{
    harness h;
    h.create();
    h.service->check_now();
    REQUIRE_EQ(h.state().state, check_state::checking);
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(h.probe->calls, 1);
    REQUIRE_EQ(h.state().install_kind,
               upd_kind(upd::file_kind::windows_installer));

    h.transport.reply_feed(transport_error::none, stable_feed("0.2.0"));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE_EQ(h.state().failure, check_failure::none);
    REQUIRE(!h.state().offer.has_value());
    REQUIRE(h.state().last_checked == start_time());
    REQUIRE_EQ(text_of(kLastCheck), QStringLiteral("2026-12-01T12:00:00Z"));
    REQUIRE_EQ(text_of(kLastSerial), QStringLiteral("5"));
    REQUIRE(h.changes >= 2);
}

TEST_CASE("update service: a newer version is offered with its file")
{
    harness h;
    const QByteArray bytes = payload_bytes(1234);
    h.create();
    h.check_with(feed_body(5, {make_release("0.3.0", "stable", bytes)}));

    REQUIRE_EQ(h.state().state, check_state::available);
    const auto& offer = require_offer(h);
    REQUIRE_EQ(offer.version_text, QStringLiteral("0.3.0"));
    REQUIRE_EQ(offer.summary, QStringLiteral("What is new."));
    REQUIRE_EQ(offer.notes_url,
               QUrl(QStringLiteral("https://example.org/notes/0.3.0")));
    REQUIRE(offer.file.has_value());
    REQUIRE_EQ(offer.file->name,
               QStringLiteral("studio-0.3.0-windows-x86_64.exe"));
    REQUIRE_EQ(offer.file->url,
               QUrl(QStringLiteral(
                   "https://example.org/dl/studio-0.3.0-windows-x86_64.exe")));
    REQUIRE_EQ(offer.file->size, std::uint64_t{1234});
    REQUIRE_EQ(digest_hex(offer.file->sha512),
               QString::fromStdString(hex_of(bytes)));
}

TEST_CASE("update service: an update with no file for this install is still "
          "offered")
{
    harness h;
    h.probe->answer = upd::file_kind::linux_deb;
    h.create();
    h.check_with(stable_feed("0.3.0"));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE(require_offer(h).file == std::nullopt);
    REQUIRE_EQ(h.state().install_kind, upd_kind(upd::file_kind::linux_deb));
}

TEST_CASE("update service: another architecture gets no file")
{
    harness h;
    h.settings.architecture = upd::architecture::arm64;
    h.create();
    h.check_with(stable_feed("0.3.0"));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE(!require_offer(h).file.has_value());
}

TEST_CASE("update service: every transport error is unreachable")
{
    for (const transport_error error :
         {transport_error::network, transport_error::tls,
          transport_error::timeout, transport_error::too_large,
          transport_error::redirected})
    {
        harness h;
        h.create();
        h.check_failing(error);
        REQUIRE_EQ(h.state().state, check_state::failed);
        REQUIRE_EQ(h.state().failure, check_failure::unreachable);
        REQUIRE(!h.state().last_checked.isValid());
        REQUIRE(!stored(kLastCheck));
        REQUIRE(!stored(kLastSerial));
    }
}

TEST_CASE("update service: a feed that cannot be trusted is untrusted and "
          "nothing of it is used")
{
    const QByteArray payload_only = QByteArray("{\"feed_version\": 1}");
    const std::string incomplete = R"({"feed_version": 1, "serial": 3})";
    struct bad
    {
        const char* name = "";
        QByteArray body;
    };
    const std::vector<bad> cases{
        {"a bad signature", badly_signed_body(5, {make_release("0.3.0")})},
        {"a malformed payload", signed_body(incomplete)},
        {"a wrong purpose",
         signed_body(payload_text(5, {make_release("0.3.0")}), "licence")},
        {"no envelope at all", payload_only},
        {"an empty body", QByteArray()}};
    for (const bad& entry : cases)
    {
        harness h;
        h.create();
        h.check_with(entry.body);
        if (h.state().state != check_state::failed ||
            h.state().failure != check_failure::untrusted)
            throw vss_test::failure(std::string("accepted: ") + entry.name);
        REQUIRE(!h.state().offer.has_value());
        REQUIRE(!h.state().last_checked.isValid());
        REQUIRE(!stored(kLastCheck));
        REQUIRE(!stored(kLastSerial));
    }
}

TEST_CASE("update service: a feed older than one already accepted is "
          "untrusted")
{
    harness h;
    h.create();
    h.check_with(stable_feed("0.3.0", 20));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE_EQ(text_of(kLastSerial), QStringLiteral("20"));

    h.now = h.now.addSecs(100);
    h.check_with(stable_feed("0.4.0", 19));
    REQUIRE_EQ(h.state().state, check_state::failed);
    REQUIRE_EQ(h.state().failure, check_failure::untrusted);
    REQUIRE_EQ(text_of(kLastSerial), QStringLiteral("20"));
    // The time is still that of the feed that was accepted.
    REQUIRE(h.state().last_checked == start_time());
    REQUIRE_EQ(text_of(kLastCheck), QStringLiteral("2026-12-01T12:00:00Z"));
}

TEST_CASE("update service: the same serial is accepted and a higher one is "
          "stored")
{
    harness h;
    QSettings().setValue(kLastSerial, QStringLiteral("7"));
    h.create();
    h.check_with(stable_feed("0.2.0", 7));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE_EQ(text_of(kLastSerial), QStringLiteral("7"));

    h.check_with(stable_feed("0.2.0", 8));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE_EQ(text_of(kLastSerial), QStringLiteral("8"));
}

TEST_CASE("update service: a stored serial that is not a number counts as "
          "zero")
{
    for (const char* text : {"abc", "-4", "", "12x", "99999999999999999999999"})
    {
        harness h;
        QSettings().setValue(kLastSerial, QString::fromLatin1(text));
        h.create();
        h.check_with(stable_feed("0.2.0", 1));
        REQUIRE_EQ(h.state().state, check_state::current);
        REQUIRE_EQ(text_of(kLastSerial), QStringLiteral("1"));
    }
}

TEST_CASE("update service: nothing happens when a policy forbids the check")
{
    harness h;
    h.settings.disabled_by_policy = true;
    h.create();
    h.service->check_now();
    h.service->start();
    h.service->poll();
    REQUIRE_EQ(h.transport.feed_requests, 0);
    REQUIRE_EQ(h.probe->calls, 0);
    REQUIRE_EQ(h.state().state, check_state::disabled_by_policy);
    REQUIRE_EQ(h.changes, 0);
}

TEST_CASE("update service: nothing happens when the build has no key")
{
    harness h;
    h.settings.keys.clear();
    h.create();
    h.service->check_now();
    REQUIRE_EQ(h.transport.feed_requests, 0);
    REQUIRE_EQ(h.state().state, check_state::no_keys);
    REQUIRE_EQ(h.changes, 0);
}

TEST_CASE("update service: a second check by hand while checking makes no "
          "second request")
{
    harness h;
    h.create();
    h.service->check_now();
    h.service->check_now();
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(h.probe->calls, 1);
    h.transport.reply_feed(transport_error::none, stable_feed("0.3.0"));
    REQUIRE_EQ(h.state().state, check_state::available);

    // When it has ended another may start.
    h.service->check_now();
    REQUIRE_EQ(h.transport.feed_requests, 2);
    // The kind is kept: the probe is asked once.
    REQUIRE_EQ(h.probe->calls, 1);
}

TEST_CASE("update service: a version that is not newer is no update")
{
    {
        harness h;
        h.settings.running_version = QStringLiteral("0.3.0");
        h.create();
        h.check_with(stable_feed("0.3.0"));
        REQUIRE_EQ(h.state().state, check_state::current);
        REQUIRE(!h.state().offer.has_value());
    }
    {
        harness h;
        h.settings.running_version = QStringLiteral("0.4.0");
        h.create();
        h.check_with(stable_feed("0.3.0"));
        REQUIRE_EQ(h.state().state, check_state::current);
    }
    {
        // A pre-release is lower than its own final release.
        harness h;
        h.settings.running_version = QStringLiteral("0.3.0");
        h.create();
        h.service->set_include_prerelease(true);
        h.check_with(feed_body(5, {make_release("0.3.0-rc.1", "preview")}));
        REQUIRE_EQ(h.state().state, check_state::current);
    }
}

TEST_CASE("update service: a running version that is not a version makes "
          "every check untrusted without a request")
{
    harness h;
    h.settings.running_version = QStringLiteral("v1");
    h.create();
    h.service->check_now();
    REQUIRE_EQ(h.state().state, check_state::failed);
    REQUIRE_EQ(h.state().failure, check_failure::untrusted);
    REQUIRE_EQ(h.transport.feed_requests, 0);
}

// -- the pre-release setting
// ----------------------------------------------------

TEST_CASE("update service: the pre-release setting chooses the entry, and "
          "changing it starts no check")
{
    harness h;
    h.create();
    const QByteArray body = feed_body(
        5, {make_release("0.3.0"), make_release("0.4.0-rc.1", "preview")});
    h.check_with(body);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));

    const int requests = h.transport.feed_requests;
    h.service->set_include_prerelease(true);
    REQUIRE(h.service->include_prerelease());
    REQUIRE_EQ(h.transport.feed_requests, requests);
    // What is on show stays until the next check.
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));

    h.check_with(body);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.4.0-rc.1"));
    REQUIRE(require_offer(h).version.is_prerelease());

    h.service->set_include_prerelease(false);
    h.check_with(body);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));
}

TEST_CASE("update service: a preview that is not newer than the stable one "
          "is not chosen")
{
    harness h;
    h.create();
    h.service->set_include_prerelease(true);
    h.check_with(feed_body(
        5, {make_release("0.3.0"), make_release("0.3.0-rc.1", "preview")}));
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));
}

TEST_CASE("update service: a running pre-release is offered pre-releases "
          "without the setting")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.3.0-rc.1");
    h.create();
    REQUIRE(!h.service->include_prerelease());
    h.check_with(feed_body(
        5, {make_release("0.2.0"), make_release("0.3.0-rc.2", "preview")}));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0-rc.2"));
}

// -- a developer build
// ----------------------------------------------------------

TEST_CASE("update service: a developer build is current and names the newest "
          "release, with no file")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.2.0-dev");
    h.create();
    h.check_with(feed_body(5, {make_release("0.3.0")}));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE(h.state().developer_build);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));
    REQUIRE(!require_offer(h).file.has_value());
    REQUIRE(!h.service->chip_due());
    REQUIRE(!h.service->notice_due());
    // No install can be started from it.
    h.service->start_download();
    REQUIRE_EQ(h.state().download, download_state::none);
    REQUIRE_EQ(h.transport.download_requests, 0);
}

TEST_CASE("update service: a developer build names the stable entry unless "
          "pre-releases are wanted")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.2.0-dev");
    h.create();
    const QByteArray body = feed_body(
        5, {make_release("0.3.0"), make_release("0.4.0-rc.1", "preview")});
    h.check_with(body);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));

    h.service->set_include_prerelease(true);
    h.check_with(body);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.4.0-rc.1"));
    REQUIRE(!require_offer(h).file.has_value());
}

TEST_CASE("update service: a developer build names a release that is older "
          "than itself, and none when the feed has none")
{
    harness h;
    h.settings.running_version = QStringLiteral("0.5.0-dev");
    h.create();
    h.check_with(feed_body(5, {make_release("0.3.0")}));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE_EQ(require_offer(h).version_text, QStringLiteral("0.3.0"));

    // Only a preview, and the setting is off: no entry is allowed.
    h.check_with(feed_body(6, {make_release("0.6.0-rc.1", "preview")}));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE(!h.state().offer.has_value());

    h.check_with(feed_body(7, {}));
    REQUIRE_EQ(h.state().state, check_state::current);
    REQUIRE(!h.state().offer.has_value());
}

// -- the question at first start
// --------------------------------------------------

TEST_CASE("update service: the question is due until it is answered, and "
          "the answer is kept")
{
    harness h;
    h.create();
    REQUIRE(h.service->question_due());
    h.service->answer_question(false);
    REQUIRE(!h.service->question_due());
    REQUIRE(!h.service->automatic());

    h.recreate();
    REQUIRE(!h.service->question_due());
    REQUIRE(!h.service->automatic());

    harness yes;
    yes.create();
    yes.service->answer_question(true);
    REQUIRE(!yes.service->question_due());
    REQUIRE(yes.service->automatic());
    yes.recreate();
    REQUIRE(!yes.service->question_due());
    REQUIRE(yes.service->automatic());
}

TEST_CASE("update service: the question is not asked under a policy, with no "
          "key or in a developer build")
{
    {
        harness h;
        h.settings.disabled_by_policy = true;
        h.create();
        REQUIRE(!h.service->question_due());
        h.service->answer_question(true);
        REQUIRE(!h.service->automatic());
    }
    {
        harness h;
        h.settings.keys.clear();
        h.create();
        REQUIRE(!h.service->question_due());
    }
    {
        harness h;
        h.settings.running_version = QStringLiteral("0.2.0-dev");
        h.create();
        REQUIRE(!h.service->question_due());
    }
}

TEST_CASE("update service: turning on makes the timer check run, and saying "
          "no thanks does not")
{
    for (const bool turn_on : {true, false})
    {
        harness h;
        h.create();
        h.service->answer_question(turn_on);
        h.service->start();
        h.set_time(60);
        REQUIRE_EQ(h.transport.feed_requests, turn_on ? 1 : 0);
        h.set_time(qint64{100} * 3600);
        REQUIRE_EQ(h.transport.feed_requests, turn_on ? 1 : 0);
    }
}

TEST_CASE("update service: the settings are kept for the next service")
{
    harness h;
    h.create();
    h.service->set_automatic(true);
    h.service->set_include_prerelease(true);
    h.recreate();
    REQUIRE(h.service->automatic());
    REQUIRE(h.service->include_prerelease());
    h.service->set_automatic(false);
    h.recreate();
    REQUIRE(!h.service->automatic());
}

TEST_CASE("update service: the settings cannot be changed when no check is "
          "possible")
{
    for (const bool by_policy : {true, false})
    {
        harness h;
        h.settings.disabled_by_policy = by_policy;
        if (!by_policy)
            h.settings.keys.clear();
        h.create();
        h.service->set_automatic(true);
        h.service->set_include_prerelease(true);
        h.service->answer_question(true);
        h.service->skip_offered_version();
        REQUIRE(!h.service->automatic());
        REQUIRE(!h.service->include_prerelease());
        REQUIRE(!stored(QStringLiteral("updates/asked")));
        REQUIRE(h.service->skipped_version().isEmpty());
        REQUIRE_EQ(h.changes, 0);
    }
}

// -- the chip and the notice
// --------------------------------------------------------

namespace
{

/// A service that checks on the timer: automatic is on and the window has
/// been shown for a minute.
void make_timer_check(harness& h, const QByteArray& body)
{
    h.service->set_automatic(true);
    h.service->start();
    h.set_time(60);
    REQUIRE(h.transport.feed_pending());
    h.transport.reply_feed(transport_error::none, body);
}

} // namespace

TEST_CASE("update service: a timer check that finds a version shows the chip "
          "and the notice")
{
    harness h;
    h.create();
    REQUIRE(!h.service->chip_due());
    REQUIRE(!h.service->notice_due());
    make_timer_check(h, stable_feed("0.3.0"));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE(h.service->chip_due());
    REQUIRE(h.service->notice_due());
}

TEST_CASE("update service: Later hides the notice and keeps the chip, for "
          "this run only")
{
    harness h;
    h.create();
    make_timer_check(h, stable_feed("0.3.0"));
    h.service->hide_notice();
    REQUIRE(!h.service->notice_due());
    REQUIRE(h.service->chip_due());

    // The next service has forgotten it.
    h.recreate();
    h.service->set_automatic(true);
    h.service->start();
    h.set_time(qint64{24} * 3600 + 200);
    h.transport.reply_feed(transport_error::none, stable_feed("0.3.0", 6));
    REQUIRE(h.service->notice_due());
}

TEST_CASE("update service: a check by hand shows the chip and no notice")
{
    harness h;
    h.create();
    h.check_with(stable_feed("0.3.0"));
    REQUIRE(h.service->chip_due());
    REQUIRE(!h.service->notice_due());
}

TEST_CASE("update service: a check by hand does not take back a notice that "
          "a timer check showed")
{
    harness h;
    h.create();
    make_timer_check(h, stable_feed("0.3.0"));
    REQUIRE(h.service->notice_due());
    h.check_with(stable_feed("0.3.0", 6));
    REQUIRE(h.service->chip_due());
    REQUIRE(h.service->notice_due());
    // But another version found by hand is not announced.
    h.check_with(stable_feed("0.4.0", 7));
    REQUIRE(h.service->chip_due());
    REQUIRE(!h.service->notice_due());
}

TEST_CASE("update service: the chip stays while a check runs")
{
    harness h;
    h.create();
    h.check_with(stable_feed("0.3.0"));
    h.service->check_now();
    REQUIRE_EQ(h.state().state, check_state::checking);
    REQUIRE(h.service->chip_due());
    h.transport.reply_feed(transport_error::none, stable_feed("0.3.0", 6));
    REQUIRE(h.service->chip_due());
}

TEST_CASE("update service: a skipped version is quiet on the timer, shown "
          "by hand, and a later version speaks again")
{
    harness h;
    h.create();
    make_timer_check(h, stable_feed("0.3.0"));
    h.service->skip_offered_version();
    REQUIRE_EQ(h.service->skipped_version(), QStringLiteral("0.3.0"));
    REQUIRE_EQ(text_of(QStringLiteral("updates/skippedVersion")),
               QStringLiteral("0.3.0"));
    REQUIRE(!h.service->chip_due());
    REQUIRE(!h.service->notice_due());

    // The timer finds it again: quiet.
    h.set_time(60 + qint64{24} * 3600);
    REQUIRE(h.transport.feed_pending());
    h.transport.reply_feed(transport_error::none, stable_feed("0.3.0", 6));
    REQUIRE_EQ(h.state().state, check_state::available);
    REQUIRE(!h.service->chip_due());
    REQUIRE(!h.service->notice_due());

    // A check by hand shows it.
    h.check_with(stable_feed("0.3.0", 7));
    REQUIRE(h.service->chip_due());
    REQUIRE(!h.service->notice_due());

    // A later version is not the one that was skipped.
    h.set_time(60 + qint64{48} * 3600 + 10);
    REQUIRE(h.transport.feed_pending());
    h.transport.reply_feed(transport_error::none, stable_feed("0.4.0", 8));
    REQUIRE(h.service->chip_due());
    REQUIRE(h.service->notice_due());
}

TEST_CASE("update service: a skipped version stays skipped in the next "
          "service")
{
    harness h;
    h.create();
    h.check_with(stable_feed("0.3.0"));
    h.service->skip_offered_version();
    h.recreate();
    REQUIRE_EQ(h.service->skipped_version(), QStringLiteral("0.3.0"));
}

TEST_CASE("update service: nothing is skipped when nothing is on offer")
{
    harness h;
    h.create();
    h.service->skip_offered_version();
    REQUIRE(h.service->skipped_version().isEmpty());
    h.check_with(stable_feed("0.2.0"));
    h.service->skip_offered_version();
    REQUIRE(h.service->skipped_version().isEmpty());
}

// -- the probe
// ------------------------------------------------------------------------

TEST_CASE("update service: a probe that answers late holds the request until "
          "it does")
{
    harness h;
    h.probe->at_once = false;
    h.probe->answer = upd::file_kind::linux_rpm;
    h.create();
    h.service->check_now();
    REQUIRE_EQ(h.state().state, check_state::checking);
    REQUIRE(h.probe->waiting());
    REQUIRE_EQ(h.transport.feed_requests, 0);
    REQUIRE(!h.state().install_kind.has_value());

    // Asking again does not ask the probe again.
    h.service->check_now();
    REQUIRE_EQ(h.probe->calls, 1);

    h.probe->answer_now();
    REQUIRE_EQ(h.transport.feed_requests, 1);
    REQUIRE_EQ(h.state().install_kind, upd_kind(upd::file_kind::linux_rpm));
    h.transport.reply_feed(transport_error::none, stable_feed("0.3.0"));
    REQUIRE_EQ(h.state().state, check_state::available);

    h.service->check_now();
    REQUIRE_EQ(h.probe->calls, 1);
    REQUIRE_EQ(h.transport.feed_requests, 2);
}

// -- the service goes while something is outstanding
// ----------------------------------------

TEST_CASE("update service: a reply that arrives after the service is gone is "
          "not used")
{
    harness h;
    h.create();
    h.service->check_now();
    REQUIRE(h.transport.feed_pending());
    h.service.reset();
    REQUIRE_EQ(h.transport.cancels, 1);
    // The transport still delivers; the dead service must not be called.
    h.transport.reply_feed(transport_error::none, stable_feed("0.3.0"));
    REQUIRE(!stored(kLastCheck));
    REQUIRE(!stored(kLastSerial));
}

TEST_CASE("update service: a probe that answers after the service is gone is "
          "not used")
{
    harness h;
    h.probe->at_once = false;
    h.create();
    h.service->check_now();
    REQUIRE(h.probe->waiting());
    h.service.reset();
    h.probe->answer_now();
    REQUIRE_EQ(h.transport.feed_requests, 0);
}

TEST_CASE("update service: a service that made no request cancels nothing")
{
    harness h;
    h.create();
    h.service.reset();
    REQUIRE_EQ(h.transport.cancels, 0);
}

TEST_CASE("update service: a reply that the transport sends as cancelled is "
          "a failure by hand")
{
    harness h;
    h.create();
    h.check_failing(transport_error::cancelled);
    REQUIRE_EQ(h.state().state, check_state::failed);
    REQUIRE_EQ(h.state().failure, check_failure::unreachable);
}

TEST_CASE("update service: the clock of the service is the one it is given")
{
    harness h;
    h.now = QDateTime(QDate(2030, 1, 2), QTime(3, 4, 5), QTimeZone::UTC);
    h.create();
    h.check_with(stable_feed("0.2.0"));
    REQUIRE(h.state().last_checked == h.now);
    REQUIRE_EQ(text_of(kLastCheck), QStringLiteral("2030-01-02T03:04:05Z"));
}

TEST_CASE("update service: the service has one timer of thirty seconds that "
          "starts with the window")
{
    harness h;
    h.create();
    const QTimer* timer = h.service->findChild<QTimer*>();
    REQUIRE(timer != nullptr);
    REQUIRE(!timer->isActive());
    h.service->start();
    REQUIRE(timer->isActive());
    REQUIRE_EQ(timer->interval(), 30 * 1000);
    // No request until the timer's poll finds one due.
    REQUIRE_EQ(h.transport.feed_requests, 0);
}
