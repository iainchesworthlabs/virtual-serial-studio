//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Licence page (docs/gui-design/docs/screens/09-licence-and-support.md):
// which edition this is, what the licence includes, and importing or
// removing a licence file. It shows what `StudioBackend::licence()` reports
// and never reads a licence file or decides a state itself.
//

#pragma once

#include "backend/studio_backend.hpp"
#include "support/licence_text.hpp"
#include "widgets/chips.hpp"
#include "widgets/notes.hpp"

#include <QDialog>
#include <QString>

#include <functional>
#include <optional>
#include <vector>

class QDragEnterEvent;
class QDropEvent;
class QFormLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace vss::gui
{
class CopyButton;
class Disclosure;
} // namespace vss::gui

namespace vss::gui::support
{

/// The shared chip kind that draws a licence tone.
StatusChip::kind chip_kind(licence_tone tone);
/// The shared banner kind that draws a licence note tone.
NoteBanner::kind banner_kind(licence_note_tone tone);

/// "Renew your licence": the address to write to and the licence number, both
/// copied to the clipboard. Studio opens nothing and sends nothing.
class RenewDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit RenewDialog(const QString& licence_id, QWidget* parent = nullptr);

    QPushButton* copy_number_button() const noexcept { return copy_number_; }
    QPushButton* copy_address_button() const noexcept { return copy_address_; }
    QPushButton* close_button() const noexcept { return close_; }

private:
    QString licence_id_;
    QPushButton* copy_number_ = nullptr;
    QPushButton* copy_address_ = nullptr;
    QPushButton* close_ = nullptr;
};

class LicenceDialog final : public QDialog
{
    Q_OBJECT

public:
    /// Chooses the file to import and returns its path; empty when the
    /// person chose none. The default opens the native file dialog.
    using file_picker = std::function<QString()>;

    explicit LicenceDialog(StudioBackend& backend, QWidget* parent = nullptr);

    /// Replaces the native file dialog, so a test can supply a path.
    void set_file_picker(file_picker picker) { picker_ = std::move(picker); }

    /// "Import licence...": picks a file and imports it.
    void choose_and_import();
    /// Imports the file at `path`, as the picker and a drop do.
    void import_file(const QString& path);
    /// Asks, then removes the licence file `file_name`. "Remove licence" in
    /// the footer and "Remove" in the list of other licences both end here.
    void remove_file(const QString& file_name);
    /// Opens "How to renew".
    void show_renew();

    QPushButton* import_button() const noexcept { return import_; }
    /// Hidden while no licence of the user's own is in use.
    QPushButton* remove_button() const noexcept { return remove_; }
    QPushButton* close_button() const noexcept { return close_; }
    QLabel* edition_heading() const noexcept { return edition_; }
    StatusChip* state_chip_widget() const noexcept { return state_chip_; }
    /// The "Status" sentence, a polite live region.
    QLabel* status_sentence() const noexcept { return status_; }
    NoteBanner* state_banner() const noexcept { return state_note_; }
    NoteBanner* import_banner() const noexcept { return import_note_; }
    Disclosure* other_licences() const noexcept { return others_; }

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void build();
    void refresh();
    void on_licence_changed();
    void refresh_others(const licence_view& view);
    void show_import_failure(const licence_import_result& result,
                             const QString& path);
    void set_state_note(const std::optional<licence_banner>& note);
    void set_import_note(const std::optional<licence_banner>& note);
    void set_row_visible(QWidget* field, bool visible);

    struct feature_row
    {
        QString feature;
        StatusChip* chip = nullptr;
        QLabel* why = nullptr;
    };

    StudioBackend& backend_;
    file_picker picker_;
    QString computer_name_;
    QString licence_id_;
    QString removable_file_;
    bool importing_ = false;

    QLabel* edition_ = nullptr;
    StatusChip* state_chip_ = nullptr;
    QFormLayout* form_ = nullptr;
    QLabel* edition_value_ = nullptr;
    QLabel* licensee_ = nullptr;
    QWidget* number_row_ = nullptr;
    QLabel* number_ = nullptr;
    CopyButton* number_copy_ = nullptr;
    QLabel* term_ = nullptr;
    QLabel* status_ = nullptr;
    QWidget* seats_row_ = nullptr;
    QLabel* seats_ = nullptr;
    QLabel* seats_note_ = nullptr;
    QWidget* computers_row_ = nullptr;
    QLabel* computers_ = nullptr;
    QLabel* this_computer_ = nullptr;
    QLabel* includes_ = nullptr;
    NoteBanner* state_note_ = nullptr;
    NoteBanner* import_note_ = nullptr;
    std::vector<feature_row> features_;
    Disclosure* others_ = nullptr;
    QPushButton* remove_ = nullptr;
    QPushButton* import_ = nullptr;
    QPushButton* close_ = nullptr;
    std::optional<licence_banner> shown_state_note_;
};

} // namespace vss::gui::support
