//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What to offer from a feed (docs/updates.md, section 4.4): the choice of a
// Studio release and of the plugin updates, as pure functions.
//

#include <vss/update/feed.hpp>
#include <vss/update/select.hpp>
#include <vss/update/version.hpp>

#include "test_framework.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace vss::update;

namespace
{

version v(std::string_view text)
{
    const auto parsed = parse_version(text);
    if (!parsed)
        throw vss_test::failure("not a version: " + std::string(text));
    return *parsed;
}

studio_file studio_file_of(file_kind kind, architecture arch)
{
    studio_file file;
    file.kind = kind;
    file.arch = arch;
    file.name =
        std::string(to_string(kind)) + "-" + std::string(to_string(arch));
    file.url = "https://example.org/" + file.name;
    file.size = 100;
    return file;
}

studio_release studio_release_of(channel which, std::string_view text,
                                 std::vector<studio_file> files)
{
    studio_release release;
    release.channel = which;
    release.version = v(text);
    release.files = std::move(files);
    return release;
}

plugin_file plugin_file_of(operating_system os, architecture arch)
{
    plugin_file file;
    file.os = os;
    file.arch = arch;
    file.name = std::string(to_string(os)) + "-" + std::string(to_string(arch));
    file.url = "https://example.org/" + file.name;
    file.size = 100;
    return file;
}

plugin_release plugin_release_of(std::string_view id, std::string_view text,
                                 std::string_view needs,
                                 std::vector<plugin_file> files)
{
    plugin_release release;
    release.id = std::string(id);
    release.version = v(text);
    release.requires_studio = v(needs);
    release.files = std::move(files);
    return release;
}

installed_plugin installed_of(std::string_view id, std::string_view text,
                              bool came_with_studio = false,
                              bool shadowed = false)
{
    installed_plugin plugin;
    plugin.id = std::string(id);
    plugin.version = v(text);
    plugin.came_with_studio = came_with_studio;
    plugin.shadowed = shadowed;
    return plugin;
}

const std::vector<studio_file> windows_and_linux{
    studio_file_of(file_kind::windows_installer, architecture::x86_64),
    studio_file_of(file_kind::windows_zip, architecture::x86_64),
    studio_file_of(file_kind::linux_deb, architecture::x86_64),
    studio_file_of(file_kind::linux_deb, architecture::arm64)};

/// A final release 0.2.0 and a pre-release 0.3.0-rc.1.
feed both_channels()
{
    feed source;
    source.serial = 5;
    source.studio.push_back(
        studio_release_of(channel::stable, "0.2.0", windows_and_linux));
    source.studio.push_back(
        studio_release_of(channel::preview, "0.3.0-rc.1", windows_and_linux));
    return source;
}

constexpr auto installer = file_kind::windows_installer;
constexpr auto x64 = architecture::x86_64;

} // namespace

// -- Studio -----------------------------------------------------------------

TEST_CASE("update select: a newer stable release is offered")
{
    const feed source = both_channels();
    const auto update =
        choose_studio(source, v("0.1.0"), false, installer, x64);
    REQUIRE(update.has_value());
    REQUIRE(update->release == &source.studio[0]);
    REQUIRE(update->release->version == v("0.2.0"));
    REQUIRE(update->file != nullptr);
    REQUIRE(update->file == &source.studio[0].files[0]);
    REQUIRE(update->file->kind == installer);
    REQUIRE(update->file->arch == x64);
}

TEST_CASE("update select: the file is the one for the kind and architecture")
{
    const feed source = both_channels();
    const auto deb_arm = choose_studio(
        source, v("0.1.0"), false, file_kind::linux_deb, architecture::arm64);
    REQUIRE(deb_arm.has_value());
    REQUIRE(deb_arm->file == &source.studio[0].files[3]);
    const auto zip =
        choose_studio(source, v("0.1.0"), false, file_kind::windows_zip, x64);
    REQUIRE(zip.has_value());
    REQUIRE(zip->file == &source.studio[0].files[1]);
}

TEST_CASE("update select: a release with no file for this install is still "
          "reported")
{
    const feed source = both_channels();
    // The release has no macOS disk image at all.
    const auto no_kind =
        choose_studio(source, v("0.1.0"), false, file_kind::macos_dmg, x64);
    REQUIRE(no_kind.has_value());
    REQUIRE(no_kind->release == &source.studio[0]);
    REQUIRE(no_kind->file == nullptr);
    // The release has the Windows installer for x86_64 only.
    const auto no_arch = choose_studio(source, v("0.1.0"), false, installer,
                                       architecture::arm64);
    REQUIRE(no_arch.has_value());
    REQUIRE(no_arch->release == &source.studio[0]);
    REQUIRE(no_arch->file == nullptr);
}

TEST_CASE("update select: a pre-release is offered only when asked for")
{
    const feed source = both_channels();

    // Stable only: the stable release, though the preview is newer.
    const auto stable =
        choose_studio(source, v("0.1.0"), false, installer, x64);
    REQUIRE(stable.has_value());
    REQUIRE(stable->release->channel == channel::stable);
    REQUIRE(stable->release->version == v("0.2.0"));

    // By the setting: the newest of the two.
    const auto preview =
        choose_studio(source, v("0.1.0"), true, installer, x64);
    REQUIRE(preview.has_value());
    REQUIRE(preview->release == &source.studio[1]);
    REQUIRE(preview->release->channel == channel::preview);
    REQUIRE(preview->file == &source.studio[1].files[0]);
}

TEST_CASE("update select: a pre-release being run is offered the preview "
          "without the setting")
{
    const feed source = both_channels();
    const auto from_rc =
        choose_studio(source, v("0.2.0-rc.1"), false, installer, x64);
    REQUIRE(from_rc.has_value());
    REQUIRE(from_rc->release == &source.studio[1]);

    // A developer build is a pre-release too.
    const auto from_dev =
        choose_studio(source, v("0.1.0-dev"), false, installer, x64);
    REQUIRE(from_dev.has_value());
    REQUIRE(from_dev->release == &source.studio[1]);

    // A final release being run is not.
    const auto from_final =
        choose_studio(source, v("0.1.9"), false, installer, x64);
    REQUIRE(from_final.has_value());
    REQUIRE(from_final->release == &source.studio[0]);
}

TEST_CASE("update select: a preview older than the stable release is not "
          "chosen over it")
{
    feed source;
    source.studio.push_back(
        studio_release_of(channel::stable, "0.4.0", windows_and_linux));
    source.studio.push_back(
        studio_release_of(channel::preview, "0.3.0-rc.1", windows_and_linux));
    const auto update = choose_studio(source, v("0.1.0"), true, installer, x64);
    REQUIRE(update.has_value());
    REQUIRE(update->release == &source.studio[0]);
}

TEST_CASE("update select: the order of the entries does not matter")
{
    feed source = both_channels();
    std::swap(source.studio[0], source.studio[1]);
    const auto update = choose_studio(source, v("0.1.0"), true, installer, x64);
    REQUIRE(update.has_value());
    REQUIRE(update->release->version == v("0.3.0-rc.1"));
    const auto stable =
        choose_studio(source, v("0.1.0"), false, installer, x64);
    REQUIRE(stable.has_value());
    REQUIRE(stable->release->version == v("0.2.0"));
}

TEST_CASE("update select: the feed never causes a downgrade")
{
    const feed source = both_channels();
    // Newer than everything the feed has.
    REQUIRE(!choose_studio(source, v("0.3.0"), true, installer, x64));
    REQUIRE(!choose_studio(source, v("0.3.0"), false, installer, x64));
    REQUIRE(!choose_studio(source, v("1.0.0"), true, installer, x64));
    REQUIRE(!choose_studio(source, v("0.2.1"), false, installer, x64));
    // Between the stable release and the preview: only the preview, and
    // only when it is asked for.
    const auto between =
        choose_studio(source, v("0.2.5"), true, installer, x64);
    REQUIRE(between.has_value());
    REQUIRE(between->release == &source.studio[1]);
    REQUIRE(!choose_studio(source, v("0.2.5"), false, installer, x64));
    // Running a newer pre-release than the preview: nothing, and not the
    // older stable release.
    REQUIRE(!choose_studio(source, v("0.3.0-rc.2"), true, installer, x64));
    REQUIRE(!choose_studio(source, v("0.3.0-rc.2"), false, installer, x64));
}

TEST_CASE("update select: the same version is no update")
{
    const feed source = both_channels();
    REQUIRE(!choose_studio(source, v("0.2.0"), false, installer, x64));
    REQUIRE(!choose_studio(source, v("0.3.0-rc.1"), true, installer, x64));
    REQUIRE(!choose_studio(source, v("0.3.0-rc.1"), false, installer, x64));
    // One step below is an update.
    REQUIRE(choose_studio(source, v("0.1.9"), false, installer, x64));
    REQUIRE(choose_studio(source, v("0.3.0-rc.0"), true, installer, x64));
}

TEST_CASE("update select: a feed with no Studio release offers nothing")
{
    feed source;
    source.serial = 1;
    REQUIRE(!choose_studio(source, v("0.1.0"), false, installer, x64));
    REQUIRE(!choose_studio(source, v("0.1.0"), true, installer, x64));
}

TEST_CASE("update select: only a preview, and preview not wanted")
{
    feed source;
    source.studio.push_back(
        studio_release_of(channel::preview, "0.3.0-rc.1", windows_and_linux));
    REQUIRE(!choose_studio(source, v("0.1.0"), false, installer, x64));
    REQUIRE(choose_studio(source, v("0.1.0"), true, installer, x64));
    REQUIRE(choose_studio(source, v("0.1.0-rc.1"), false, installer, x64));
}

// -- plugins ----------------------------------------------------------------

namespace
{

constexpr auto decoding = "com.virtualserialstudio.decoding";
constexpr auto exporter = "com.virtualserialstudio.export";

feed plugin_feed()
{
    feed source;
    source.plugins.push_back(plugin_release_of(
        decoding, "1.3.0", "0.2.0",
        {plugin_file_of(operating_system::windows, architecture::x86_64),
         plugin_file_of(operating_system::linux, architecture::x86_64)}));
    source.plugins.push_back(plugin_release_of(
        exporter, "2.0.0", "0.2.0",
        {plugin_file_of(operating_system::macos, architecture::arm64)}));
    return source;
}

} // namespace

TEST_CASE("update select: a newer version of an installed plugin is offered")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.2.0")};
    const auto offers = choose_plugins(
        source, v("0.2.0"), operating_system::windows, x64, installed);
    REQUIRE_EQ(offers.size(), 1);
    REQUIRE(offers[0].release == &source.plugins[0]);
    REQUIRE(offers[0].file == &source.plugins[0].files[0]);
    REQUIRE(!offers[0].studio_too_old);
}

TEST_CASE("update select: the file is the one for the system and "
          "architecture")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.2.0")};
    const auto linux_offers = choose_plugins(
        source, v("0.2.0"), operating_system::linux, x64, installed);
    REQUIRE_EQ(linux_offers.size(), 1);
    REQUIRE(linux_offers[0].file == &source.plugins[0].files[1]);
}

TEST_CASE("update select: a plugin with no file for this system is reported "
          "without one")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.2.0")};
    // No macOS file for this plugin.
    const auto macos =
        choose_plugins(source, v("0.2.0"), operating_system::macos,
                       architecture::arm64, installed);
    REQUIRE_EQ(macos.size(), 1);
    REQUIRE(macos[0].release == &source.plugins[0]);
    REQUIRE(macos[0].file == nullptr);
    // A Windows file, but not for arm64.
    const auto arm =
        choose_plugins(source, v("0.2.0"), operating_system::windows,
                       architecture::arm64, installed);
    REQUIRE_EQ(arm.size(), 1);
    REQUIRE(arm[0].file == nullptr);
}

TEST_CASE("update select: a plugin that is not newer is not offered")
{
    const feed source = plugin_feed();
    for (const std::string_view text : {"1.3.0", "1.3.1", "1.4.0", "2.0.0"})
    {
        const std::vector<installed_plugin> installed{
            installed_of(decoding, text)};
        const auto offers = choose_plugins(
            source, v("0.2.0"), operating_system::windows, x64, installed);
        REQUIRE_EQ(std::string(text) + " " + std::to_string(offers.size()),
                   std::string(text) + " 0");
    }
    // A pre-release of the feed's version is older than it.
    const std::vector<installed_plugin> candidate{
        installed_of(decoding, "1.3.0-rc.1")};
    REQUIRE_EQ(choose_plugins(source, v("0.2.0"), operating_system::windows,
                              x64, candidate)
                   .size(),
               1);
}

TEST_CASE("update select: a plugin that is not installed is never offered")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> none;
    REQUIRE(
        choose_plugins(source, v("0.2.0"), operating_system::windows, x64, none)
            .empty());
    // Only what is installed: the feed's other plugin is not offered.
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.0.0")};
    const auto offers = choose_plugins(
        source, v("0.2.0"), operating_system::windows, x64, installed);
    REQUIRE_EQ(offers.size(), 1);
    REQUIRE(offers[0].release->id == decoding);
    // A plugin the feed does not list, official or not.
    const std::vector<installed_plugin> unknown{
        installed_of("com.virtualserialstudio.other", "0.0.1"),
        installed_of("com.example.third-party", "0.0.1")};
    REQUIRE(choose_plugins(source, v("0.2.0"), operating_system::windows, x64,
                           unknown)
                .empty());
}

TEST_CASE("update select: a shadowed plugin is not offered")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.0.0", false, true)};
    REQUIRE(choose_plugins(source, v("0.2.0"), operating_system::windows, x64,
                           installed)
                .empty());
}

TEST_CASE("update select: a plugin that came with Studio is updated with "
          "Studio")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.0.0", true, false)};
    REQUIRE(choose_plugins(source, v("0.2.0"), operating_system::windows, x64,
                           installed)
                .empty());
}

TEST_CASE("update select: a plugin that needs a newer Studio says so")
{
    feed source;
    source.plugins.push_back(plugin_release_of(
        decoding, "1.3.0", "0.3.0-rc.1",
        {plugin_file_of(operating_system::windows, architecture::x86_64)}));
    const std::vector<installed_plugin> installed{
        installed_of(decoding, "1.0.0")};

    // Older than what it needs: flagged, and still reported.
    for (const std::string_view running : {"0.2.0", "0.1.0", "0.3.0-beta.1"})
    {
        const auto offers = choose_plugins(
            source, v(running), operating_system::windows, x64, installed);
        REQUIRE_EQ(offers.size(), 1);
        REQUIRE(offers[0].studio_too_old);
        REQUIRE(offers[0].file != nullptr);
    }
    // Exactly what it needs, or newer: not flagged.
    for (const std::string_view running :
         {"0.3.0-rc.1", "0.3.0-rc.2", "0.3.0", "1.0.0"})
    {
        const auto offers = choose_plugins(
            source, v(running), operating_system::windows, x64, installed);
        REQUIRE_EQ(offers.size(), 1);
        REQUIRE(!offers[0].studio_too_old);
    }
}

TEST_CASE("update select: several plugins keep the order they are installed "
          "in")
{
    const feed source = plugin_feed();
    const std::vector<installed_plugin> installed{
        installed_of(exporter, "1.0.0"),
        installed_of("com.example.other", "1.0.0"),
        installed_of(decoding, "1.0.0")};
    const auto offers =
        choose_plugins(source, v("0.2.0"), operating_system::macos,
                       architecture::arm64, installed);
    REQUIRE_EQ(offers.size(), 2);
    REQUIRE(offers[0].release->id == exporter);
    REQUIRE(offers[0].file == &source.plugins[1].files[0]);
    REQUIRE(offers[1].release->id == decoding);
    REQUIRE(offers[1].file == nullptr);
}
