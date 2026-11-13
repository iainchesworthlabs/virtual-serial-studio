//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/about_dialog.hpp"
#include "support/build_info.hpp"
#include "support/bundle.hpp"
#include "support/check_row.hpp"
#include "support/diagnostic_bundle_dialog.hpp"
#include "support/event_log.hpp"
#include "support/help_contents_dialog.hpp"
#include "support/redact.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "support/setup_check_dialog.hpp"
#include "support/setup_facts.hpp"
#include "support/shortcuts_dialog.hpp"
#include "support/third_party.hpp"
#include "support/troubleshoot.hpp"
#include "support/troubleshoot_dialog.hpp"
#include "support/zip_writer.hpp"
#include "theme/theme.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>

#include <chrono>
#include <cstdint>

namespace
{

namespace support = vss::gui::support;
using vss::gui::confirm_choice;
using vss::gui::ConfirmDialog;
using vss::gui::density_mode;
using vss::gui::FakeStudioBackend;
using vss::gui::theme_mode;

void clear_settings()
{
    QSettings settings;
    settings.clear();
    settings.sync();
}

QString setting(const char* key)
{
    return QSettings().value(QString::fromLatin1(key)).toString();
}

QPushButton* button_with_text(QWidget& root, const QString& text)
{
    const auto buttons = root.findChildren<QPushButton*>();
    for (QPushButton* button : buttons)
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

QRadioButton* radio_with_text(QWidget& root, const QString& text)
{
    const auto buttons = root.findChildren<QRadioButton*>();
    for (QRadioButton* button : buttons)
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

/// Walks a shown dialog and fails on a control without an accessible name,
/// or a button under 24 px.
void check_accessible(QWidget& dialog)
{
    dialog.show();
    QApplication::processEvents();

    QStringList problems;
    for (QWidget* widget : dialog.findChildren<QWidget*>())
    {
        if (!widget->isVisibleTo(&dialog))
            continue;
        if (widget->focusPolicy() == Qt::NoFocus)
            continue;
        const bool interactive =
            qobject_cast<QAbstractButton*>(widget) != nullptr ||
            qobject_cast<QLineEdit*>(widget) != nullptr ||
            qobject_cast<QComboBox*>(widget) != nullptr ||
            qobject_cast<QSlider*>(widget) != nullptr ||
            qobject_cast<QAbstractItemView*>(widget) != nullptr ||
            qobject_cast<QTabWidget*>(widget) != nullptr;
        if (!interactive)
            continue;
        QString name = widget->accessibleName();
        if (name.isEmpty())
        {
            if (auto* button = qobject_cast<QAbstractButton*>(widget))
                name = button->text();
        }
        if (name.trimmed().isEmpty())
        {
            problems << QStringLiteral("%1 without a name")
                            .arg(widget->metaObject()->className());
        }
    }
    for (QAbstractButton* button : dialog.findChildren<QAbstractButton*>())
    {
        if (!button->isVisibleTo(&dialog))
            continue;
        if (button->height() < 24)
        {
            problems << QStringLiteral("%1 is %2 px high")
                            .arg(button->text())
                            .arg(button->height());
        }
    }
    REQUIRE_EQ(problems.join(QStringLiteral(", ")), QString());
}

vss::gui::setup_facts windows_facts()
{
    vss::gui::setup_facts facts;
    facts.platform = QStringLiteral("windows");
    facts.com0com_found = false;
    facts.com0com_pairs = 0;
    facts.serial_devices = 2;
    facts.elevation_known = true;
    facts.elevated = false;
    return facts;
}

support::platform_facts platform_facts()
{
    support::platform_facts facts;
    facts.elevation_known = true;
    facts.elevated = false;
    facts.pty_known = true;
    facts.pty_available = true;
    facts.dialout_known = true;
    facts.dialout_member = false;
    return facts;
}

const support::setup_row* find_row(const support::setup_report& report,
                                   const QString& title)
{
    for (const support::setup_row& row : report.rows)
    {
        if (row.title == title)
            return &row;
    }
    return nullptr;
}

vss::gui::port_view make_port(vss::endpoint_id id, const QString& name,
                              const QString& path, vss::endpoint_kind kind)
{
    vss::gui::port_view port;
    port.id = id;
    port.name = name;
    port.path = path;
    port.kind = kind;
    port.state = vss::endpoint_state::open;
    port.open = true;
    return port;
}

std::uint16_t read_u16(const QByteArray& data, int offset)
{
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data.at(offset)) |
        (static_cast<unsigned char>(data.at(offset + 1)) << 8));
}

std::uint32_t read_u32(const QByteArray& data, int offset)
{
    return static_cast<std::uint32_t>(
        static_cast<unsigned char>(data.at(offset)) |
        (static_cast<unsigned char>(data.at(offset + 1)) << 8) |
        (static_cast<unsigned char>(data.at(offset + 2)) << 16) |
        (static_cast<unsigned char>(data.at(offset + 3)) << 24));
}

struct zip_entry
{
    QString name;
    QByteArray data;
};

/// A tiny independent reader, so the test proves the archive structure the
/// same way a ZIP tool walks the central directory.
QVector<zip_entry> read_zip(const QByteArray& archive)
{
    QVector<zip_entry> entries;
    int eocd = -1;
    for (int offset = static_cast<int>(archive.size()) - 22; offset >= 0;
         --offset)
    {
        if (read_u32(archive, offset) == 0x06054b50U)
        {
            eocd = offset;
            break;
        }
    }
    if (eocd < 0)
        return entries;
    const int count = read_u16(archive, eocd + 10);
    int cursor = static_cast<int>(read_u32(archive, eocd + 16));
    for (int index = 0; index < count; ++index)
    {
        if (read_u32(archive, cursor) != 0x02014b50U)
            break;
        const int name_length = read_u16(archive, cursor + 28);
        const int extra_length = read_u16(archive, cursor + 30);
        const int comment_length = read_u16(archive, cursor + 32);
        const int local_offset =
            static_cast<int>(read_u32(archive, cursor + 42));
        const QString name =
            QString::fromUtf8(archive.mid(cursor + 46, name_length));

        const int local_name = read_u16(archive, local_offset + 26);
        const int local_extra = read_u16(archive, local_offset + 28);
        const int size = static_cast<int>(read_u32(archive, local_offset + 18));
        const int data_offset = local_offset + 30 + local_name + local_extra;
        entries.push_back({name, archive.mid(data_offset, size)});

        cursor += 46 + name_length + extra_length + comment_length;
    }
    return entries;
}

} // namespace

TEST_CASE("support: the version string is composed from its parts")
{
    using support::compose_version;
    REQUIRE_EQ(compose_version(QStringLiteral("0.10.0-beta.1"),
                               QStringLiteral("6083"),
                               QStringLiteral("3de4617"), false),
               QStringLiteral("v0.10.0-beta.1+build.6083-3de4617"));
    REQUIRE_EQ(compose_version(QStringLiteral("0.1.0"), QString(),
                               QStringLiteral("3de4617"), false),
               QStringLiteral("v0.1.0+3de4617"));
    REQUIRE_EQ(compose_version(QStringLiteral("0.1.0-dev"),
                               QStringLiteral("12"), QString(), false),
               QStringLiteral("v0.1.0-dev+build.12"));
    REQUIRE_EQ(compose_version(QStringLiteral("0.1.0"), QString(),
                               QStringLiteral("unknown"), false),
               QStringLiteral("v0.1.0"));
    REQUIRE_EQ(compose_version(QStringLiteral("0.1.0-rc.3"),
                               QStringLiteral("7"), QStringLiteral("abc1234"),
                               true),
               QStringLiteral("v0.1.0-rc.3+build.7-abc1234-dirty"));

    // This binary's own string is built the same way from the stamp.
    const support::build_info info = support::current_build_info();
    REQUIRE_EQ(info.full_version(), support::current_version_string());
    REQUIRE(info.full_version().startsWith(QStringLiteral("v") + info.version));
    static const QRegularExpression semver(
        QStringLiteral("^\\d+\\.\\d+\\.\\d+(-[0-9A-Za-z.-]+)?$"));
    REQUIRE(semver.match(info.version).hasMatch());
}

TEST_CASE("support: build information and environment are separate reports")
{
    const support::build_info info = support::current_build_info();
    REQUIRE_EQ(info.qt_runtime, QString::fromLatin1(qVersion()));
    REQUIRE(!info.operating_system.isEmpty());
    REQUIRE(!info.profiling.isEmpty());
    REQUIRE_EQ(info.commit_full.left(info.commit.size()), info.commit);

    QStringList build_labels;
    for (const support::environment_row& row :
         support::build_information_rows(info))
        build_labels << row.label;
    for (const char* label :
         {"Version", "Git tag", "Git revision", "Short revision",
          "Working tree", "Branch", "Build number", "Built", "Build type",
          "Compiler", "Qt built with", "Boost"})
        REQUIRE(build_labels.contains(QString::fromLatin1(label)));

    QStringList environment_labels;
    for (const support::environment_row& row : support::environment_rows(info))
        environment_labels << row.label;
    for (const char* label :
         {"Operating system", "Architecture", "Kernel", "Qt running",
          "Platform plugin", "Display", "Renderer", "Language", "Profiling"})
        REQUIRE(environment_labels.contains(QString::fromLatin1(label)));
    for (const QString& label : build_labels)
        REQUIRE(!environment_labels.contains(label));

    const QString build = support::build_information_report(info);
    const QString environment = support::environment_report(info);
    const QString both = support::diagnostic_report(info);
    REQUIRE(build.contains(QStringLiteral("Version: %1").arg(info.full_version())));
    REQUIRE(build.contains(QStringLiteral("Git revision: ")));
    REQUIRE(!build.contains(QStringLiteral("Operating system:")));
    REQUIRE(environment.contains(QStringLiteral("Operating system: ")));
    REQUIRE(environment.contains(QStringLiteral("Profiling: ")));
    REQUIRE(!environment.contains(QStringLiteral("Git revision:")));
    REQUIRE(both.contains(build));
    REQUIRE(both.contains(environment));
    REQUIRE(both.indexOf(build) < both.indexOf(environment));

    // A fixed identity prints as it should, including a build outside CI.
    support::build_info fixed = info;
    fixed.version = QStringLiteral("0.10.0-beta.1");
    fixed.tag = QStringLiteral("v0.10.0-beta.1");
    fixed.commits_since_tag = 3;
    fixed.commit = QStringLiteral("3de4617");
    fixed.commit_full = QStringLiteral("3de4617aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    fixed.branch = QStringLiteral("develop");
    fixed.dirty = true;
    fixed.build_number = QString();
    const QString text = support::build_information_report(fixed);
    REQUIRE(text.contains(QStringLiteral("Version: v0.10.0-beta.1+3de4617-dirty")));
    REQUIRE(text.contains(QStringLiteral("Git tag: v0.10.0-beta.1, 3 commits later")));
    REQUIRE(text.contains(QStringLiteral("Working tree: modified")));
    REQUIRE(text.contains(QStringLiteral("Branch: develop")));
    REQUIRE(text.contains(QStringLiteral("Build number: none (not a CI build)")));
}

namespace
{

QString read_repository_file(const QString& relative)
{
    QFile file(QStringLiteral(VSS_SOURCE_DIR "/") + relative);
    REQUIRE(file.open(QIODevice::ReadOnly | QIODevice::Text));
    return QString::fromUtf8(file.readAll());
}

/// The component names in the first column of the "Linked or built against"
/// table of THIRD_PARTY_NOTICES.md.
QStringList notice_components()
{
    const QString notices = read_repository_file(
        QStringLiteral("THIRD_PARTY_NOTICES.md"));
    const qsizetype start = notices.indexOf(QStringLiteral("## Linked or built against"));
    const qsizetype end = notices.indexOf(QStringLiteral("\n## "), start + 3);
    REQUIRE(start >= 0);
    QStringList names;
    static const QRegularExpression row(
        QStringLiteral("^\\| \\[?([^\\]|]+?)\\]?(\\([^)]*\\))? \\|"),
        QRegularExpression::MultilineOption);
    const QString table = notices.mid(start, end - start);
    for (const QRegularExpressionMatch& match : row.globalMatch(table))
    {
        const QString name = match.captured(1).trimmed();
        if (name != QStringLiteral("Component") && !name.startsWith(QLatin1Char('-')))
            names << name;
    }
    return names;
}

} // namespace

TEST_CASE("support: licences match the bundled components")
{
    const QVector<support::third_party_component> all =
        support::third_party_components();
    REQUIRE(!all.isEmpty());

    // third_party.json and the notices table list the same components.
    QStringList inventory;
    for (const support::third_party_component& component : all)
    {
        inventory << component.notice;
        REQUIRE(!component.name.isEmpty());
        REQUIRE(!component.spdx.isEmpty());
        REQUIRE(!component.shipped.isEmpty());
        // Every licence has wording in the tab, not just its SPDX name.
        REQUIRE(support::licence_text(component.spdx) != component.spdx);
    }
    QStringList notices = notice_components();
    inventory.sort();
    notices.sort();
    REQUIRE_EQ(inventory.join(QLatin1Char(',')), notices.join(QLatin1Char(',')));

    // Every vcpkg dependency of the program is in the inventory.
    const QJsonObject manifest =
        QJsonDocument::fromJson(
            read_repository_file(QStringLiteral("vcpkg.json")).toUtf8())
            .object();
    for (const auto& dependency : manifest.value(QLatin1String("dependencies")).toArray())
    {
        const QString package = dependency.isString()
                                    ? dependency.toString()
                                    : dependency.toObject().value(QLatin1String("name")).toString();
        bool listed = false;
        for (const support::third_party_component& component : all)
            listed = listed || component.vcpkg == package;
        REQUIRE(listed);
    }

    // Qt is shipped because the package deploys it; Tracy only when it is
    // compiled in, and the release presets do not use vcpkg.
    REQUIRE(read_repository_file(QStringLiteral("apps/vss_gui/CMakeLists.txt"))
                .contains(QStringLiteral("qt_generate_deploy_app_script")));
    for (const support::third_party_component& component : all)
    {
        if (component.vcpkg.isEmpty())
            REQUIRE(component.condition.isEmpty());
        else
            REQUIRE_EQ(component.condition, QStringLiteral("profiling"));
    }

    // A bundled font names its family, licence, licence text and source, and
    // the licence text is in the repository.
    for (const support::third_party_font& font : support::third_party_fonts())
    {
        REQUIRE(!font.family.isEmpty());
        REQUIRE(!font.spdx.isEmpty());
        REQUIRE(!font.url.isEmpty());
        REQUIRE(!read_repository_file(font.licence_text).isEmpty());
        REQUIRE(support::licence_text(font.spdx) != font.spdx);
    }

    // A component that is only installed separately is never listed.
    for (const QString& name : support::third_party_not_shipped())
    {
        REQUIRE(!inventory.contains(name));
        for (const support::third_party_component& component :
             support::bundled_components(true))
            REQUIRE(component.name.compare(name, Qt::CaseInsensitive) != 0);
    }
    REQUIRE(support::third_party_not_shipped().contains(QStringLiteral("com0com")));

    // The tab shows the unconditional components, plus Tracy only in a build
    // that has the profiler.
    REQUIRE_EQ(support::bundled_components(false).size(), all.size() - 1);
    REQUIRE_EQ(support::bundled_components(true).size(), all.size());
    support::AboutDialog dialog;
    QTabWidget* tabs = dialog.tabs();
    QWidget* licences = tabs->widget(3);
    QStringList shown;
    for (QLabel* label : licences->findChildren<QLabel*>())
        shown << label->text();
    const QString shown_text = shown.join(QLatin1Char('\n'));
    for (const support::third_party_component& component :
         support::bundled_components(support::profiling_compiled()))
        REQUIRE(shown_text.contains(component.name));
    REQUIRE(!shown_text.contains(QStringLiteral("com0com")));
    REQUIRE_EQ(shown_text.contains(QStringLiteral("Tracy")),
               support::profiling_compiled());
}

TEST_CASE("support: redaction removes credentials and keeps JSON valid")
{
    const QString home = QDir::homePath();
    const QString text =
        QStringLiteral("connect tcp://user:secret@10.0.0.1:4001\r\n"
                       "password=hunter2\r\n"
                       "Authorization: Bearer abc.def.ghi\r\n"
                       "path %1/work")
            .arg(home);
    const QString redacted = support::redact_secrets(text);
    REQUIRE(!redacted.contains(QStringLiteral("secret")));
    REQUIRE(!redacted.contains(QStringLiteral("hunter2")));
    REQUIRE(!redacted.contains(QStringLiteral("abc.def.ghi")));
    REQUIRE(redacted.contains(QStringLiteral("[redacted]")));
    if (home.size() > 1)
        REQUIRE(redacted.contains(QStringLiteral("~/work")));

    const QString json =
        support::redact_secrets(QStringLiteral("{\"token\": \"abc123\"}"));
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(json.toUtf8(), &error);
    REQUIRE_EQ(error.error, QJsonParseError::NoError);
    REQUIRE(!json.contains(QStringLiteral("abc123")));
}

TEST_CASE(
    "support: stored zip writer has valid headers and a python round trip")
{
    REQUIRE_EQ(support::StoredZipWriter::crc32(QByteArray("123456789")),
               0xCBF43926U);
    REQUIRE_EQ(support::StoredZipWriter::crc32(QByteArray()), 0U);

    support::StoredZipWriter writer;
    REQUIRE(writer.add_file(QStringLiteral("a.txt"), QByteArray("hello")));
    QByteArray binary;
    for (int value = 0; value < 256; ++value)
        binary.append(static_cast<char>(value));
    REQUIRE(writer.add_file(QStringLiteral("dir/b.bin"), binary));
    REQUIRE_EQ(writer.file_count(), 2);

    const QByteArray archive = writer.data();
    REQUIRE(archive.size() > 80);
    REQUIRE_EQ(read_u32(archive, 0), 0x04034b50U);

    const QVector<zip_entry> entries = read_zip(archive);
    REQUIRE_EQ(entries.size(), 2);
    REQUIRE_EQ(entries.at(0).name, QStringLiteral("a.txt"));
    REQUIRE_EQ(entries.at(0).data, QByteArray("hello"));
    REQUIRE_EQ(entries.at(1).name, QStringLiteral("dir/b.bin"));
    REQUIRE_EQ(entries.at(1).data, binary);
    REQUIRE_EQ(support::StoredZipWriter::crc32(entries.at(1).data),
               0x29058C73U);

    QString python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (python.isEmpty())
        python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty())
        VSS_SKIP("python is not on PATH, so the zipfile round trip is skipped");

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("bundle.zip"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly));
    REQUIRE_EQ(file.write(archive), archive.size());
    file.close();

    const QString script = QStringLiteral("import sys, zipfile\n"
                                          "z = zipfile.ZipFile(sys.argv[1])\n"
                                          "assert z.testzip() is None\n"
                                          "print(len(z.namelist()))\n"
                                          "print(z.read('a.txt').decode())\n"
                                          "print(z.read('dir/b.bin')[255])\n");
    QProcess process;
    process.start(python, {QStringLiteral("-c"), script, path});
    REQUIRE(process.waitForFinished(30000));
    REQUIRE_EQ(process.exitStatus(), QProcess::NormalExit);
    REQUIRE_EQ(process.exitCode(), 0);
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    REQUIRE(output.contains(QStringLiteral("2")));
    REQUIRE(output.contains(QStringLiteral("hello")));
    REQUIRE(output.contains(QStringLiteral("255")));
}

TEST_CASE("support: setup report degrades and never fakes a check")
{
    const vss::gui::setup_facts facts = windows_facts();
    const support::platform_facts platform = platform_facts();
    const QStringList devices = {QStringLiteral("COM3"),
                                 QStringLiteral("COM4")};

    const support::setup_report missing =
        support::build_setup_report(facts, platform, devices);
    REQUIRE_EQ(missing.state,
               support::setup_report::readiness::needs_attention);
    REQUIRE(!missing.ports_ready);
    REQUIRE_EQ(missing.header_chip, QStringLiteral("1 thing to fix"));
    REQUIRE(!missing.driver_note.isEmpty());
    REQUIRE_EQ(missing.rows.size(), 4);
    REQUIRE_EQ(missing.rows.at(0).chip, QStringLiteral("Needs attention"));
    REQUIRE_EQ(missing.rows.at(0).action_text,
               QStringLiteral("Install com0com"));
    REQUIRE(missing.rows.at(0).sentence.contains(QStringLiteral(
        "Neither the Virtual Serial Studio driver nor com0com was found")));
    REQUIRE_EQ(missing.rows.at(1).chip, QStringLiteral("Needed later"));
    REQUIRE_EQ(missing.rows.at(2).chip, QStringLiteral("OK"));
    REQUIRE(missing.rows.at(2).sentence.contains(QStringLiteral("COM3")));
    REQUIRE(missing.rows.at(3).chip.isEmpty());

    vss::gui::setup_facts present = facts;
    present.com0com_found = true;
    present.com0com_pairs = 1;
    const support::setup_report ready =
        support::build_setup_report(present, platform, devices);
    REQUIRE_EQ(ready.state, support::setup_report::readiness::ready);
    REQUIRE(ready.ports_ready);
    REQUIRE_EQ(ready.header_chip, QStringLiteral("All set"));
    REQUIRE(ready.driver_note.isEmpty());
    REQUIRE_EQ(ready.rows.at(0).chip, QStringLiteral("OK"));

    // The Studio driver alone is enough: no com0com, no pair count.
    vss::gui::setup_facts studio = facts;
    studio.driver_found = true;
    studio.driver_version = QStringLiteral("2");
    const support::setup_report studio_report =
        support::build_setup_report(studio, platform, devices);
    REQUIRE_EQ(studio_report.state, support::setup_report::readiness::ready);
    REQUIRE(studio_report.ports_ready);
    REQUIRE_EQ(studio_report.header_chip, QStringLiteral("All set"));
    REQUIRE(studio_report.driver_note.isEmpty());
    REQUIRE_EQ(studio_report.rows.at(0).chip, QStringLiteral("OK"));
    REQUIRE(studio_report.rows.at(0).action_text.isEmpty());
    REQUIRE_EQ(studio_report.rows.at(0).sentence,
               QStringLiteral("The Virtual Serial Studio driver is installed, "
                              "so other programs can see ports Studio makes as "
                              "COM ports. Driver version 2."));
    REQUIRE_EQ(
        find_row(studio_report, QStringLiteral("Administrator rights"))->chip,
        QStringLiteral("OK"));
    REQUIRE(find_row(studio_report, QStringLiteral("Administrator rights"))
                ->sentence.contains(QStringLiteral("without administrator")));

    // An unreadable version is left out rather than guessed.
    vss::gui::setup_facts no_version = studio;
    no_version.driver_version.clear();
    REQUIRE(!support::build_setup_report(no_version, platform, devices)
                 .rows.at(0)
                 .sentence.contains(QStringLiteral("version")));

    // With both installed the row says which one Studio tries first.
    vss::gui::setup_facts both = studio;
    both.com0com_found = true;
    both.com0com_pairs = 2;
    const support::setup_report both_report =
        support::build_setup_report(both, platform, devices);
    REQUIRE(both_report.ports_ready);
    REQUIRE(both_report.rows.at(0).sentence.contains(
        QStringLiteral("com0com is also installed, and Studio tries its own "
                       "driver first.")));

    // Linux has no Windows driver row, whatever the facts say.
    vss::gui::setup_facts linux_driver = studio;
    linux_driver.platform = QStringLiteral("linux");
    REQUIRE_EQ(support::build_setup_report(linux_driver, platform, devices)
                   .rows.size(),
               3);

    support::platform_facts unknown_platform = platform;
    unknown_platform.elevation_known = false;
    const support::setup_report unknown_report =
        support::build_setup_report(facts, unknown_platform, devices);
    REQUIRE_EQ(
        find_row(unknown_report, QStringLiteral("Administrator rights"))->chip,
        QStringLiteral("Not checked yet"));

    const support::setup_report degraded =
        support::build_setup_report(facts, platform, devices, false);
    REQUIRE_EQ(degraded.state, support::setup_report::readiness::not_checked);
    REQUIRE_EQ(degraded.header_chip, QStringLiteral("Not checked yet"));
    REQUIRE(!degraded.ports_ready);
    REQUIRE_EQ(degraded.rows.at(0).chip, QStringLiteral("Not checked yet"));

    vss::gui::setup_facts linux = facts;
    linux.platform = QStringLiteral("linux");
    linux.com0com_found = false;
    const support::setup_report linux_report =
        support::build_setup_report(linux, platform, devices);
    REQUIRE_EQ(linux_report.rows.size(), 3);
    REQUIRE(linux_report.ports_ready);
    REQUIRE_EQ(linux_report.rows.at(0).chip, QStringLiteral("OK"));
    REQUIRE_EQ(linux_report.rows.at(1).chip, QStringLiteral("Needs attention"));
    REQUIRE_EQ(linux_report.rows.at(1).action_text,
               QStringLiteral("How to add me"));
    REQUIRE_EQ(linux_report.header_chip, QStringLiteral("1 thing to fix"));

    support::platform_facts no_dialout = platform;
    no_dialout.dialout_known = false;
    const support::setup_report unknown_group =
        support::build_setup_report(linux, no_dialout, devices);
    REQUIRE_EQ(unknown_group.rows.at(1).chip,
               QStringLiteral("Not checked yet"));
}

TEST_CASE("support: the setup report cache asks the backend only when stale")
{
    // The report asks the operating system about the driver, the devices and
    // the firewall, which takes tens of milliseconds. The status bar redraws
    // with every batch of traffic, so it reuses the report while it is young.
    FakeStudioBackend backend;
    backend.set_facts(windows_facts());
    support::setup_report_cache cache;

    // A backend that answers zero is asked every time: a test, or any
    // backend with nothing slow behind it, sees a change at once.
    REQUIRE_EQ(cache.get(backend).header_chip,
               QStringLiteral("1 thing to fix"));
    vss::gui::setup_facts present = windows_facts();
    present.com0com_found = true;
    backend.set_facts(present);
    REQUIRE_EQ(cache.get(backend).header_chip, QStringLiteral("All set"));

    // A backend with something slow behind it is asked once in a while.
    backend.set_probe_cache_age(std::chrono::hours(1));
    cache.invalidate();
    REQUIRE_EQ(cache.get(backend).header_chip, QStringLiteral("All set"));
    backend.set_facts(windows_facts());
    REQUIRE_EQ(cache.get(backend).header_chip, QStringLiteral("All set"));
    cache.invalidate();
    REQUIRE_EQ(cache.get(backend).header_chip,
               QStringLiteral("1 thing to fix"));
}

TEST_CASE("support: setup check dialog reads engine facts")
{
    FakeStudioBackend backend;
    backend.set_facts(windows_facts());
    backend.set_devices({{QStringLiteral("COM3"),
                          QStringLiteral("Serial"),
                          {},
                          false,
                          false,
                          -1,
                          {}},
                         {QStringLiteral("COM4"),
                          QStringLiteral("Serial"),
                          {},
                          false,
                          false,
                          -1,
                          {}}});

    support::SetupCheckDialog dialog(backend);
    // On a Windows host the Setup check also reads the real firewall, which
    // adds its row; the rows built from the injected facts are the four.
    const int firewall_rows =
        support::host_platform() == QLatin1String("windows") ? 1 : 0;
    REQUIRE_EQ(dialog.row_count(), 4 + firewall_rows);
    REQUIRE_EQ(dialog.report().header_chip, QStringLiteral("1 thing to fix"));
    REQUIRE_EQ(dialog.row_widget(0)->title_text(),
               QStringLiteral("Virtual port driver"));
    check_accessible(dialog);

    vss::gui::setup_facts present = windows_facts();
    present.com0com_found = true;
    backend.set_facts(present);
    dialog.recheck();
    REQUIRE_EQ(dialog.report().header_chip, QStringLiteral("All set"));

    vss::gui::setup_facts studio = windows_facts();
    studio.driver_found = true;
    studio.driver_version = QStringLiteral("2");
    backend.set_facts(studio);
    dialog.recheck();
    REQUIRE_EQ(dialog.report().header_chip, QStringLiteral("All set"));
    REQUIRE(dialog.report().ports_ready);
    check_accessible(dialog);

    vss::gui::capabilities caps;
    caps.setup_check = false;
    backend.set_capabilities(caps);
    dialog.recheck();
    REQUIRE_EQ(dialog.report().state,
               support::setup_report::readiness::not_checked);
    REQUIRE_EQ(dialog.report().header_chip, QStringLiteral("Not checked yet"));
    REQUIRE_EQ(dialog.row_widget(0)->title_text(),
               QStringLiteral("Virtual port driver"));
}

TEST_CASE("support: troubleshooting checklist reports each state honestly")
{
    const auto now = vss::clock_type::now();
    vss::gui::port_view terminal =
        make_port(2, QStringLiteral("terminal"), QStringLiteral("COM7"),
                  vss::endpoint_kind::virtual_port);
    vss::gui::port_view device =
        make_port(1, QStringLiteral("device"), QStringLiteral("COM3"),
                  vss::endpoint_kind::physical);
    device.stats.bytes_rx = 120;
    device.stats.started = now - std::chrono::seconds(120);
    device.stats.last_activity = now - std::chrono::seconds(10);

    vss::gui::link_view link;
    link.id = 1;
    link.shape = vss::gui::link_shape::split;
    link.mode = vss::link_mode::duplex;
    link.sources = {1};
    link.targets = {2};

    support::troubleshoot_facts facts;
    facts.port = 2;
    facts.ports = {device, terminal};
    facts.links = {link};
    facts.now = now;

    support::troubleshoot_report report =
        support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.size(), 5);
    REQUIRE_EQ(report.rows.at(0).state, support::check_state::passed);
    REQUIRE_EQ(report.rows.at(0).sentence,
               QStringLiteral("terminal is open on COM7."));
    REQUIRE_EQ(report.rows.at(1).sentence,
               QStringLiteral("terminal is in a split link from device."));
    REQUIRE_EQ(report.rows.at(2).sentence,
               QStringLiteral("device received data in the last minute."));
    REQUIRE_EQ(report.rows.at(3).state, support::check_state::not_checked);
    REQUIRE(report.rows.at(3).sentence.contains(QStringLiteral("cannot tell")));
    REQUIRE_EQ(report.rows.at(4).state, support::check_state::not_checked);
    REQUIRE_EQ(report.rows.at(4).sentence,
               QStringLiteral("Checked once a program connects."));

    // The engine cannot see a program: checks 4 and 5 say so.
    facts.peer_open_known = false;
    report = support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.at(3).chip, QStringLiteral("Not checked yet"));

    facts.peer_open_known = true;
    vss::gui::port_view not_open = terminal;
    not_open.peer = vss::gui::peer_state::not_open;
    facts.ports = {device, not_open};
    report = support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.at(3).state, support::check_state::attention);
    REQUIRE(report.rows.at(3).highlight);
    REQUIRE_EQ(report.rows.at(3).sentence,
               QStringLiteral("No program has opened COM7 yet, so nothing can "
                              "arrive. The program has to open exactly COM7; "
                              "a different COM port will not receive "
                              "anything."));

    vss::gui::port_view open = terminal;
    open.peer = vss::gui::peer_state::open;
    facts.ports = {device, open};
    report = support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.at(3).state, support::check_state::passed);
    REQUIRE(
        report.rows.at(4).sentence.contains(QStringLiteral("read the data")));

    // A closed port and no link.
    vss::gui::port_view closed = terminal;
    closed.open = false;
    closed.state = vss::endpoint_state::closed;
    facts.ports = {device, closed};
    facts.links.clear();
    report = support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.at(0).state, support::check_state::attention);
    REQUIRE_EQ(report.rows.at(1).state, support::check_state::attention);
    REQUIRE_EQ(
        report.rows.at(1).sentence,
        QStringLiteral("This port has no link. Connect it to a device."));

    // The device has been quiet for more than a minute.
    facts.links = {link};
    vss::gui::port_view quiet = device;
    quiet.stats.last_activity = now - std::chrono::seconds(120);
    facts.ports = {quiet, open};
    report = support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.at(2).state, support::check_state::attention);
    REQUIRE_EQ(report.rows.at(2).sentence,
               QStringLiteral("device has not sent anything in the last "
                              "minute. Check the cable and the device's "
                              "power."));

    // Nothing selected at all.
    facts.port = 0;
    facts.ports.clear();
    facts.links.clear();
    report = support::build_troubleshoot_report(facts);
    REQUIRE_EQ(report.rows.size(), 5);
    for (int index = 0; index < 3; ++index)
        REQUIRE_EQ(report.rows.at(index).state,
                   support::check_state::not_checked);
}

TEST_CASE("support: troubleshooting dialog follows the selected port")
{
    FakeStudioBackend backend;
    const auto terminal = backend.create_virtual_port(
        QStringLiteral("terminal"), vss::virtual_port_kind::loopback, true);
    REQUIRE(terminal.has_value());
    const auto device = backend.create_mock_port(QStringLiteral("device"),
                                                 vss::serial_settings{},
                                                 QStringLiteral("scripted"));
    REQUIRE(device.has_value());
    const auto link = backend.connect_ports(*device, *terminal, {});
    REQUIRE(link.has_value());

    support::TroubleshootDialog dialog(backend, *terminal);
    REQUIRE_EQ(dialog.check_count(), 5);
    REQUIRE(dialog.title_text().contains(QStringLiteral("loopback:terminal")));
    REQUIRE_EQ(dialog.port_path(), QStringLiteral("loopback:terminal"));
    REQUIRE_EQ(dialog.check_widget(0)->title_text(),
               QStringLiteral("The port exists and is open"));
    REQUIRE_EQ(dialog.check_widget(1)->title_text(),
               QStringLiteral("The port is linked to the device"));
    REQUIRE_EQ(dialog.check_widget(4)->title_text(),
               QStringLiteral("Data reaches the program"));
    check_accessible(dialog);

    vss::gui::capabilities caps;
    caps.peer_open = true;
    backend.set_capabilities(caps);
    backend.set_peer_state(*terminal, vss::gui::peer_state::not_open);
    dialog.refresh_checks();
    REQUIRE_EQ(dialog.check_count(), 5);
}

TEST_CASE("support: settings persist and apply immediately")
{
    clear_settings();
    vss::gui::Theme::instance().set_mode(theme_mode::system);
    vss::gui::Theme::instance().set_density(density_mode::comfortable);
    FakeStudioBackend backend;

    support::SettingsDialog dialog(backend, support::load_ui_settings());
    bool advanced_signal = false;
    QObject::connect(&dialog, &support::SettingsDialog::advanced_changed,
                     [&advanced_signal](bool on) { advanced_signal = on; });

    auto* advanced =
        dialog.findChild<QCheckBox*>(QStringLiteral("advancedSwitch"));
    REQUIRE(advanced != nullptr);
    advanced->setChecked(true);
    REQUIRE(advanced_signal);
    REQUIRE(dialog.settings().advanced);
    REQUIRE(QSettings().value(QStringLiteral("ui/advanced")).toBool());

    QRadioButton* dark = radio_with_text(dialog, QStringLiteral("Dark"));
    REQUIRE(dark != nullptr);
    dark->setChecked(true);
    REQUIRE_EQ(vss::gui::Theme::instance().mode(), theme_mode::dark);
    REQUIRE_EQ(setting("ui/theme"), QStringLiteral("dark"));
    // The dialog's own nav chrome must follow a live theme switch.
    auto* nav = dialog.findChild<QFrame*>(QStringLiteral("settingsNav"));
    REQUIRE(nav != nullptr);
    REQUIRE(nav->styleSheet().contains(
        vss::gui::Theme::instance()
            .colour(vss::gui::colour_role::accentSoft)
            .name(QColor::HexRgb)));

    auto* density = dialog.findChild<vss::gui::SegmentedControl*>(
        QStringLiteral("densityControl"));
    REQUIRE(density != nullptr);
    density->set_current(1);
    REQUIRE_EQ(vss::gui::Theme::instance().density(), density_mode::compact);
    REQUIRE_EQ(setting("ui/density"), QStringLiteral("compact"));

    // Link lines default to curved, change at once, and are signalled so the
    // canvas can redraw.
    REQUIRE_EQ(support::default_ui_settings().links,
               vss::gui::link_style::curved);
    REQUIRE_EQ(dialog.settings().links, vss::gui::link_style::curved);
    auto* link_lines = dialog.findChild<vss::gui::SegmentedControl*>(
        QStringLiteral("linkStyleControl"));
    REQUIRE(link_lines != nullptr);
    vss::gui::link_style signalled = vss::gui::link_style::curved;
    QObject::connect(&dialog, &support::SettingsDialog::settings_changed,
                     [&signalled](const support::ui_settings& changed)
                     { signalled = changed.links; });
    link_lines->set_current(1);
    REQUIRE_EQ(signalled, vss::gui::link_style::elbow);
    REQUIRE_EQ(dialog.settings().links, vss::gui::link_style::elbow);
    REQUIRE_EQ(setting("ui/linkStyle"), QStringLiteral("elbow"));
    REQUIRE_EQ(support::load_ui_settings().links, vss::gui::link_style::elbow);

    auto* slider = dialog.findChild<QSlider*>(QStringLiteral("refreshSlider"));
    REQUIRE(slider != nullptr);
    slider->setValue(500);
    REQUIRE_EQ(dialog.settings().refresh_ms, 500);
    REQUIRE_EQ(setting("ui/refreshMs"), QStringLiteral("500"));
    REQUIRE_EQ(backend.refresh_interval_ms(), 500);

    auto* baud = dialog.findChild<QComboBox*>(QStringLiteral("baudCombo"));
    REQUIRE(baud != nullptr);
    baud->setCurrentText(QStringLiteral("9600"));
    REQUIRE_EQ(setting("ui/defaultBaud"), QStringLiteral("9600"));

    auto* ask = dialog.findChild<QCheckBox*>(QStringLiteral("askOnDiscard"));
    REQUIRE(ask != nullptr);
    ask->setChecked(false);
    REQUIRE_EQ(setting("ui/askOnDiscard"), QStringLiteral("false"));

    // A new dialog reads the saved values back.
    const support::ui_settings saved = support::load_ui_settings();
    REQUIRE(saved.advanced);
    REQUIRE_EQ(saved.theme, theme_mode::dark);
    REQUIRE_EQ(saved.density, density_mode::compact);
    REQUIRE_EQ(saved.links, vss::gui::link_style::elbow);
    REQUIRE_EQ(saved.refresh_ms, 500);
    REQUIRE_EQ(saved.default_baud, 9600);
    REQUIRE(!saved.ask_on_discard);

    support::SettingsDialog reopened(backend, saved);
    REQUIRE_EQ(reopened.settings().refresh_ms, 500);
    auto* reopened_dark = radio_with_text(reopened, QStringLiteral("Dark"));
    REQUIRE(reopened_dark != nullptr);
    REQUIRE(reopened_dark->isChecked());

    reopened.reset_to_defaults();
    REQUIRE_EQ(reopened.settings().advanced, false);
    REQUIRE_EQ(reopened.settings().refresh_ms, 250);
    REQUIRE_EQ(vss::gui::Theme::instance().mode(), theme_mode::system);
    REQUIRE_EQ(vss::gui::Theme::instance().density(),
               density_mode::comfortable);
    REQUIRE_EQ(backend.refresh_interval_ms(), 250);
    REQUIRE(!QSettings().value(QStringLiteral("ui/advanced")).toBool());
    REQUIRE_EQ(setting("ui/theme"), QStringLiteral("system"));
    REQUIRE_EQ(reopened.settings().links, vss::gui::link_style::curved);
    REQUIRE_EQ(setting("ui/linkStyle"), QStringLiteral("curved"));

    clear_settings();
}

TEST_CASE("support: diagnostic bundle contents honour the traffic opt-in")
{
    clear_settings();
    // Arrange: one loopback port that has carried "hello", so the traffic
    // file has something to show.
    FakeStudioBackend backend;
    const auto terminal = backend.create_virtual_port(
        QStringLiteral("terminal"), vss::virtual_port_kind::loopback, true);
    REQUIRE(terminal.has_value());
    REQUIRE(backend.send(*terminal, QByteArrayLiteral("hello")));

    // Two log events: a plain one that must survive, and one carrying a
    // credential-like token that the log file must redact.
    support::EventLog log;
    vss::gui::gui_event message;
    message.kind = vss::event_kind::log_message;
    message.name = QStringLiteral("terminal");
    message.message = QStringLiteral("opened");
    log.append(message);
    vss::gui::gui_event secret;
    secret.kind = vss::event_kind::log_message;
    secret.name = QStringLiteral("terminal");
    secret.message = QStringLiteral("token=abc123");
    log.append(secret);

    // By default traffic is off, so the bundle holds four files in a fixed
    // order.
    support::bundle_selection selection;
    REQUIRE_EQ(selection.file_count(), 4);
    QVector<support::bundle_file> files =
        support::build_bundle_files(backend, log, selection);
    REQUIRE_EQ(files.size(), 4);
    REQUIRE_EQ(files.at(0).name, QStringLiteral("setup.json"));
    REQUIRE_EQ(files.at(1).name, QStringLiteral("log.txt"));
    REQUIRE_EQ(files.at(2).name, QStringLiteral("environment.txt"));
    // environment.txt holds the build information and the environment.
    REQUIRE(files.at(2).data.contains("Git revision:"));
    REQUIRE(files.at(2).data.contains("Operating system:"));
    REQUIRE_EQ(files.at(3).name, QStringLiteral("counters.csv"));

    // Check each file's content: the setup is valid JSON with the one port,
    // the counters are CSV, and the log keeps "opened" but not the secret.
    QJsonParseError parse_error{};
    const QJsonDocument setup =
        QJsonDocument::fromJson(files.at(0).data, &parse_error);
    REQUIRE_EQ(parse_error.error, QJsonParseError::NoError);
    REQUIRE_EQ(setup.object().value(QStringLiteral("ports")).toArray().size(),
               1);
    REQUIRE(files.at(3).data.contains("terminal"));
    REQUIRE(files.at(3).data.contains("name,path,state"));
    REQUIRE(files.at(1).data.contains("opened"));
    REQUIRE(!files.at(1).data.contains("abc123"));
    REQUIRE(files.at(2).data.contains("Short revision:"));

    // Opting in to traffic adds a fifth file, placed after the others.
    selection.traffic = true;
    REQUIRE_EQ(selection.file_count(), 5);
    files = support::build_bundle_files(backend, log, selection);
    REQUIRE_EQ(files.size(), 5);
    REQUIRE_EQ(files.at(4).name, QStringLiteral("traffic.txt"));
    REQUIRE(files.at(4).data.contains("hello"));
    REQUIRE(files.at(4).data.contains("TX"));

    // The dialog starts from the same defaults, and ticking the traffic box
    // moves the file count both ways.
    support::DiagnosticBundleDialog dialog(backend, log);
    REQUIRE(dialog.available());
    REQUIRE_EQ(dialog.selected_file_count(), 4);
    REQUIRE(dialog.bundle_name().startsWith(QStringLiteral("vss-bundle-")));
    auto* traffic =
        dialog.findChild<QCheckBox*>(QStringLiteral("bundleTraffic"));
    REQUIRE(traffic != nullptr);
    REQUIRE(!traffic->isChecked());
    traffic->setChecked(true);
    REQUIRE_EQ(dialog.selected_file_count(), 5);
    traffic->setChecked(false);
    REQUIRE_EQ(dialog.selected_file_count(), 4);
    check_accessible(dialog);

    // Write real zip files and read them back: the archive must match the
    // checkbox state, with no traffic file unless it was chosen.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    const QString plain_path = directory.filePath(QStringLiteral("plain.zip"));
    QString error;
    REQUIRE(dialog.write_bundle(plain_path, &error));
    REQUIRE(error.isEmpty());
    QFile plain(plain_path);
    REQUIRE(plain.open(QIODevice::ReadOnly));
    const QVector<zip_entry> plain_entries = read_zip(plain.readAll());
    plain.close();
    REQUIRE_EQ(plain_entries.size(), 4);
    for (const zip_entry& entry : plain_entries)
        REQUIRE_NE(entry.name, QStringLiteral("traffic.txt"));

    // Same dialog, traffic opted in this time.
    traffic->setChecked(true);
    const QString traffic_path =
        directory.filePath(QStringLiteral("traffic.zip"));
    REQUIRE(dialog.write_bundle(traffic_path, &error));
    QFile with_traffic(traffic_path);
    REQUIRE(with_traffic.open(QIODevice::ReadOnly));
    const QVector<zip_entry> traffic_entries = read_zip(with_traffic.readAll());
    with_traffic.close();
    REQUIRE_EQ(traffic_entries.size(), 5);
    bool saw_traffic = false;
    for (const zip_entry& entry : traffic_entries)
    {
        if (entry.name == QStringLiteral("traffic.txt"))
            saw_traffic = true;
    }
    REQUIRE(saw_traffic);

    // A backend without diagnostics support: the dialog reports itself
    // unavailable and Save is disabled.
    vss::gui::capabilities caps;
    caps.diagnostics = false;
    backend.set_capabilities(caps);
    support::DiagnosticBundleDialog unavailable(backend, log);
    REQUIRE(!unavailable.available());
    auto* save =
        unavailable.findChild<QPushButton*>(QStringLiteral("saveBundle"));
    REQUIRE(save != nullptr);
    REQUIRE(!save->isEnabled());
}

TEST_CASE("support: traffic redaction covers the ascii preview")
{
    std::vector<vss::gui::traffic_view> traffic;
    vss::gui::traffic_view record;
    record.endpoint = 1;
    record.name = QStringLiteral("terminal");
    record.direction = vss::data_direction::tx;
    record.data = QByteArrayLiteral("password=swordfish");
    traffic.push_back(record);
    const QString rendered = support::render_traffic(traffic);
    REQUIRE(rendered.contains(QStringLiteral("TX")));
    REQUIRE(!rendered.contains(QStringLiteral("swordfish")));
    REQUIRE(rendered.contains(QStringLiteral("[redacted]")));

    const std::vector<vss::gui::traffic_view> empty;
    REQUIRE(support::render_traffic(empty).contains(
        QStringLiteral("No traffic recorded.")));
}

TEST_CASE("support: confirm dialog keeps the safe choice and cancels safely")
{
    ConfirmDialog destructive(QStringLiteral("Remove terminal?"),
                              QStringLiteral("Its link is removed too."),
                              QStringLiteral("Remove"),
                              QStringLiteral("Cancel"), true);
    REQUIRE_EQ(destructive.choice_count(), 2);
    REQUIRE(destructive.choice_button(0)->isDefault());
    REQUIRE_EQ(destructive.choice_button(1)->property("role").toString(),
               QStringLiteral("danger"));
    destructive.choice_button(1)->click();
    REQUIRE_EQ(destructive.result(), static_cast<int>(QDialog::Accepted));
    REQUIRE_EQ(destructive.chosen(), 1);

    ConfirmDialog unsaved(
        QStringLiteral("Save changes to lab.json?"),
        QStringLiteral("Your ports and links have changed."),
        QVector<confirm_choice>{
            {QStringLiteral("Don't save"), QStringLiteral("quiet"), false},
            {QStringLiteral("Cancel"), QString(), true},
            {QStringLiteral("Save"), QStringLiteral("primary"), false}},
        2);
    REQUIRE_EQ(unsaved.choice_count(), 3);
    REQUIRE(unsaved.choice_button(2)->isDefault());
    REQUIRE_EQ(unsaved.choice_button(2)->property("role").toString(),
               QStringLiteral("primary"));
    unsaved.choice_button(1)->click();
    REQUIRE_EQ(unsaved.result(), static_cast<int>(QDialog::Rejected));
    REQUIRE_EQ(unsaved.chosen(), -1);

    ConfirmDialog escape(QStringLiteral("Save changes?"),
                         QStringLiteral("Body"), QStringLiteral("Save"),
                         QStringLiteral("Cancel"), false);
    escape.show();
    QApplication::processEvents();
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&escape, &key);
    REQUIRE_EQ(escape.result(), static_cast<int>(QDialog::Rejected));
    REQUIRE_EQ(escape.chosen(), -1);

    for (int index = 0; index < unsaved.choice_count(); ++index)
    {
        REQUIRE(unsaved.choice_button(index)->sizeHint().height() >= 24);
        REQUIRE(!unsaved.choice_button(index)->text().isEmpty());
    }
}

TEST_CASE("support: shortcuts are generated, filtered and cross-platform")
{
    QVector<support::shortcut_entry> entries = {
        {QStringLiteral("New"), QKeySequence::New, false},
        {QStringLiteral("Open"), QKeySequence::Open, false},
        {QStringLiteral("Save"), QKeySequence::Save, false},
        {QStringLiteral("Remove selected"), QKeySequence(Qt::Key_Delete),
         false},
        {QStringLiteral("Help"), QKeySequence(Qt::Key_F1), false},
        {QStringLiteral("No shortcut"), QKeySequence(), false}};

    REQUIRE_EQ(support::ShortcutsDialog::windows_linux_text(QKeySequence::New),
               QStringLiteral("Ctrl+N"));
    REQUIRE_EQ(support::ShortcutsDialog::macos_text(QKeySequence::New),
               QStringLiteral("Cmd+N"));
    REQUIRE_EQ(support::ShortcutsDialog::windows_linux_text(
                   QKeySequence(Qt::Key_Delete)),
               QStringLiteral("Delete"));
    REQUIRE_EQ(support::ShortcutsDialog::macos_text(QKeySequence(Qt::Key_F1)),
               QStringLiteral("F1"));

    support::ShortcutsDialog dialog(entries);
    REQUIRE_EQ(dialog.total_row_count(), 5);
    REQUIRE_EQ(dialog.action_texts().size(), 5);
    dialog.set_filter(QStringLiteral("save"));
    REQUIRE_EQ(dialog.visible_row_count(), 1);
    dialog.set_filter(QString());
    REQUIRE_EQ(dialog.visible_row_count(), 5);
    dialog.set_filter(QStringLiteral("ctrl"));
    REQUIRE_EQ(dialog.visible_row_count(), 3);
    dialog.set_filter(QString());
    check_accessible(dialog);
}

TEST_CASE("support: About reports the build and environment and copies them")
{
    support::AboutDialog dialog;
    QTabWidget* tabs = dialog.tabs();
    REQUIRE(tabs != nullptr);
    REQUIRE_EQ(tabs->count(), 5);
    REQUIRE_EQ(tabs->tabText(0), QStringLiteral("About"));
    REQUIRE_EQ(tabs->tabText(1), QStringLiteral("Build information"));
    REQUIRE_EQ(tabs->tabText(2), QStringLiteral("Environment"));
    REQUIRE_EQ(tabs->tabText(3), QStringLiteral("Licences"));
    REQUIRE_EQ(tabs->tabText(4), QStringLiteral("Fonts"));

    // The header carries the single full version string.
    bool has_version = false;
    for (QLabel* label : dialog.findChildren<QLabel*>())
    {
        if (label->text().startsWith(QStringLiteral("Version ")) &&
            label->text().contains(support::current_version_string()))
            has_version = true;
    }
    REQUIRE(has_version);

    // The About tab offers the licence and a link to the source.
    bool has_source_link = false;
    for (QLabel* label : dialog.findChildren<QLabel*>())
    {
        if (label->text().contains(support::AboutDialog::source_url()) &&
            label->text().contains(QStringLiteral("href=")))
            has_source_link = true;
    }
    REQUIRE(has_source_link);
    REQUIRE(support::AboutDialog::source_url().startsWith(
        QStringLiteral("https://")));

    // Each copy button puts exactly its own text on the clipboard.
    REQUIRE(dialog.build_information_text().contains(QStringLiteral("Git revision:")));
    REQUIRE(dialog.environment_text().contains(QStringLiteral("Operating system:")));
    REQUIRE(!dialog.environment_text().contains(QStringLiteral("Git revision:")));
    const struct
    {
        const char* button;
        QString text;
    } copies[] = {
        {"Copy build information", dialog.build_information_text()},
        {"Copy environment", dialog.environment_text()},
        {"Copy diagnostic information", dialog.diagnostic_text()},
    };
    for (const auto& copy : copies)
    {
        QGuiApplication::clipboard()->clear();
        QPushButton* button =
            button_with_text(dialog, QString::fromLatin1(copy.button));
        REQUIRE(button != nullptr);
        button->click();
        REQUIRE_EQ(QGuiApplication::clipboard()->text(), copy.text);
    }
    REQUIRE_EQ(dialog.diagnostic_text(),
               dialog.build_information_text() + QLatin1Char('\n') +
                   dialog.environment_text());
    check_accessible(dialog);
}

TEST_CASE("support: event log keeps events, not payloads")
{
    support::EventLog log;
    REQUIRE(log.text().isEmpty());

    vss::gui::gui_event data;
    data.kind = vss::event_kind::data;
    data.name = QStringLiteral("terminal");
    data.direction = vss::data_direction::rx;
    data.data = QByteArrayLiteral("secretpayload");
    log.append(data);
    REQUIRE(log.text().contains(QStringLiteral("RX")));
    REQUIRE(log.text().contains(QStringLiteral("13 bytes")));
    REQUIRE(!log.text().contains(QStringLiteral("secretpayload")));

    vss::gui::gui_event stats;
    stats.kind = vss::event_kind::stats;
    const int before = log.size();
    log.append(stats);
    REQUIRE_EQ(log.size(), before);

    vss::gui::gui_event error;
    error.kind = vss::event_kind::error;
    error.name = QStringLiteral("terminal");
    error.error = QStringLiteral("device stopped");
    log.append(error);
    REQUIRE(log.text().contains(QStringLiteral("device stopped")));

    vss::gui::gui_event fault;
    fault.kind = vss::event_kind::bus_fault;
    fault.name = QStringLiteral("p1");
    fault.message = QStringLiteral("sent 8 bytes while p0 was sending");
    log.append(fault);
    REQUIRE(log.text().contains(
        QStringLiteral("p1 bus: sent 8 bytes while p0 was sending")));
}

TEST_CASE("support: help contents offers the support surfaces")
{
    support::HelpContentsDialog dialog;
    dialog.show();
    QApplication::processEvents();
    const auto buttons = dialog.findChildren<QPushButton*>();
    int open_buttons = 0;
    for (QPushButton* button : buttons)
    {
        if (button->text() == QStringLiteral("Open"))
            ++open_buttons;
    }
    REQUIRE_EQ(open_buttons, 4);
    REQUIRE_EQ(dialog.requested(), support::help_topic::none);
    check_accessible(dialog);

    support::HelpContentsDialog chooser;
    QPushButton* first = nullptr;
    for (QPushButton* button : chooser.findChildren<QPushButton*>())
    {
        if (button->text() == QStringLiteral("Open"))
        {
            first = button;
            break;
        }
    }
    REQUIRE(first != nullptr);
    first->click();
    REQUIRE_EQ(chooser.requested(), support::help_topic::setup_check);
}
