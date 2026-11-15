//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for the payload of an update feed. The feed is written by
// someone else and read after its signature verified, so the reader must
// refuse bad input with an error and never crash. What it accepts must be
// inside every limit the header promises, and each version in it must write
// back and read again as an equal one. The input is also read as a version
// number, because a payload that reaches the version reader is rare among
// random bytes. The choice functions are run on whatever was accepted.
//

#include <vss/update/error.hpp>
#include <vss/update/feed.hpp>
#include <vss/update/select.hpp>
#include <vss/update/version.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

namespace upd = vss::update;

/// A property did not hold: stop, so the fuzzer keeps the input.
void require(bool holds)
{
    if (!holds)
    {
        std::abort();
    }
}

void require_version(const upd::version& value)
{
    require(!upd::is_developer_build(value));
    require(value.major <= upd::max_version_number);
    require(value.minor <= upd::max_version_number);
    require(value.patch <= upd::max_version_number);
    require(value.prerelease.size() <= upd::max_prerelease_identifiers);
    const std::string text = upd::to_string(value);
    require(text.size() <= upd::max_version_length);
    const auto again = upd::parse_version(text);
    if (!again.has_value())
    {
        std::abort();
    }
    require(*again == value);
    require(upd::compare(*again, value) == 0);
}

void require_text(const std::string& name, const std::string& url,
                  std::uint64_t size)
{
    require(!name.empty() && name.size() <= upd::max_name_length);
    require(name.front() != '.');
    require(name.find_first_of("/\\:") == std::string::npos);
    require(url.starts_with("https://") && url.size() <= upd::max_url_length);
    // No user name or password: an `@` may only come after the first slash.
    require(url.substr(0, url.find('/', 8)).find('@') == std::string::npos);
    require(url.find('\\') == std::string::npos);
    require(url.find_first_of(" \t\r\n") == std::string::npos);
    require(size >= 1 && size <= upd::max_file_size);
}

/// What `parse_feed_payload` promises about a feed it accepts.
void require_feed(const upd::feed& feed)
{
    require(feed.serial >= 1 && feed.serial <= upd::max_serial);
    require(feed.studio.size() <= upd::max_studio_releases);
    require(feed.plugins.size() <= upd::max_plugins);

    std::set<upd::channel> channels;
    for (const upd::studio_release& release : feed.studio)
    {
        require(channels.insert(release.channel).second);
        require_version(release.version);
        require((release.channel == upd::channel::preview) ==
                release.version.is_prerelease());
        require(release.released.ok());
        require(release.summary.size() <= upd::max_summary_length);
        require(release.notes_url.starts_with("https://"));
        require(!release.files.empty() &&
                release.files.size() <= upd::max_files);
        std::set<std::pair<upd::file_kind, upd::architecture>> seen;
        for (const upd::studio_file& file : release.files)
        {
            require(seen.insert({file.kind, file.arch}).second);
            require(upd::file_kind_from(upd::to_string(file.kind)) ==
                    file.kind);
            require_text(file.name, file.url, file.size);
        }
    }

    std::set<std::string> ids;
    for (const upd::plugin_release& release : feed.plugins)
    {
        require(ids.insert(release.id).second);
        require(release.id.starts_with(upd::official_plugin_prefix));
        require(release.id.size() <= upd::max_plugin_id_length);
        require_version(release.version);
        require_version(release.requires_studio);
        require(release.released.ok());
        require(release.summary.size() <= upd::max_summary_length);
        require(release.files.size() <= upd::max_files);
        std::set<std::pair<upd::operating_system, upd::architecture>> seen;
        for (const upd::plugin_file& file : release.files)
        {
            require(seen.insert({file.os, file.arch}).second);
            require_text(file.name, file.url, file.size);
        }
    }
}

} // namespace

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    std::string detail;
    const auto feed = upd::parse_feed_payload(text, &detail);
    if (feed)
    {
        require(detail.empty());
        require_feed(*feed);

        // The same feed is chosen from for every kind of install and for a
        // few running versions; nothing it returns may point outside it.
        const upd::version running{0, 2, 0, {}};
        const upd::installed_plugin installed{
            feed->plugins.empty() ? std::string("com.virtualserialstudio.x")
                                  : feed->plugins.front().id,
            upd::version{0, 0, 1, {}}, false, false};
        for (const bool preview : {false, true})
        {
            const auto chosen = upd::choose_studio(*feed, running, preview,
                                                   upd::file_kind::linux_deb,
                                                   upd::architecture::x86_64);
            if (chosen)
            {
                require(chosen->release != nullptr);
                require(upd::compare(chosen->release->version, running) > 0);
            }
        }
        for (const upd::plugin_update& offer : upd::choose_plugins(
                 *feed, running, upd::operating_system::linux,
                 upd::architecture::x86_64,
                 std::span<const upd::installed_plugin>(&installed, 1)))
        {
            require(offer.release != nullptr);
        }
    }
    else
    {
        require(!detail.empty());
        require(feed.error() == upd::update_errc::feed_malformed ||
                feed.error() == upd::update_errc::feed_version_unsupported);
    }

    if (const auto parsed = upd::parse_version(text))
    {
        // A version has one spelling.
        require(upd::to_string(*parsed) == text);
        require(upd::compare(*parsed, *parsed) == 0);
    }
    return 0;
}
