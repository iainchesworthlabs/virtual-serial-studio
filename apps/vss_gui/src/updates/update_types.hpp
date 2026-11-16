//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The values the update service shows to the window (docs/updates.md;
// docs/gui-design/docs/screens/12-updates.md, "Where the state comes from").
//

#pragma once

#include <vss/update/feed.hpp>
#include <vss/update/version.hpp>

#include <QDateTime>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <optional>

namespace vss::gui::updates
{

/// Where the feed is read from. Compiled in: it is not a setting and no
/// environment variable changes it (docs/updates.md, section 4.1).
inline constexpr const char* kFeedAddress =
    "https://virtual-serial-studio.com/updates/v1/feed.json";

/// The whole `User-Agent` of every request. It carries no version, no
/// system and no identifier (section 6.1).
inline constexpr const char* kUserAgent = "VirtualSerialStudio";

/// The page a person is sent to when no package fits this install.
inline constexpr const char* kDownloadsPage =
    "https://virtual-serial-studio.com/downloads.html";

/// A feed larger than this is cut off and refused.
inline constexpr std::int64_t kMaxFeedBytes = std::int64_t{64} * 1024;

/// A request that has not finished after this long is given up.
inline constexpr int kFeedTimeoutMs = 15 * 1000;

/// What the last check came to.
enum class check_state
{
    /// Never checked in this run, and nothing is stored from an earlier one.
    idle,
    checking,
    /// The running version is the newest.
    current,
    /// A newer Studio is published. `snapshot::offer` says which.
    available,
    /// The check did not finish. `snapshot::failure` says why.
    failed,
    /// An administrator's policy forbids the check.
    disabled_by_policy,
    /// The program was built with no feed key, so it can trust no feed.
    no_keys
};

enum class check_failure
{
    none,
    /// No connection, a time-out, a TLS error, a redirect, a status other
    /// than 200 or a body over the limit.
    unreachable,
    /// The reply is not a feed this program trusts: the signature, the
    /// payload, the key's window or the serial.
    untrusted
};

enum class download_state
{
    none,
    downloading,
    /// The bytes are here and are being hashed.
    verifying,
    /// The file matched the feed and is held for the install step.
    ready,
    failed
};

enum class download_failure
{
    none,
    /// The connection failed, timed out or was refused.
    unreachable,
    /// The size or the SHA-512 is not the feed's. The file was deleted.
    mismatch,
    /// The folder could not be written or the disk is full.
    save_failed
};

/// One file of the feed, as the window needs it.
struct offered_file
{
    QString name;
    QUrl url;
    std::uint64_t size = 0;
    vss::update::sha512_digest sha512{};

    friend bool operator==(const offered_file&, const offered_file&) = default;
};

/// A newer Studio.
struct studio_offer
{
    vss::update::version version;
    /// The version as published: "0.3.0", "0.3.0-rc.1".
    QString version_text;
    /// Plain text. May be empty.
    QString summary;
    /// May be empty.
    QUrl notes_url;
    /// Nothing when the release has no package for this kind of install and
    /// this architecture.
    std::optional<offered_file> file;

    friend bool operator==(const studio_offer&, const studio_offer&) = default;
};

/// Everything the window shows, read in one piece.
struct snapshot
{
    check_state state = check_state::idle;
    check_failure failure = check_failure::none;
    /// Set in `available`. In a developer build it is set in `current` too:
    /// the newest release is named and no install is offered.
    std::optional<studio_offer> offer;
    /// The running program is a developer build (`X.Y.Z-dev`).
    bool developer_build = false;
    /// When a feed was last accepted, in UTC. Invalid when never.
    QDateTime last_checked;
    /// How this copy was installed. Nothing until the probe has answered.
    std::optional<vss::update::file_kind> install_kind;

    download_state download = download_state::none;
    download_failure download_error = download_failure::none;
    /// Bytes received so far, and the size the feed gives.
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    /// The checked file, in `download_state::ready`.
    QString ready_path;
};

} // namespace vss::gui::updates
