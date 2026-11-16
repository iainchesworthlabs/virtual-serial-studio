//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dialogs of the Plugins page (docs/gui-design/docs/screens/10-plugins.md):
// the decision about a plugin, the import of an author's key, running a plugin
// without isolation, and removing a plugin. Each one is modal, starts with
// focus on Cancel, has Cancel as its default button, and sends the backend
// nothing until the person presses its action. A dialog that is open for a
// plugin version that is replaced closes without sending anything.
//

#pragma once

#include "backend/plugin_backend.hpp"
#include "support/plugin_text.hpp"

#include <QDialog>
#include <QString>

#include <vector>

class QCheckBox;
class QLabel;
class QPushButton;

namespace vss::gui
{
class NoteBanner;
}

namespace vss::gui::support
{

/// "Decide about a plugin" (width 640). Opens from "Review and decide", from
/// switching on a plugin that needs a decision, and from "Change what it may
/// do...".
class PluginDecisionDialog final : public QDialog
{
    Q_OBJECT

public:
    enum class mode
    {
        /// The plugin needs a decision: the run button switches it on.
        decide,
        /// "Change what it may do...": the switch is left as it is.
        permissions
    };

    PluginDecisionDialog(PluginBackend& backend, const plugin_view& plugin,
                         mode how, QWidget* parent = nullptr);

    QPushButton* cancel_button() const noexcept { return cancel_; }
    QPushButton* run_button() const noexcept { return run_; }
    QPushButton* import_key_button() const noexcept { return import_key_; }
    NoteBanner* trust_banner() const noexcept { return trust_note_; }
    NoteBanner* changed_banner() const noexcept { return changed_note_; }
    NoteBanner* more_banner() const noexcept { return more_note_; }
    QLabel* isolation_label() const noexcept { return isolation_; }
    QLabel* fingerprint_label() const noexcept { return fingerprint_; }
    /// The check boxes of the optional permissions, in the plugin's order.
    const std::vector<QCheckBox*>& optional_boxes() const noexcept
    {
        return boxes_;
    }
    /// The plugin version this dialog was opened for.
    const plugin_view& plugin() const noexcept { return plugin_; }

    /// What the run button sends: the needed permissions and the ticked ones.
    QStringList granted() const;
    /// Presses the run button.
    void run();

Q_SIGNALS:
    /// "Import the author's key..." was pressed. The dialog closes first.
    void import_key_requested();

private:
    void build();
    void on_snapshot_changed();

    PluginBackend& backend_;
    plugin_view plugin_;
    mode mode_;
    bool decide_anyway_ = false;
    NoteBanner* trust_note_ = nullptr;
    NoteBanner* changed_note_ = nullptr;
    NoteBanner* more_note_ = nullptr;
    QLabel* isolation_ = nullptr;
    QLabel* fingerprint_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* run_ = nullptr;
    QPushButton* import_key_ = nullptr;
    std::vector<QCheckBox*> boxes_;
};

/// "Approve the key of <author name>?" (width 560).
class PluginKeyImportDialog final : public QDialog
{
    Q_OBJECT

public:
    PluginKeyImportDialog(PluginBackend& backend,
                          const plugin_key_file_view& key,
                          QWidget* parent = nullptr);

    QPushButton* cancel_button() const noexcept { return cancel_; }
    QPushButton* approve_button() const noexcept { return approve_; }
    QCheckBox* confirm_box() const noexcept { return confirm_; }
    QLabel* fingerprint_label() const noexcept { return fingerprint_; }
    NoteBanner* compare_banner() const noexcept { return compare_note_; }
    NoteBanner* inside_banner() const noexcept { return inside_note_; }

    void approve();

private:
    PluginBackend& backend_;
    plugin_key_file_view key_;
    QLabel* fingerprint_ = nullptr;
    NoteBanner* compare_note_ = nullptr;
    NoteBanner* inside_note_ = nullptr;
    QCheckBox* confirm_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* approve_ = nullptr;
};

/// "Run <plugin name> without isolation?" (width 520).
class PluginNoIsolationDialog final : public QDialog
{
    Q_OBJECT

public:
    PluginNoIsolationDialog(PluginBackend& backend, const plugin_view& plugin,
                            QWidget* parent = nullptr);

    QPushButton* cancel_button() const noexcept { return cancel_; }
    QPushButton* run_button() const noexcept { return run_; }
    QCheckBox* confirm_box() const noexcept { return confirm_; }
    NoteBanner* banner() const noexcept { return note_; }
    const plugin_view& plugin() const noexcept { return plugin_; }

    void run();

private:
    void on_snapshot_changed();

    PluginBackend& backend_;
    plugin_view plugin_;
    NoteBanner* note_ = nullptr;
    QCheckBox* confirm_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* run_ = nullptr;
};

/// "Add <plugin name>?" (width 560): what Studio found in the folder the user
/// chose, before anything is copied. A plugin that cannot be used, or that is
/// installed already, cannot be confirmed.
class PluginInstallDialog final : public QDialog
{
    Q_OBJECT

public:
    PluginInstallDialog(PluginBackend& backend,
                        const plugin_inspection_view& found,
                        QWidget* parent = nullptr);

    QPushButton* cancel_button() const noexcept { return cancel_; }
    QPushButton* add_button() const noexcept { return add_; }
    NoteBanner* banner() const noexcept { return note_; }
    /// The command for installing it for everyone, as text to copy.
    QString everyone_command() const noexcept { return command_; }

    void add();

private:
    PluginBackend& backend_;
    plugin_inspection_view found_;
    QString command_;
    NoteBanner* note_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* add_ = nullptr;
};

/// "Remove <plugin name>?": Cancel is the default.
class PluginRemoveDialog final : public QDialog
{
    Q_OBJECT

public:
    PluginRemoveDialog(PluginBackend& backend, const plugin_view& plugin,
                       QWidget* parent = nullptr);

    QPushButton* cancel_button() const noexcept { return cancel_; }
    QPushButton* remove_button() const noexcept { return remove_; }
    QCheckBox* delete_data_box() const noexcept { return delete_data_; }

    void remove();

private:
    PluginBackend& backend_;
    plugin_view plugin_;
    QCheckBox* delete_data_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* remove_ = nullptr;
};

/// The plugin in the current snapshot with this id and folder, or null. A
/// dialog uses it to see that the version it was opened for is still there.
const plugin_view* find_plugin(const plugin_snapshot& snapshot,
                               const plugin_view& like);

} // namespace vss::gui::support
