//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a support bundle holds and what it never holds. The backend is seeded
// with marker values, and the test looks for them in every byte the bundle
// would write.
//

#include "backend/fake_studio_backend.hpp"
#include "support/bundle.hpp"
#include "support/event_log.hpp"

#include "licence_samples.hpp"
#include "test_framework.hpp"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTimeZone>

#include <string>

using namespace vss::gui;
namespace lic = vss::license;
namespace sample = vss::gui::test_licence;

namespace
{

const QString computer_marker = QStringLiteral("MARKERHOST");
const QString user_marker = QStringLiteral("markeruser");
const QString serial_marker = QStringLiteral("SERIALMARK123");
const QString payload_marker = QStringLiteral("PAYLOADMARK");
const QString licensee_marker = QStringLiteral("Licensee Marker Pty Ltd");
const QString secret_marker = QStringLiteral("hunter2marker");

support::bundle_identity markers()
{
    return {computer_marker, user_marker};
}

QDateTime created()
{
    return QDateTime(QDate(2027, 6, 1), QTime(10, 30, 0), QTimeZone::UTC);
}

/// A backend and a log that carry every marker somewhere.
struct seeded
{
    FakeStudioBackend backend;
    support::EventLog log;

    seeded()
    {
        const auto port = backend.create_virtual_port(
            QStringLiteral("rig on ") + computer_marker,
            vss::virtual_port_kind::loopback, true);
        REQUIRE(port.has_value());
        REQUIRE(backend.send(*port, payload_marker.toUtf8()));

        device_view usb;
        usb.path = QStringLiteral("COM7");
        usb.description = QStringLiteral("USB Serial Port");
        usb.hardware_id =
            QStringLiteral("USB\\VID_0403&PID_6001\\") + serial_marker;
        device_view by_id;
        by_id.path = QStringLiteral("/dev/serial/by-id/usb-FTDI_FT232R_") +
                     serial_marker + QStringLiteral("-if00-port0");
        by_id.description = QStringLiteral("FT232R USB UART");
        by_id.hardware_id = QStringLiteral("usb:0403:6001:") + serial_marker;
        device_view made;
        made.path = QStringLiteral("COM21");
        made.description = QStringLiteral("Virtual Serial Studio port");
        made.hardware_id = QStringLiteral("VSSCOM\\PORT\\0001");
        made.virtual_device = true;
        backend.set_devices({usb, by_id, made});

        add_log(QStringLiteral("opened by ") + user_marker + QStringLiteral(
                                                                 " on ") +
                computer_marker);
        add_log(QStringLiteral("password=") + secret_marker);
        add_log(QStringLiteral("saved ") +
                QDir::toNativeSeparators(QDir::homePath()) +
                QStringLiteral("/setups/lab.json"));
        add_log(QDir::homePath() + QStringLiteral("/setups/lab.json"));

        lic::licence value = sample::annual();
        value.licensee_name = licensee_marker.toStdString();
        value.machines = {computer_marker.toStdString() + "*"};
        licence_view view =
            sample::view_of(sample::judge(value, "2027-01-01T00:00:00Z", {},
                                          computer_marker.toStdString()));
        view.computer_name = computer_marker;
        backend.set_licence(view);
    }

    void add_log(const QString& text)
    {
        gui_event event;
        event.kind = vss::event_kind::log_message;
        event.name = QStringLiteral("rig");
        event.message = text;
        log.append(event);
    }
};

QString all_bytes(const support::bundle_contents& contents)
{
    QString text;
    for (const support::bundle_file& file : contents.files())
        text += file.name + QLatin1Char('\n') +
                QString::fromUtf8(file.data) + QLatin1Char('\n');
    return text;
}

const support::bundle_file* file_named(const support::bundle_contents& contents,
                                       const QString& name)
{
    for (const support::bundle_section& part : contents.sections)
        for (const support::bundle_file& file : part.files)
            if (file.name == name)
                return &file;
    return nullptr;
}

QStringList manifest_ids(const support::bundle_contents& contents)
{
    QStringList ids;
    const QJsonObject manifest =
        QJsonDocument::fromJson(contents.manifest.data).object();
    for (const auto& entry : manifest.value(QLatin1String("sections")).toArray())
        ids << entry.toObject().value(QLatin1String("id")).toString();
    return ids;
}

} // namespace

TEST_CASE("support bundle: the default bundle holds nothing that names the "
          "person, the computer or a device")
{
    seeded s;
    REQUIRE(support::bundle_has_licence(s.backend));
    REQUIRE(support::bundle_licence_names_computers(s.backend));

    const support::bundle_contents contents = support::build_bundle(
        s.backend, s.log, support::bundle_selection{}, markers(), created());
    const QString bytes = all_bytes(contents);

    // The markers were seeded: the port name, the log, the devices and the
    // licence all carry them.
    REQUIRE(!bytes.contains(computer_marker, Qt::CaseInsensitive));
    REQUIRE(!bytes.contains(user_marker, Qt::CaseInsensitive));
    REQUIRE(!bytes.contains(serial_marker, Qt::CaseInsensitive));
    REQUIRE(!bytes.contains(payload_marker));
    REQUIRE(!bytes.contains(licensee_marker));
    REQUIRE(!bytes.contains(secret_marker));
    REQUIRE(!bytes.contains(QDir::homePath()));
    REQUIRE(!bytes.contains(QDir::toNativeSeparators(QDir::homePath())));
    // Nothing of a licence file: no envelope, no signature.
    REQUIRE(!bytes.contains(QStringLiteral("\"signature\"")));
    REQUIRE(!bytes.contains(QStringLiteral("\"payload\"")));

    // What took their place, and what a helper still gets.
    REQUIRE(bytes.contains(QStringLiteral("[computer]")));
    REQUIRE(bytes.contains(QStringLiteral("[user]")));
    REQUIRE(bytes.contains(QStringLiteral("[redacted]")));
    REQUIRE(bytes.contains(QStringLiteral("~")));
    const support::bundle_file* ports =
        file_named(contents, QStringLiteral("serial-ports.txt"));
    REQUIRE(ports != nullptr);
    const QString ports_text = QString::fromUtf8(ports->data);
    REQUIRE(ports_text.contains(
        QStringLiteral("COM7 | USB Serial Port | 0403:6001 | not known | "
                       "device")));
    REQUIRE(ports_text.contains(
        QStringLiteral("/dev/serial/by-id/[redacted] | FT232R USB UART | "
                       "0403:6001 | not known | device")));
    REQUIRE(ports_text.contains(
        QStringLiteral("COM21 | Virtual Serial Studio port | VSSCOM | not "
                       "known | virtual")));

    const support::bundle_file* licence =
        file_named(contents, QStringLiteral("licence.txt"));
    REQUIRE(licence != nullptr);
    const QString licence_text = QString::fromUtf8(licence->data);
    REQUIRE(licence_text.contains(
        QStringLiteral("Licence number: VSS-2026-000123")));
    REQUIRE(licence_text.contains(QStringLiteral("Edition in use: Pro")));
    REQUIRE(licence_text.contains(QStringLiteral("State: valid")));
    REQUIRE(licence_text.contains(QStringLiteral("Names computers: yes")));

    const support::bundle_file* environment =
        file_named(contents, QStringLiteral("environment.txt"));
    REQUIRE(environment != nullptr);
    REQUIRE(QString::fromUtf8(environment->data)
                .contains(QStringLiteral("Edition: Pro")));
}

TEST_CASE("support bundle: the manifest lists exactly the sections that "
          "were ticked")
{
    seeded s;
    const support::bundle_contents all = support::build_bundle(
        s.backend, s.log, support::bundle_selection{}, markers(), created());
    const QStringList every{
        QStringLiteral("environment"),  QStringLiteral("driver"),
        QStringLiteral("setup"),        QStringLiteral("log"),
        QStringLiteral("counters"),     QStringLiteral("serial-ports"),
        QStringLiteral("licence")};
    REQUIRE_EQ(manifest_ids(all).join(QLatin1Char(',')),
               every.join(QLatin1Char(',')));

    // The manifest is the first file, then the sections in order.
    const QVector<support::bundle_file> files = all.files();
    REQUIRE_EQ(files.size(), qsizetype{8});
    REQUIRE_EQ(files.at(0).name, QStringLiteral("manifest.json"));
    REQUIRE_EQ(files.at(1).name, QStringLiteral("environment.txt"));
    REQUIRE_EQ(files.at(7).name, QStringLiteral("licence.txt"));

    const QJsonObject manifest =
        QJsonDocument::fromJson(all.manifest.data).object();
    REQUIRE_EQ(manifest.value(QLatin1String("bundle_format")).toInt(), 1);
    REQUIRE_EQ(manifest.value(QLatin1String("created")).toString(),
               QStringLiteral("2027-06-01T10:30:00Z"));
    REQUIRE(!manifest.value(QLatin1String("studio_version"))
                 .toString()
                 .isEmpty());
    const QJsonObject first =
        manifest.value(QLatin1String("sections")).toArray().at(0).toObject();
    REQUIRE_EQ(first.value(QLatin1String("title")).toString(),
               QStringLiteral("Version and environment"));
    REQUIRE_EQ(first.value(QLatin1String("files")).toArray().at(0).toString(),
               QStringLiteral("environment.txt"));
    REQUIRE(!first.value(QLatin1String("sensitive")).toBool());
    REQUIRE_EQ(first.value(QLatin1String("added_by")).toString(),
               QStringLiteral("studio"));

    // A section that was left out has no entry and no file.
    support::bundle_selection some;
    some.driver = false;
    some.event_log = false;
    some.licence = false;
    const support::bundle_contents fewer = support::build_bundle(
        s.backend, s.log, some, markers(), created());
    REQUIRE_EQ(manifest_ids(fewer).join(QLatin1Char(',')),
               QStringLiteral("environment,setup,counters,serial-ports"));
    REQUIRE(file_named(fewer, QStringLiteral("driver.txt")) == nullptr);
    REQUIRE(file_named(fewer, QStringLiteral("log.txt")) == nullptr);
    REQUIRE(file_named(fewer, QStringLiteral("licence.txt")) == nullptr);

    // Studio never writes the section kept for the replay plugin, and no
    // selection holds the bytes a port carried.
    REQUIRE(!manifest_ids(all).contains(
        QLatin1String(support::reserved_traffic_section)));
    for (const support::bundle_file& file : files)
        REQUIRE(!file.name.startsWith(QStringLiteral("traffic")));
}

TEST_CASE("support bundle: without a licence there is no licence section")
{
    FakeStudioBackend backend;
    support::EventLog log;
    REQUIRE(!support::bundle_has_licence(backend));
    REQUIRE(!support::bundle_licence_names_computers(backend));

    support::bundle_selection selection;
    selection.licensee_name = true;
    selection.computer_name = true;
    const support::bundle_contents contents = support::build_bundle(
        backend, log, selection, markers(), created());
    REQUIRE(!manifest_ids(contents).contains(QStringLiteral("licence")));
    REQUIRE(!all_bytes(contents).contains(computer_marker));

    const support::bundle_file* environment =
        file_named(contents, QStringLiteral("environment.txt"));
    REQUIRE(environment != nullptr);
    REQUIRE(QString::fromUtf8(environment->data)
                .contains(QStringLiteral("Edition: Community")));
    const support::bundle_file* events =
        file_named(contents, QStringLiteral("log.txt"));
    REQUIRE(events != nullptr);
    REQUIRE(!events->data.isEmpty());
    const support::bundle_file* ports =
        file_named(contents, QStringLiteral("serial-ports.txt"));
    REQUIRE(ports != nullptr);
    REQUIRE(QString::fromUtf8(ports->data)
                .contains(QStringLiteral("No serial ports found.")));
}

TEST_CASE("support bundle: the amber rows add only what they name")
{
    seeded s;

    support::bundle_selection named;
    named.licensee_name = true;
    const support::bundle_contents with_name = support::build_bundle(
        s.backend, s.log, named, markers(), created());
    const QString licence = QString::fromUtf8(
        file_named(with_name, QStringLiteral("licence.txt"))->data);
    REQUIRE(licence.contains(QStringLiteral("Licensed to: ") + licensee_marker));
    REQUIRE(!all_bytes(with_name).contains(computer_marker,
                                           Qt::CaseInsensitive));
    const QJsonObject manifest =
        QJsonDocument::fromJson(with_name.manifest.data).object();
    REQUIRE(manifest.value(QLatin1String("sections"))
                .toArray()
                .last()
                .toObject()
                .value(QLatin1String("sensitive"))
                .toBool());

    support::bundle_selection computer;
    computer.computer_name = true;
    const support::bundle_contents with_computer = support::build_bundle(
        s.backend, s.log, computer, markers(), created());
    const QString named_computer = QString::fromUtf8(
        file_named(with_computer, QStringLiteral("licence.txt"))->data);
    REQUIRE(named_computer.contains(QStringLiteral("This computer: ") +
                                    computer_marker));
    REQUIRE(named_computer.contains(QStringLiteral("Computers in the licence: ") +
                                    computer_marker + QLatin1Char('*')));
    REQUIRE(!named_computer.contains(licensee_marker));
    // Only licence.txt gains the name. The log and the setup stay clean.
    for (const QString& other :
         {QStringLiteral("log.txt"), QStringLiteral("setup.json"),
          QStringLiteral("counters.csv"), QStringLiteral("environment.txt")})
    {
        REQUIRE(!QString::fromUtf8(file_named(with_computer, other)->data)
                     .contains(computer_marker, Qt::CaseInsensitive));
    }

    // A licence that names no computer never gains the computer's name.
    licence_view anywhere =
        sample::view_of(sample::judge(sample::annual(), "2027-01-01T00:00:00Z"));
    anywhere.computer_name = computer_marker;
    s.backend.set_licence(anywhere);
    REQUIRE(!support::bundle_licence_names_computers(s.backend));
    REQUIRE(!all_bytes(support::build_bundle(s.backend, s.log, computer,
                                             markers(), created()))
                 .contains(computer_marker, Qt::CaseInsensitive));
}

TEST_CASE("support bundle: a hardware id gives its vendor and product and "
          "nothing else")
{
    using support::bundle_hardware_summary;
    REQUIRE_EQ(bundle_hardware_summary(
                   QStringLiteral("USB\\VID_0403&PID_6001\\A50285BI")),
               QStringLiteral("0403:6001"));
    REQUIRE_EQ(bundle_hardware_summary(QStringLiteral(
                   "FTDIBUS\\VID_0403+PID_6001+A50285BIA\\0000")),
               QStringLiteral("0403:6001"));
    REQUIRE_EQ(bundle_hardware_summary(
                   QStringLiteral("usb:1a86:7523:5432ABCDEF")),
               QStringLiteral("1a86:7523"));
    REQUIRE_EQ(bundle_hardware_summary(QStringLiteral("ACPI\\PNP0501\\1")),
               QStringLiteral("ACPI"));
    REQUIRE_EQ(bundle_hardware_summary(QStringLiteral("VSSCOM\\PORT\\0007")),
               QStringLiteral("VSSCOM"));
    REQUIRE_EQ(bundle_hardware_summary(QString()), QStringLiteral("not known"));
    REQUIRE_EQ(bundle_hardware_summary(QStringLiteral("5432ABCDEF serial")),
               QStringLiteral("not known"));
    // An id that is only a serial number is not taken for a bus name.
    REQUIRE_EQ(bundle_hardware_summary(QStringLiteral("A50285BI")),
               QStringLiteral("not known"));
}

TEST_CASE("support bundle: the file name carries the date")
{
    REQUIRE_EQ(support::bundle_default_name(QDate(2027, 6, 1)),
               QStringLiteral("vss-support-2027-06-01.zip"));
}
