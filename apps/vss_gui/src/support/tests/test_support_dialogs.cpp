//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The support bundle dialog and the Support page
// (docs/gui-design/docs/screens/09-licence-and-support.md): the rows of the
// checklist, the preview that equals the written zip, the Save and failure
// paths, and the three variants of "Your support". The backend is the fake
// one, which can report any licence.
//

#include "qt_wait.hpp"
#include "test_framework.hpp"
#include "zip_reader.hpp"

#include "backend/fake_studio_backend.hpp"
#include "support/about_dialog.hpp"
#include "support/build_info.hpp"
#include "support/bundle.hpp"
#include "support/event_log.hpp"
#include "support/licence_dialog.hpp"
#include "support/licence_text.hpp"
#include "support/support_bundle_dialog.hpp"
#include "support/support_dialog.hpp"
#include "support/tests/licence_samples.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>

#include <functional>

using namespace vss::gui;
namespace lic = vss::license;
namespace sample = vss::gui::test_licence;
namespace zip = vss::gui::test_zip;
using vss::gui::support::support_variant;
using vss::gui::support::SupportBundleDialog;
using vss::gui::support::SupportDialog;

namespace
{

/// A licence with the features of `annual()` that names computers.
licence_view licence_naming_computers()
{
    lic::licence value = sample::annual();
    value.machines = {"LAB-*"};
    return sample::view_of(sample::judge(value, "2027-03-01T12:00:00Z"));
}

licence_view licence_for_anywhere()
{
    return sample::view_of(sample::valid_status());
}

licence_view licence_without_support()
{
    lic::licence value = sample::annual();
    value.features = {"plugins.official"};
    return sample::view_of(sample::judge(value, "2027-03-01T12:00:00Z"));
}

void log_message(support::EventLog& log, const QString& text)
{
    gui_event event;
    event.kind = vss::event_kind::log_message;
    event.name = QStringLiteral("rig");
    event.message = text;
    log.append(event);
}

QStringList titles_of(const QVector<QCheckBox*>& checks)
{
    QStringList titles;
    for (const QCheckBox* check : checks)
        titles << check->accessibleName();
    return titles;
}

QStringList file_names(const QListWidget& list)
{
    QStringList names;
    for (int row = 0; row < list.count(); ++row)
        names << list.item(row)->text();
    return names;
}

/// The row whose text is `name`, or -1.
int row_of(const QListWidget& list, const QString& name)
{
    for (int row = 0; row < list.count(); ++row)
    {
        if (list.item(row)->text() == name)
            return row;
    }
    return -1;
}

QStringList file_names(const QVector<support::bundle_file>& files)
{
    QStringList names;
    for (const support::bundle_file& file : files)
        names << file.name;
    return names;
}

const support::bundle_file* file_named(const SupportBundleDialog& dialog,
                                       const QString& name)
{
    for (const support::bundle_file& file : dialog.files())
    {
        if (file.name == name)
            return &file;
    }
    return nullptr;
}

QVector<zip::zip_entry> read_archive(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    return zip::read_zip(file.readAll());
}

QStringList manifest_ids(const QByteArray& manifest)
{
    QStringList ids;
    const QJsonObject object = QJsonDocument::fromJson(manifest).object();
    for (const auto& entry : object.value(QLatin1String("sections")).toArray())
        ids << entry.toObject().value(QLatin1String("id")).toString();
    return ids;
}

QPushButton* button_named(QWidget& parent, const QString& text)
{
    for (QPushButton* button : parent.findChildren<QPushButton*>())
    {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

bool has_label(QWidget& parent, const QString& text)
{
    for (QLabel* label : parent.findChildren<QLabel*>())
    {
        if (label->text() == text)
            return true;
    }
    return false;
}

QStringList toast_texts(QWidget& parent)
{
    QStringList texts;
    for (Toast* toast : parent.findChildren<Toast*>())
    {
        for (QLabel* label : toast->findChildren<QLabel*>())
            texts << label->text();
    }
    return texts;
}

/// What the failure dialog showed.
struct failure_seen
{
    int count = 0;
    QString title;
    QStringList texts;
    QStringList buttons;
    QString default_button;
    QString copied;
};

/// Records the next "Could not save the bundle" dialog, copies its details
/// when asked, then presses `press`. The handler must not use REQUIRE, which
/// cannot unwind through the event loop. If no dialog appears in time the
/// modal widget is rejected, so the test fails instead of hanging.
void answer_failure(failure_seen& seen, const QString& press, bool copy_first,
                    int tries = 300)
{
    QTimer::singleShot(
        10, QApplication::instance(),
        [&seen, press, copy_first, tries]
        {
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                if (widget->objectName() != QLatin1String("bundleSaveFailed") ||
                    !widget->isVisible())
                    continue;
                ++seen.count;
                seen.title = widget->windowTitle();
                seen.texts.clear();
                for (QLabel* label : widget->findChildren<QLabel*>())
                    seen.texts << label->text();
                seen.buttons.clear();
                for (QPushButton* button : widget->findChildren<QPushButton*>())
                {
                    seen.buttons << button->text();
                    if (button->isDefault())
                        seen.default_button = button->text();
                }
                if (copy_first)
                {
                    if (QPushButton* copy = button_named(
                            *widget, QStringLiteral("Copy details")))
                    {
                        QGuiApplication::clipboard()->clear();
                        copy->click();
                        seen.copied = QGuiApplication::clipboard()->text();
                    }
                }
                if (QPushButton* target = button_named(*widget, press))
                    target->click();
                return;
            }
            if (tries > 0)
            {
                answer_failure(seen, press, copy_first, tries - 1);
                return;
            }
            if (QWidget* modal = QApplication::activeModalWidget())
                modal->close();
        });
}

/// Closes the renew dialog that opens while a button is pressed.
bool renew_dialog_opened(const std::function<void()>& trigger)
{
    bool found = false;
    QTimer::singleShot(
        10, QApplication::instance(),
        [&found]
        {
            for (QWidget* widget : QApplication::topLevelWidgets())
            {
                if (auto* dialog = qobject_cast<support::RenewDialog*>(widget))
                {
                    found = true;
                    dialog->reject();
                }
            }
        });
    trigger();
    return found;
}

} // namespace

// -- the support bundle dialog
// -----------------------------------------------------

TEST_CASE("support bundle dialog: the rows follow the licence state")
{
    support::EventLog log;

    // No licence: six sections, no Licence row and no amber row.
    {
        FakeStudioBackend backend;
        SupportBundleDialog dialog(backend, log);
        REQUIRE_EQ(titles_of(dialog.checks()).join(QLatin1Char('|')),
                   QStringLiteral("Version and environment|Driver state|"
                                  "Setup|Event log|Port counters|"
                                  "Serial ports on this computer"));
        REQUIRE(dialog.check(QStringLiteral("bundleLicence")) == nullptr);
        REQUIRE(dialog.check(QStringLiteral("bundleLicenseeName")) == nullptr);
        REQUIRE(dialog.check(QStringLiteral("bundleComputerName")) == nullptr);
        for (const QCheckBox* check : dialog.checks())
            REQUIRE(check->isChecked());
        REQUIRE_EQ(file_names(dialog.files()).join(QLatin1Char('|')),
                   QStringLiteral("manifest.json|environment.txt|driver.txt|"
                                  "setup.json|log.txt|counters.csv|"
                                  "serial-ports.txt"));
    }

    // A licence for anywhere: the Licence row and the Licensee name row.
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_for_anywhere());
        SupportBundleDialog dialog(backend, log);
        REQUIRE_EQ(titles_of(dialog.checks()).join(QLatin1Char('|')),
                   QStringLiteral("Version and environment|Driver state|"
                                  "Setup|Event log|Port counters|"
                                  "Serial ports on this computer|Licence|"
                                  "Licensee name"));
        REQUIRE(dialog.check(QStringLiteral("bundleComputerName")) == nullptr);
        QCheckBox* licence = dialog.check(QStringLiteral("bundleLicence"));
        QCheckBox* licensee =
            dialog.check(QStringLiteral("bundleLicenseeName"));
        REQUIRE(licence != nullptr);
        REQUIRE(licensee != nullptr);
        REQUIRE(licence->isChecked());
        REQUIRE(!licensee->isChecked());
        REQUIRE_EQ(licensee->accessibleDescription(),
                   QStringLiteral("The name your licence was issued to. Turn "
                                  "this on if you are writing to support "
                                  "about the licence itself."));
        REQUIRE_EQ(licensee->parentWidget()->property("note").toString(),
                   QStringLiteral("warn"));
        REQUIRE(licence->parentWidget()->property("note").toString().isEmpty());
        REQUIRE(dialog.files().size() == 8);
        REQUIRE(file_named(dialog, QStringLiteral("licence.txt")) != nullptr);
    }

    // A licence that names computers: the third row appears as well, off.
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_naming_computers());
        SupportBundleDialog dialog(backend, log);
        REQUIRE_EQ(titles_of(dialog.checks()).join(QLatin1Char('|')),
                   QStringLiteral("Version and environment|Driver state|"
                                  "Setup|Event log|Port counters|"
                                  "Serial ports on this computer|Licence|"
                                  "Licensee name|This computer's name"));
        QCheckBox* computer =
            dialog.check(QStringLiteral("bundleComputerName"));
        REQUIRE(computer != nullptr);
        REQUIRE(!computer->isChecked());
        REQUIRE(
            !dialog.check(QStringLiteral("bundleLicenseeName"))->isChecked());
        REQUIRE_EQ(computer->accessibleDescription(),
                   QStringLiteral("Needed when a licence names computers."));
        REQUIRE_EQ(computer->parentWidget()->property("note").toString(),
                   QStringLiteral("warn"));
        // The amber rows say so in words as well as colour.
        REQUIRE(has_label(*computer->parentWidget(),
                          QStringLiteral("Off by default")));
        REQUIRE(!dialog.selection().licensee_name);
        REQUIRE(!dialog.selection().computer_name);
        REQUIRE_EQ(dialog.files().size(), 8);
        REQUIRE(!QString::fromUtf8(
                     file_named(dialog, QStringLiteral("licence.txt"))->data)
                     .contains(QStringLiteral("Example Lab")));
    }
}

TEST_CASE("support bundle dialog: the text of the dialog")
{
    FakeStudioBackend backend;
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    REQUIRE_EQ(dialog.windowTitle(), QStringLiteral("Create a support bundle"));
    REQUIRE(has_label(dialog, QStringLiteral("Create a support bundle")));
    REQUIRE(has_label(
        dialog,
        QStringLiteral("Gathers what a helper needs to understand your setup. "
                       "Nothing is sent anywhere. You choose where to save "
                       "it.")));
    REQUIRE(has_label(dialog, QStringLiteral("Include")));
    REQUIRE(has_label(dialog, QStringLiteral("Always left out")));
    REQUIRE(has_label(
        dialog,
        QStringLiteral(
            "The bytes your ports sent and received are never included. "
            "Passwords, tokens and keys are replaced with [redacted]. The "
            "path of your home folder is shortened to ~. The name of this "
            "computer and your user name are not included. Serial numbers of "
            "devices are not included. The licence file itself is never "
            "included.")));
    REQUIRE(has_label(dialog, QStringLiteral("Preview")));
    REQUIRE(has_label(
        dialog, QStringLiteral("This is exactly what the file will contain.")));

    bool banner = false;
    for (NoteBanner* note : dialog.findChildren<NoteBanner*>())
    {
        if (note->text() == QStringLiteral("Nothing leaves this computer until "
                                           "you share the file yourself.") &&
            note->get_kind() == NoteBanner::kind::info)
            banner = true;
    }
    REQUIRE(banner);

    // The footer: the file name in the fixed font, the count, the buttons.
    auto* name = dialog.findChild<QLabel*>(QStringLiteral("bundleName"));
    REQUIRE(name != nullptr);
    REQUIRE_EQ(name->text(), support::bundle_default_name());
    REQUIRE(name->text().startsWith(QStringLiteral("vss-support-")));
    REQUIRE_EQ(dialog.file_count_label()->text(), QStringLiteral("7 files"));
    REQUIRE_EQ(dialog.copy_button()->text(),
               QStringLiteral("Copy environment only"));
    REQUIRE_EQ(dialog.copy_button()->property("role").toString(),
               QStringLiteral("quiet"));
    REQUIRE_EQ(dialog.cancel_button()->text(), QStringLiteral("Cancel"));
    REQUIRE_EQ(dialog.save_button()->text(), QStringLiteral("Save bundle"));
    REQUIRE_EQ(dialog.save_button()->property("role").toString(),
               QStringLiteral("primary"));
    REQUIRE(dialog.save_button()->isDefault());

    // One file: only the manifest is left.
    for (QCheckBox* check : dialog.checks())
        check->setChecked(false);
    REQUIRE_EQ(dialog.file_count_label()->text(), QStringLiteral("1 file"));
    REQUIRE_EQ(dialog.files().size(), 1);
}

TEST_CASE("support bundle dialog: the preview is the file, the first file is "
          "selected, and unticking removes a file at once")
{
    FakeStudioBackend backend;
    backend.set_licence(licence_naming_computers());
    support::EventLog log;
    log_message(log, QStringLiteral("opened"));
    SupportBundleDialog dialog(backend, log);

    QListWidget* list = dialog.file_list();
    QPlainTextEdit* preview = dialog.preview();
    REQUIRE_EQ(list->count(), 8);
    REQUIRE_EQ(list->item(0)->text(), QStringLiteral("manifest.json"));
    REQUIRE_EQ(list->currentRow(), 0);
    // The first file is shown when the dialog opens.
    REQUIRE(preview->isReadOnly());
    REQUIRE_EQ(preview->toPlainText(),
               QString::fromUtf8(dialog.files().at(0).data));
    REQUIRE_EQ(preview->accessibleName(),
               QStringLiteral("Preview of manifest.json"));
    // Each row carries its size.
    // The separator may be a no-break space.
    REQUIRE(list->item(0)
                ->data(Qt::UserRole + 1)
                .toString()
                .contains(QRegularExpression(
                    QStringLiteral("^[0-9][0-9.,]*.(bytes|kB|MB)$"))));
    REQUIRE(list->item(0)
                ->data(Qt::AccessibleTextRole)
                .toString()
                .startsWith(QStringLiteral("manifest.json, ")));
    REQUIRE(!dialog.shortened_note()->isVisibleTo(&dialog));

    // Selecting another file shows that file.
    const int log_row = row_of(*list, QStringLiteral("log.txt"));
    REQUIRE(log_row > 0);
    list->setCurrentRow(log_row);
    REQUIRE(preview->toPlainText().contains(QStringLiteral("opened")));
    REQUIRE_EQ(preview->accessibleName(), QStringLiteral("Preview of log.txt"));

    // Unticking a row removes its file at once, and the selection stays on
    // the file that is still there.
    dialog.check(QStringLiteral("bundleDriver"))->setChecked(false);
    REQUIRE(!file_names(*list).contains(QStringLiteral("driver.txt")));
    REQUIRE_EQ(list->count(), 7);
    REQUIRE_EQ(list->currentItem()->text(), QStringLiteral("log.txt"));
    REQUIRE_EQ(preview->accessibleName(), QStringLiteral("Preview of log.txt"));
    REQUIRE_EQ(dialog.file_count_label()->text(), QStringLiteral("7 files"));
    REQUIRE_EQ(dialog.selected_file_count(), 7);

    // Unticking the file that is shown moves the preview to the first file.
    dialog.check(QStringLiteral("bundleLog"))->setChecked(false);
    REQUIRE(!file_names(*list).contains(QStringLiteral("log.txt")));
    REQUIRE_EQ(list->currentItem()->text(), QStringLiteral("manifest.json"));
    REQUIRE_EQ(preview->toPlainText(),
               QString::fromUtf8(dialog.files().at(0).data));

    // The manifest lists exactly what is ticked.
    REQUIRE_EQ(manifest_ids(dialog.files().at(0).data).join(QLatin1Char(',')),
               QStringLiteral("environment,setup,counters,serial-ports,"
                              "licence"));

    // Ticking it again brings it back.
    dialog.check(QStringLiteral("bundleDriver"))->setChecked(true);
    REQUIRE(file_names(*list).contains(QStringLiteral("driver.txt")));
}

TEST_CASE("support bundle dialog: for every file the preview equals the bytes "
          "in the zip, and the manifest lists the ticked sections")
{
    FakeStudioBackend backend;
    backend.set_licence(licence_naming_computers());
    const auto port = backend.create_virtual_port(
        QStringLiteral("rig"), vss::virtual_port_kind::loopback, true);
    REQUIRE(port.has_value());
    support::EventLog log;
    log_message(log, QStringLiteral("opened"));
    log_message(log, QStringLiteral("password=hunter2"));
    SupportBundleDialog dialog(backend, log);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());

    struct scenario
    {
        const char* name;
        std::function<void()> prepare;
        const char* ids;
    };
    const scenario scenarios[] = {
        {"defaults", [] {},
         "environment,driver,setup,log,counters,serial-ports,licence"},
        {"fewer sections",
         [&dialog]
         {
             dialog.check(QStringLiteral("bundleDriver"))->setChecked(false);
             dialog.check(QStringLiteral("bundleCounters"))->setChecked(false);
         },
         "environment,setup,log,serial-ports,licence"},
        {"every amber row on",
         [&dialog]
         {
             dialog.check(QStringLiteral("bundleLicenseeName"))
                 ->setChecked(true);
             dialog.check(QStringLiteral("bundleComputerName"))
                 ->setChecked(true);
         },
         "environment,setup,log,serial-ports,licence"},
        {"licence left out", [&dialog]
         { dialog.check(QStringLiteral("bundleLicence"))->setChecked(false); },
         "environment,setup,log,serial-ports"},
    };

    int number = 0;
    for (const scenario& step : scenarios)
    {
        step.prepare();
        QString path =
            directory.filePath(QStringLiteral("bundle%1.zip").arg(++number));
        dialog.set_path_picker([path](const QString&) { return path; });
        dialog.set_folder_opener([](const QString&) {});
        dialog.save();
        REQUIRE(QFile::exists(path));

        const QVector<zip::zip_entry> entries = read_archive(path);
        REQUIRE_EQ(entries.size(), dialog.files().size());
        // The manifest is first, and the zip holds the files in the order of
        // the list.
        REQUIRE_EQ(entries.at(0).name, QStringLiteral("manifest.json"));
        REQUIRE_EQ(file_names(dialog.files()).join(QLatin1Char('|')),
                   [&entries]
                   {
                       QStringList names;
                       for (const zip::zip_entry& entry : entries)
                           names << entry.name;
                       return names.join(QLatin1Char('|'));
                   }());
        REQUIRE_EQ(manifest_ids(entries.at(0).data).join(QLatin1Char(',')),
                   QString::fromLatin1(step.ids));

        // For every file, the text in the preview is the text in the zip.
        QListWidget* list = dialog.file_list();
        REQUIRE_EQ(list->count(), static_cast<int>(entries.size()));
        for (int row = 0; row < list->count(); ++row)
        {
            list->setCurrentRow(row);
            REQUIRE_EQ(list->currentItem()->text(), entries.at(row).name);
            REQUIRE_EQ(dialog.preview()->toPlainText(),
                       QString::fromUtf8(entries.at(row).data));
            REQUIRE_EQ(
                dialog.preview()->accessibleName(),
                QStringLiteral("Preview of %1").arg(entries.at(row).name));
            REQUIRE_EQ(entries.at(row).data, dialog.files().at(row).data);
        }

        // The sections the zip holds are the sections that were ticked: a
        // section left out has no file.
        const QStringList ids = manifest_ids(entries.at(0).data);
        REQUIRE_EQ(
            ids.contains(QStringLiteral("driver")),
            file_names(dialog.files()).contains(QStringLiteral("driver.txt")));
        REQUIRE_EQ(
            ids.contains(QStringLiteral("licence")),
            file_names(dialog.files()).contains(QStringLiteral("licence.txt")));
        // Saving closes the dialog. Open it again for the next step.
        dialog.setResult(QDialog::Rejected);
    }
}

TEST_CASE("support bundle dialog: a file over 256 kB is shown shortened and "
          "saved whole")
{
    FakeStudioBackend backend;
    support::EventLog log;
    const QString line(200, QLatin1Char('x'));
    for (int index = 0; index < support::EventLog::kMaxLines; ++index)
        log_message(log, line);
    SupportBundleDialog dialog(backend, log);

    const support::bundle_file* big =
        file_named(dialog, QStringLiteral("log.txt"));
    REQUIRE(big != nullptr);
    REQUIRE(big->data.size() > support::kBundlePreviewLimit);
    const QByteArray whole = big->data;

    QListWidget* list = dialog.file_list();
    // The small files show no note.
    REQUIRE(!dialog.shortened_note()->isVisibleTo(&dialog));
    REQUIRE_EQ(dialog.shortened_note()->text(),
               QStringLiteral("Preview shortened. The saved file is "
                              "complete."));

    list->setCurrentRow(row_of(*list, QStringLiteral("log.txt")));
    // The list gives the size of the whole file, not of the preview.
    REQUIRE(list->currentItem()
                ->data(Qt::UserRole + 1)
                .toString()
                .contains(QStringLiteral("MB")));
    REQUIRE(dialog.shortened_note()->isVisibleTo(&dialog));
    const QByteArray shown = dialog.preview()->toPlainText().toUtf8();
    REQUIRE_EQ(static_cast<qsizetype>(shown.size()),
               support::kBundlePreviewLimit);
    REQUIRE_EQ(shown, whole.left(support::kBundlePreviewLimit));

    // The saved file is complete.
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString path = directory.filePath(QStringLiteral("big.zip"));
    dialog.set_path_picker([path](const QString&) { return path; });
    dialog.set_folder_opener([](const QString&) {});
    dialog.save();
    const QVector<zip::zip_entry> entries = read_archive(path);
    bool seen = false;
    for (const zip::zip_entry& entry : entries)
    {
        if (entry.name == QStringLiteral("log.txt"))
        {
            seen = true;
            REQUIRE_EQ(entry.data, whole);
        }
    }
    REQUIRE(seen);

    // Another file hides the note again.
    list->setCurrentRow(0);
    REQUIRE(!dialog.shortened_note()->isVisibleTo(&dialog));
}

TEST_CASE("support bundle dialog: a long file is cut at a whole character")
{
    FakeStudioBackend backend;
    support::EventLog log;
    // Two-byte characters, so the limit falls inside one for some offsets.
    const QString line(100, QChar(0x00e9));
    for (int index = 0; index < support::EventLog::kMaxLines; ++index)
        log_message(log, QStringLiteral("é") + line);
    SupportBundleDialog dialog(backend, log);
    QListWidget* list = dialog.file_list();
    list->setCurrentRow(row_of(*list, QStringLiteral("log.txt")));
    const QString shown = dialog.preview()->toPlainText();
    REQUIRE(!shown.contains(QChar(0xfffd)));
    const QByteArray bytes = shown.toUtf8();
    REQUIRE(bytes.size() <= support::kBundlePreviewLimit);
    REQUIRE(bytes.size() >= support::kBundlePreviewLimit - 3);
}

TEST_CASE("support bundle dialog: the zip is written only where the person "
          "chose")
{
    FakeStudioBackend backend;
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString chosen = directory.filePath(QStringLiteral("mine.zip"));

    // Cancelling the save dialog writes nothing and keeps the dialog open.
    int asked = 0;
    QString suggested;
    dialog.set_path_picker(
        [&asked, &suggested](const QString& path)
        {
            ++asked;
            suggested = path;
            return QString();
        });
    dialog.save();
    REQUIRE_EQ(asked, 1);
    REQUIRE_EQ(suggested, dialog.suggested_path());
    REQUIRE(QFileInfo(suggested).fileName().startsWith(
        QStringLiteral("vss-support-")));
    REQUIRE_EQ(dialog.result(), static_cast<int>(QDialog::Rejected));
    REQUIRE(QDir(directory.path())
                .entryList(QDir::Files | QDir::NoDotAndDotDot)
                .isEmpty());

    // A chosen path gets the file, and nothing else is written beside it.
    dialog.set_path_picker(
        [&asked, &chosen](const QString&)
        {
            ++asked;
            return chosen;
        });
    QString opened;
    dialog.set_folder_opener([&opened](const QString& folder)
                             { opened = folder; });
    const bool existed = QFile::exists(dialog.suggested_path());
    dialog.save();
    REQUIRE_EQ(asked, 2);
    REQUIRE_EQ(QDir(directory.path())
                   .entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
                   .join(QLatin1Char('|')),
               QStringLiteral("mine.zip"));
    // The suggested name was only a suggestion.
    REQUIRE_EQ(QFile::exists(dialog.suggested_path()), existed);
    REQUIRE_EQ(dialog.result(), static_cast<int>(QDialog::Accepted));

    // Saved: the toast names the file and offers "Show in folder".
    const QStringList toasts = toast_texts(dialog);
    REQUIRE(toasts.contains(QStringLiteral("Saved mine.zip.")));
    Toast* toast = dialog.findChild<Toast*>();
    REQUIRE(toast != nullptr);
    QPushButton* show = button_named(*toast, QStringLiteral("Show in folder"));
    REQUIRE(show != nullptr);
    show->click();
    REQUIRE_EQ(QDir::cleanPath(opened), QDir::cleanPath(directory.path()));

    // A path without an extension gets ".zip".
    SupportBundleDialog second(backend, log);
    QString bare = directory.filePath(QStringLiteral("plain"));
    second.set_path_picker([&bare](const QString&) { return bare; });
    second.set_folder_opener([](const QString&) {});
    second.save();
    REQUIRE(QFile::exists(bare + QStringLiteral(".zip")));
    REQUIRE(!QFile::exists(bare));
}

TEST_CASE("support bundle dialog: a path that cannot be written ends in the "
          "failure dialog")
{
    FakeStudioBackend backend;
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    QString bad =
        directory.filePath(QStringLiteral("no/such/folder/bundle.zip"));
    const QString good = directory.filePath(QStringLiteral("again.zip"));

    // Cancel leaves the failure and the dialog stays open.
    int asked = 0;
    dialog.set_path_picker(
        [&asked, &bad](const QString&)
        {
            ++asked;
            return bad;
        });
    failure_seen first;
    answer_failure(first, QStringLiteral("Cancel"), true);
    dialog.save();
    REQUIRE_EQ(first.count, 1);
    REQUIRE_EQ(asked, 1);
    REQUIRE_EQ(first.title, QStringLiteral("Could not save the bundle"));
    REQUIRE(first.texts.contains(QStringLiteral(
        "Studio could not write to that folder. Choose another place, such as "
        "your Documents folder.")));
    REQUIRE_EQ(first.buttons.join(QLatin1Char('|')),
               QStringLiteral("Copy details|Choose another place|Cancel"));
    REQUIRE_EQ(first.default_button, QStringLiteral("Choose another place"));
    REQUIRE(!first.copied.isEmpty());
    REQUIRE_EQ(dialog.result(), static_cast<int>(QDialog::Rejected));
    REQUIRE(!QFile::exists(bad));

    // "Choose another place" asks again, and a good path is written.
    int again = 0;
    dialog.set_path_picker(
        [&again, &bad, &good](const QString&)
        {
            ++again;
            return again == 1 ? bad : good;
        });
    dialog.set_folder_opener([](const QString&) {});
    failure_seen second;
    answer_failure(second, QStringLiteral("Choose another place"), false);
    dialog.save();
    REQUIRE_EQ(second.count, 1);
    REQUIRE_EQ(again, 2);
    REQUIRE(QFile::exists(good));
    REQUIRE_EQ(dialog.result(), static_cast<int>(QDialog::Accepted));
}

TEST_CASE("support bundle dialog: the preview has an accessible name and the "
          "keyboard reaches the checklist, the list and the preview in order")
{
    FakeStudioBackend backend;
    backend.set_licence(licence_naming_computers());
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    dialog.show();
    QApplication::processEvents();

    REQUIRE_EQ(dialog.preview()->accessibleName(),
               QStringLiteral("Preview of manifest.json"));
    REQUIRE(!dialog.file_list()->accessibleName().isEmpty());
    for (const QCheckBox* check : dialog.checks())
        REQUIRE(!check->accessibleName().isEmpty());
    REQUIRE(dialog.preview()->focusPolicy() != Qt::NoFocus);
    REQUIRE(dialog.file_list()->focusPolicy() != Qt::NoFocus);

    // Initial focus is on the checklist.
    REQUIRE(vss_test::wait_until([&dialog]
                                 { return dialog.focusWidget() != nullptr; }));
    REQUIRE(dialog.focusWidget() == dialog.checks().first());

    // Following the focus chain from the first box: the checklist in the
    // order of its rows, then the list of files, then the preview, then the
    // buttons.
    QList<QWidget*> wanted;
    for (QCheckBox* check : dialog.checks())
        wanted << check;
    wanted << dialog.file_list() << dialog.preview() << dialog.copy_button()
           << dialog.cancel_button() << dialog.save_button();
    QList<QWidget*> walked;
    QWidget* cursor = dialog.checks().first();
    for (int step = 0; step < 200; ++step)
    {
        if (wanted.contains(cursor) && !walked.contains(cursor))
            walked << cursor;
        cursor = cursor->nextInFocusChain();
        if (cursor == dialog.checks().first())
            break;
    }
    REQUIRE_EQ(walked.size(), wanted.size());
    for (qsizetype index = 0; index < wanted.size(); ++index)
        REQUIRE(walked.at(index) == wanted.at(index));
}

TEST_CASE("support bundle dialog: the amber rows wait for the Licence row")
{
    FakeStudioBackend backend;
    backend.set_licence(licence_naming_computers());
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    QCheckBox* licence = dialog.check(QStringLiteral("bundleLicence"));
    QCheckBox* licensee = dialog.check(QStringLiteral("bundleLicenseeName"));
    QCheckBox* computer = dialog.check(QStringLiteral("bundleComputerName"));
    REQUIRE(licensee->isEnabled());
    REQUIRE(computer->isEnabled());

    // They add to licence.txt, so they cannot be ticked without it.
    licensee->setChecked(true);
    licence->setChecked(false);
    REQUIRE(!licensee->isEnabled());
    REQUIRE(!computer->isEnabled());
    REQUIRE(file_named(dialog, QStringLiteral("licence.txt")) == nullptr);
    licence->setChecked(true);
    REQUIRE(licensee->isEnabled());
    REQUIRE(licensee->isChecked());
    REQUIRE(QString::fromUtf8(
                file_named(dialog, QStringLiteral("licence.txt"))->data)
                .contains(QStringLiteral("Licensed to: Example Lab Pty Ltd")));
}

TEST_CASE("support bundle dialog: Copy environment only copies the "
          "environment file")
{
    FakeStudioBackend backend;
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    const support::bundle_file* environment =
        file_named(dialog, QStringLiteral("environment.txt"));
    REQUIRE(environment != nullptr);

    QGuiApplication::clipboard()->clear();
    dialog.copy_button()->click();
    REQUIRE_EQ(QGuiApplication::clipboard()->text(),
               QString::fromUtf8(environment->data));
    REQUIRE(QGuiApplication::clipboard()->text().contains(
        QStringLiteral("Edition: Community")));
    REQUIRE(
        toast_texts(dialog).contains(QStringLiteral("Environment copied.")));
    // Copying saves nothing and closes nothing.
    REQUIRE_EQ(dialog.result(), static_cast<int>(QDialog::Rejected));
}

TEST_CASE("support bundle dialog: a build without diagnostics offers nothing "
          "to save")
{
    FakeStudioBackend backend;
    capabilities caps;
    caps.diagnostics = false;
    backend.set_capabilities(caps);
    support::EventLog log;
    SupportBundleDialog dialog(backend, log);
    REQUIRE(!dialog.available());
    REQUIRE(!dialog.save_button()->isEnabled());
    for (const QCheckBox* check : dialog.checks())
        REQUIRE(!check->isEnabled());
    REQUIRE_EQ(dialog.file_list()->count(), 0);
    REQUIRE(has_label(dialog, QStringLiteral("Creating a support bundle is not "
                                             "available in this build.")));
    int asked = 0;
    dialog.set_path_picker(
        [&asked](const QString&)
        {
            ++asked;
            return QString();
        });
    dialog.save();
    REQUIRE_EQ(asked, 0);
    // Copying the environment is still possible.
    QGuiApplication::clipboard()->clear();
    dialog.copy_button()->click();
    REQUIRE(!QGuiApplication::clipboard()->text().isEmpty());
}

// -- the Support page
// --------------------------------------------------------------

TEST_CASE("support page: the card for each licence state")
{
    using vss::gui::support::support_address;

    // Community: no licence at all.
    {
        FakeStudioBackend backend;
        SupportDialog dialog(backend);
        REQUIRE(dialog.variant() == support_variant::community);
        REQUIRE_EQ(dialog.windowTitle(), QStringLiteral("Support"));
        REQUIRE(has_label(
            dialog,
            QStringLiteral("Where to get help, and what to send so a helper "
                           "can understand your setup.")));
        REQUIRE_EQ(dialog.support_chip()->get_kind(),
                   StatusChip::kind::neutral);
        REQUIRE_EQ(dialog.support_chip()->text(),
                   QStringLiteral("Community support"));
        REQUIRE_EQ(dialog.support_sentence()->text(),
                   QStringLiteral(
                       "The Community edition is supported by its users. "
                       "Search the documents and the issue list first, then "
                       "open an issue. Support from the developer comes with "
                       "a Pro or Enterprise licence."));
        REQUIRE(dialog.copy_documents_button()->isVisibleTo(&dialog));
        REQUIRE(dialog.copy_issues_button()->isVisibleTo(&dialog));
        REQUIRE_EQ(dialog.copy_documents_button()->text(),
                   QStringLiteral("Copy documents address"));
        REQUIRE_EQ(dialog.copy_issues_button()->text(),
                   QStringLiteral("Copy issues address"));
        REQUIRE(!dialog.copy_address_button()->isVisibleTo(&dialog));
        REQUIRE(!dialog.copy_number_button()->isVisibleTo(&dialog));
        REQUIRE(!dialog.renew_button()->isVisibleTo(&dialog));
        REQUIRE(has_label(
            dialog,
            QStringLiteral("Studio never sends anything itself. You choose "
                           "what to share.")));
    }

    // A licence that includes support.
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_for_anywhere());
        SupportDialog dialog(backend);
        REQUIRE(dialog.variant() == support_variant::included);
        REQUIRE_EQ(dialog.support_chip()->get_kind(),
                   StatusChip::kind::healthy);
        REQUIRE_EQ(dialog.support_chip()->text(),
                   QStringLiteral("Support included"));
        REQUIRE_EQ(
            dialog.support_sentence()->text(),
            QStringLiteral("Your Pro licence includes support until "
                           "%1. Write to %2 and quote your licence "
                           "number, VSS-2026-000123.")
                .arg(sample::date("2027-11-13T00:00:00Z"), support_address()));
        REQUIRE(dialog.copy_address_button()->isVisibleTo(&dialog));
        REQUIRE(dialog.copy_number_button()->isVisibleTo(&dialog));
        REQUIRE_EQ(dialog.copy_address_button()->text(),
                   QStringLiteral("Copy address"));
        REQUIRE_EQ(dialog.copy_number_button()->text(),
                   QStringLiteral("Copy licence number"));
        REQUIRE(!dialog.copy_documents_button()->isVisibleTo(&dialog));
        REQUIRE(!dialog.copy_issues_button()->isVisibleTo(&dialog));
        REQUIRE(!dialog.renew_button()->isVisibleTo(&dialog));

        // The same dialog follows the licence while it is open.
        backend.set_licence(sample::view_of(sample::expired_status()));
        REQUIRE(dialog.variant() == support_variant::ended);
        REQUIRE_EQ(dialog.support_chip()->text(),
                   QStringLiteral("Support ended"));
        backend.set_licence(licence_view{});
        REQUIRE(dialog.variant() == support_variant::community);
    }

    // A licence without support is the Community card.
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_without_support());
        SupportDialog dialog(backend);
        REQUIRE(dialog.variant() == support_variant::community);
        REQUIRE_EQ(dialog.support_chip()->text(),
                   QStringLiteral("Community support"));
    }

    // A licence that ended, in its grace period and after it.
    for (const lic::status& status :
         {sample::grace_status(), sample::expired_status()})
    {
        FakeStudioBackend backend;
        backend.set_licence(sample::view_of(status));
        SupportDialog dialog(backend);
        REQUIRE(dialog.variant() == support_variant::ended);
        REQUIRE_EQ(dialog.support_chip()->get_kind(),
                   StatusChip::kind::waiting);
        REQUIRE_EQ(dialog.support_chip()->text(),
                   QStringLiteral("Support ended"));
        REQUIRE_EQ(dialog.support_sentence()->text(),
                   QStringLiteral("Support ended with your licence on %1.")
                       .arg(sample::date("2027-11-13T00:00:00Z")));
        REQUIRE(dialog.renew_button()->isVisibleTo(&dialog));
        REQUIRE_EQ(dialog.renew_button()->text(),
                   QStringLiteral("How to renew"));
        REQUIRE(!dialog.copy_address_button()->isVisibleTo(&dialog));
        REQUIRE(!dialog.copy_documents_button()->isVisibleTo(&dialog));

        // "How to renew" opens the renew dialog.
        REQUIRE(
            renew_dialog_opened([&dialog] { dialog.renew_button()->click(); }));
    }
}

TEST_CASE("support page: each copy button puts its own text on the clipboard")
{
    using vss::gui::support::documents_address;
    using vss::gui::support::issues_address;
    using vss::gui::support::support_address;

    REQUIRE_EQ(
        documents_address(),
        QStringLiteral(
            "https://iainchesworthlabs.github.io/virtual-serial-studio/"));
    REQUIRE_EQ(issues_address(),
               QStringLiteral("https://github.com/iainchesworthlabs/"
                              "virtual-serial-studio/issues"));

    {
        FakeStudioBackend backend;
        SupportDialog dialog(backend);
        QGuiApplication::clipboard()->clear();
        dialog.copy_documents_button()->click();
        REQUIRE_EQ(QGuiApplication::clipboard()->text(), documents_address());
        QGuiApplication::clipboard()->clear();
        dialog.copy_issues_button()->click();
        REQUIRE_EQ(QGuiApplication::clipboard()->text(), issues_address());
        REQUIRE(toast_texts(dialog).contains(documents_address() +
                                             QStringLiteral(" copied.")));
    }
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_for_anywhere());
        SupportDialog dialog(backend);
        QGuiApplication::clipboard()->clear();
        dialog.copy_address_button()->click();
        REQUIRE_EQ(QGuiApplication::clipboard()->text(), support_address());
        QGuiApplication::clipboard()->clear();
        dialog.copy_number_button()->click();
        REQUIRE_EQ(QGuiApplication::clipboard()->text(),
                   QStringLiteral("VSS-2026-000123"));
    }
}

TEST_CASE("support page: This copy of Studio, the bundle card and the footer")
{
    const support::build_info info = support::current_build_info();

    // The rows, for Community with no licence number.
    {
        FakeStudioBackend backend;
        SupportDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        REQUIRE(has_label(dialog, QStringLiteral("This copy of Studio")));
        const auto text_of = [&dialog](const char* name)
        {
            auto* label = dialog.findChild<QLabel*>(QString::fromLatin1(name));
            return label != nullptr ? label->text() : QString();
        };
        REQUIRE_EQ(text_of("supportVersion"), info.full_version());
        REQUIRE_EQ(text_of("supportEdition"), QStringLiteral("Community"));
        REQUIRE_EQ(text_of("supportSystem"), info.operating_system);
        auto* number =
            dialog.findChild<QLabel*>(QStringLiteral("supportLicence"));
        REQUIRE(number != nullptr);
        REQUIRE(!number->isVisible());

        // "Copy" copies those lines and says so.
        QGuiApplication::clipboard()->clear();
        dialog.copy_details_button()->click();
        const QString copied = QGuiApplication::clipboard()->text();
        REQUIRE_EQ(copied, dialog.details_text());
        REQUIRE(
            copied.contains(QStringLiteral("Version: ") + info.full_version()));
        REQUIRE(copied.contains(QStringLiteral("Edition: Community")));
        REQUIRE(copied.contains(QStringLiteral("Operating system: ") +
                                info.operating_system));
        REQUIRE(!copied.contains(QStringLiteral("Licence number")));
        REQUIRE(
            toast_texts(dialog).contains(QStringLiteral("Details copied.")));
        REQUIRE_EQ(dialog.copy_details_button()->text(),
                   QStringLiteral("Copy"));
        REQUIRE(dialog.copy_details_button()->accessibleName().contains(
            QStringLiteral("Copy")));

        // The other cards.
        REQUIRE(has_label(dialog, QStringLiteral("Before you write")));
        REQUIRE(has_label(
            dialog, QStringLiteral("Say what you expected and what happened "
                                   "instead.")));
        REQUIRE(has_label(dialog, QStringLiteral("Say which port or program it "
                                                 "happened with.")));
        REQUIRE(has_label(
            dialog,
            QStringLiteral("Attach a support bundle. It answers most first "
                           "questions.")));
        REQUIRE(has_label(
            dialog,
            QStringLiteral(
                "A support bundle is one zip file with the version, the "
                "driver state, your setup, the log and the list of serial "
                "ports on this computer. You see everything in it before you "
                "save it.")));

        // The buttons and what they ask for.
        REQUIRE_EQ(dialog.bundle_button()->text(),
                   QStringLiteral("Create support bundle"));
        REQUIRE_EQ(dialog.bundle_button()->property("role").toString(),
                   QStringLiteral("primary"));
        REQUIRE_EQ(dialog.help_button()->text(),
                   QStringLiteral("Help contents"));
        REQUIRE_EQ(dialog.troubleshoot_button()->text(),
                   QStringLiteral("Why is nothing arriving?"));
        REQUIRE_EQ(dialog.close_button()->text(), QStringLiteral("Close"));
        REQUIRE_EQ(dialog.close_button()->property("role").toString(),
                   QStringLiteral("primary"));
        REQUIRE(dialog.close_button()->isDefault());

        int bundle = 0;
        int help = 0;
        int troubleshoot = 0;
        QObject::connect(&dialog, &SupportDialog::bundle_requested,
                         [&bundle] { ++bundle; });
        QObject::connect(&dialog, &SupportDialog::help_requested,
                         [&help] { ++help; });
        QObject::connect(&dialog, &SupportDialog::troubleshoot_requested,
                         [&troubleshoot] { ++troubleshoot; });
        dialog.bundle_button()->click();
        dialog.help_button()->click();
        dialog.troubleshoot_button()->click();
        REQUIRE_EQ(bundle, 1);
        REQUIRE_EQ(help, 1);
        REQUIRE_EQ(troubleshoot, 1);

        // Initial focus is on Close.
        REQUIRE(vss_test::wait_until(
            [&dialog] { return dialog.focusWidget() != nullptr; }));
        REQUIRE(dialog.focusWidget() == dialog.close_button());

        // Every button has a 24 px target and a name.
        for (QPushButton* button : dialog.findChildren<QPushButton*>())
        {
            if (!button->isVisible())
                continue;
            REQUIRE(button->height() >= 24);
            REQUIRE(button->width() >= 24);
            REQUIRE(!button->accessibleName().isEmpty() ||
                    !button->text().isEmpty());
        }
        REQUIRE(dialog.width() >= 760);
    }

    // With a licence the licence number is a row of its own and is copied.
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_for_anywhere());
        SupportDialog dialog(backend);
        dialog.show();
        QApplication::processEvents();
        auto* number =
            dialog.findChild<QLabel*>(QStringLiteral("supportLicence"));
        REQUIRE(number != nullptr);
        REQUIRE(number->isVisible());
        REQUIRE_EQ(number->text(), QStringLiteral("VSS-2026-000123"));
        REQUIRE_EQ(
            dialog.findChild<QLabel*>(QStringLiteral("supportEdition"))->text(),
            QStringLiteral("Pro"));
        REQUIRE(dialog.details_text().contains(
            QStringLiteral("Licence number: VSS-2026-000123")));
    }
}

TEST_CASE("support page: no browser, no mail program and no network in the new "
          "files")
{
    // Studio copies addresses to the clipboard. These are the pieces that
    // would open a page, start a mail program or reach a server.
    const char* files[] = {
        "apps/vss_gui/src/support/support_dialog.cpp",
        "apps/vss_gui/src/support/support_dialog.hpp",
        "apps/vss_gui/src/support/support_bundle_dialog.cpp",
        "apps/vss_gui/src/support/support_bundle_dialog.hpp",
    };
    const char* forbidden[] = {"QDesktopServices", "QUrl",       "QNetwork",
                               "QProcess",         "mailto",     "QWebEngine",
                               "QTcpSocket",       "QSslSocket", "QHttp"};
    for (const char* relative : files)
    {
        QFile file(QStringLiteral(VSS_SOURCE_DIR "/") +
                   QString::fromLatin1(relative));
        REQUIRE(file.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(file.readAll());
        REQUIRE(!text.isEmpty());
        for (const char* word : forbidden)
            REQUIRE(!text.contains(QString::fromLatin1(word)));
    }
}

TEST_CASE("support page: About shows the edition in its second sentence")
{
    {
        FakeStudioBackend backend;
        support::AboutDialog dialog(backend);
        bool found = false;
        for (QLabel* label : dialog.findChildren<QLabel*>())
        {
            if (label->text().startsWith(QStringLiteral("Version ")) &&
                label->text().endsWith(QStringLiteral(
                    "Free software under the GNU General Public License, "
                    "version 3 or later. Edition: Community.")))
                found = true;
        }
        REQUIRE(found);
        REQUIRE(button_named(dialog, QStringLiteral("Create support bundle")) !=
                nullptr);
        REQUIRE(
            button_named(dialog, QStringLiteral("Create diagnostic bundle")) ==
            nullptr);
    }
    {
        FakeStudioBackend backend;
        backend.set_licence(licence_for_anywhere());
        support::AboutDialog dialog(backend);
        bool found = false;
        for (QLabel* label : dialog.findChildren<QLabel*>())
            found = found ||
                    label->text().endsWith(QStringLiteral("Edition: Pro."));
        REQUIRE(found);
    }
}
