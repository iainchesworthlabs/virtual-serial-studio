//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/plugin_dialogs.hpp"

#include "support/plugin_widgets.hpp"
#include "theme/scaled_font.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::support
{
namespace
{

/// The frame every dialog here is built in: a heading, a body that scrolls so
/// the dialog stays usable at 200 percent text, and a footer.
struct frame
{
    QVBoxLayout* body = nullptr;
    QHBoxLayout* footer = nullptr;
};

frame build_frame(QDialog* dialog, const QString& title, const QString& sub,
                  int width)
{
    dialog->setWindowTitle(title);
    dialog->setAccessibleName(title);
    dialog->setProperty("surface", QStringLiteral("panel"));
    polish(dialog);
    dialog->setMinimumWidth(width);
    dialog->resize(width, 520);

    auto* outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto* header = new QWidget(dialog);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 10);
    header_layout->setSpacing(4);
    // The title can carry a plugin's name, so it is added as plain text.
    auto* heading = make_plain_label(title, header, text_style::title);
    heading->setObjectName(QStringLiteral("dialogHeading"));
    header_layout->addWidget(heading);
    if (!sub.isEmpty())
        header_layout->addWidget(make_muted_label(sub, header));
    outer->addWidget(header);

    auto* scroll = new QScrollArea(dialog);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFocusPolicy(Qt::NoFocus);
    auto* body = new QWidget(scroll);
    frame out;
    out.body = new QVBoxLayout(body);
    out.body->setContentsMargins(24, 4, 24, 16);
    out.body->setSpacing(10);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    QFrame* footer = make_dialog_footer(dialog, out.footer);
    outer->addWidget(footer);
    return out;
}

/// Cancel is the default button and has the focus when the dialog opens.
void focus_cancel(QDialog* dialog, QPushButton* cancel)
{
    cancel->setDefault(true);
    cancel->setAutoDefault(true);
    cancel->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, dialog,
                       [cancel] { cancel->setFocus(Qt::OtherFocusReason); });
}

QPushButton* footer_button(QHBoxLayout* footer, const QString& text,
                           const char* role, const QString& object_name)
{
    QPushButton* button = make_dialog_button(text, role, nullptr);
    button->setParent(footer->parentWidget());
    button->setObjectName(object_name);
    button->setAutoDefault(false);
    footer->addWidget(button);
    return button;
}

void add_row(QFormLayout* form, const QString& label, QWidget* value)
{
    QLabel* name = make_muted_label(label, value->parentWidget());
    form->addRow(name, value);
}

QFormLayout* make_form(QWidget* parent)
{
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(8);
    static_cast<void>(parent);
    return form;
}

} // namespace

const plugin_view* find_plugin(const plugin_snapshot& snapshot,
                               const plugin_view& like)
{
    for (const plugin_view& each : snapshot.plugins)
    {
        if (each.id == like.id && each.folder == like.folder)
            return &each;
    }
    return nullptr;
}

// -- PluginDecisionDialog -----------------------------------------------------

PluginDecisionDialog::PluginDecisionDialog(PluginBackend& backend,
                                           const plugin_view& plugin, mode how,
                                           QWidget* parent)
    : QDialog(parent), backend_(backend), plugin_(plugin), mode_(how)
{
    build();
    connect(&backend_, &PluginBackend::snapshot_changed, this,
            &PluginDecisionDialog::on_snapshot_changed);
}

void PluginDecisionDialog::build()
{
    setObjectName(QStringLiteral("pluginDecisionDialog"));
    frame parts = build_frame(
        this, decision_title(plugin_), {},
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.small")));
    QVBoxLayout* body = parts.body;

    // -- the banner by trust level
    const decision_banner_text banner = decision_banner(plugin_);
    trust_note_ = make_plain_note(this);
    trust_note_->setObjectName(QStringLiteral("decisionTrustNote"));
    trust_note_->set_kind(banner.kind);
    trust_note_->set_text(banner.lines.join(QLatin1Char('\n')));
    body->addWidget(trust_note_);

    if (plugin_.trust == plugin_trust::unknown_key)
    {
        import_key_ = new QPushButton(
            QCoreApplication::translate("PluginDecisionDialog",
                                        "Import the author's key..."),
            this);
        set_role(import_key_, "quiet");
        import_key_->setObjectName(QStringLiteral("decisionImportKey"));
        import_key_->setAutoDefault(false);
        connect(import_key_, &QPushButton::clicked, this,
                [this]
                {
                    reject();
                    Q_EMIT import_key_requested();
                });
        body->addWidget(import_key_, 0, Qt::AlignLeft);
    }

    // -- a changed plugin
    changed_note_ = make_plain_note(this);
    changed_note_->setObjectName(QStringLiteral("decisionChangedNote"));
    changed_note_->set_kind(NoteBanner::kind::warn);
    changed_note_->set_text(QCoreApplication::translate(
        "PluginDecisionDialog",
        "You allowed an earlier version of this plugin. Look at what changed "
        "before you allow this one."));
    changed_note_->setVisible(plugin_.differs_from_approved);
    body->addWidget(changed_note_);

    QStringList more;
    for (const plugin_permission_view& permission : plugin_.permissions)
    {
        if (permission.is_new)
            more << permission_sentence(permission);
    }
    more_note_ = make_plain_note(this);
    more_note_->setObjectName(QStringLiteral("decisionMoreNote"));
    more_note_->set_kind(NoteBanner::kind::warn);
    if (!more.isEmpty())
    {
        more_note_->set_text(
            QCoreApplication::translate("PluginDecisionDialog",
                                        "It asks for more than before:") +
            QLatin1Char('\n') + more.join(QLatin1Char('\n')));
    }
    more_note_->setVisible(plugin_.differs_from_approved && !more.isEmpty());
    body->addWidget(more_note_);

    // -- name, version, fingerprint
    auto* name =
        make_plain_label(plugin_.name, this, text_style::heading, true);
    name->setObjectName(QStringLiteral("decisionName"));
    body->addWidget(name);

    QFormLayout* form = make_form(this);
    auto* version = make_fixed_label(plain_text(plugin_.version), this);
    version->setObjectName(QStringLiteral("decisionVersion"));
    add_row(form,
            QCoreApplication::translate("PluginDecisionDialog", "Version"),
            version);
    fingerprint_ = make_fixed_label(plugin_.fingerprint, this);
    fingerprint_->setObjectName(QStringLiteral("decisionFingerprint"));
    add_row(form,
            QCoreApplication::translate("PluginDecisionDialog", "Fingerprint"),
            fingerprint_);
    if (!plugin_.adds.empty())
    {
        QStringList lines;
        for (const plugin_addition& addition : plugin_.adds)
            lines << addition_text(addition);
        auto* adds = make_plain_label(lines.join(QLatin1Char('\n')), this,
                                      text_style::body, true);
        adds->setObjectName(QStringLiteral("decisionAdds"));
        add_row(
            form,
            QCoreApplication::translate("PluginDecisionDialog", "What it adds"),
            adds);
    }
    body->addLayout(form);

    // -- permissions
    auto* asks = make_plain_label(
        QCoreApplication::translate("PluginDecisionDialog", "It asks to:"),
        this, text_style::heading);
    body->addWidget(asks);
    for (const plugin_permission_view& permission : plugin_.permissions)
    {
        const QString sentence = permission_sentence(permission);
        const bool amber = is_amber_permission(permission.id);
        const QString reason = permission.reason.isEmpty()
                                   ? QString()
                                   : QCoreApplication::translate(
                                         "PluginsDialog", "The plugin says: %1")
                                         .arg(quoted(permission.reason));
        if (permission.required)
        {
            QFrame* row = make_permission_row(
                sentence, amber, true,
                QCoreApplication::translate("PluginDecisionDialog", "Needed"),
                reason, this);
            row->setObjectName(QStringLiteral("permission.") + permission.id);
            body->addWidget(row);
            continue;
        }
        // An optional permission is a check box, ticked by default.
        auto* holder = new QFrame(this);
        if (amber)
        {
            holder->setProperty("note", QStringLiteral("warn"));
            polish(holder);
        }
        auto* holder_layout = new QVBoxLayout(holder);
        holder_layout->setContentsMargins(8, 6, 8, 6);
        holder_layout->setSpacing(2);
        auto* box = new QCheckBox(holder);
        box->setObjectName(QStringLiteral("permission.") + permission.id);
        box->setText(sentence);
        box->setChecked(true);
        box->setAccessibleName(sentence);
        if (!reason.isEmpty())
            box->setAccessibleDescription(reason);
        box->setFont(Theme::instance().font(text_style::body));
        holder_layout->addWidget(box);
        if (!reason.isEmpty())
        {
            QLabel* why =
                make_muted_label(reason, holder, text_style::caption, true);
            why->setContentsMargins(24, 0, 0, 0);
            holder_layout->addWidget(why);
        }
        boxes_.push_back(box);
        body->addWidget(holder);
    }

    // -- isolation
    const decision_isolation_text isolation = decision_isolation(plugin_);
    decide_anyway_ = isolation.decide_anyway;
    isolation_ = make_plain_label(isolation.text, this);
    isolation_->setObjectName(QStringLiteral("decisionIsolation"));
    body->addWidget(isolation_);
    body->addStretch(1);

    // -- buttons
    cancel_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginDecisionDialog", "Cancel"), nullptr,
        QStringLiteral("decisionCancel"));
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    const QString run_text =
        decide_anyway_ ? QCoreApplication::translate("PluginDecisionDialog",
                                                     "Decide anyway")
                       : decision_run_button(plugin_);
    run_ = footer_button(parts.footer, run_text,
                         plugin_.trust == plugin_trust::unsigned_plugin ||
                                 plugin_.trust == plugin_trust::unknown_key
                             ? "danger"
                             : "primary",
                         QStringLiteral("decisionRun"));
    connect(run_, &QPushButton::clicked, this, [this] { run(); });
    setTabOrder(cancel_, run_);
    focus_cancel(this, cancel_);
}

QStringList PluginDecisionDialog::granted() const
{
    QStringList out;
    std::size_t next_box = 0;
    for (const plugin_permission_view& permission : plugin_.permissions)
    {
        if (permission.required)
        {
            out << permission.id;
            continue;
        }
        const bool ticked =
            next_box < boxes_.size() && boxes_[next_box]->isChecked();
        ++next_box;
        if (ticked)
            out << permission.id;
    }
    return out;
}

void PluginDecisionDialog::run()
{
    plugin_decision_request request;
    request.plugin_id = plugin_.id;
    request.hash = plugin_.hash;
    request.key_id = plugin_.key_id;
    request.granted = granted();
    // "Decide anyway" stores the decision and the wish to run the plugin; it
    // stays in Needs more isolation until this computer can keep it apart.
    request.switch_on = mode_ == mode::decide ? true : plugin_.wished_on;
    backend_.decide(request);
    accept();
}

void PluginDecisionDialog::on_snapshot_changed()
{
    // The decision is for one version of one plugin. When that version is
    // replaced or gone, the dialog closes and sends nothing; the list shows
    // the plugin as the host now sees it, and asks again if it must.
    const plugin_snapshot_ptr snapshot = backend_.snapshot();
    const plugin_view* now =
        snapshot ? find_plugin(*snapshot, plugin_) : nullptr;
    if (now == nullptr || now->hash != plugin_.hash)
        reject();
}

// -- PluginKeyImportDialog ----------------------------------------------------

PluginKeyImportDialog::PluginKeyImportDialog(PluginBackend& backend,
                                             const plugin_key_file_view& key,
                                             QWidget* parent)
    : QDialog(parent), backend_(backend), key_(key)
{
    setObjectName(QStringLiteral("pluginKeyImportDialog"));
    const QString author = plain_text(key.author);
    frame parts = build_frame(
        this,
        QCoreApplication::translate("PluginKeyImportDialog",
                                    "Approve the key of %1?")
            .arg(author),
        QCoreApplication::translate(
            "PluginKeyImportDialog",
            "Plugins signed with this key will be shown as signed by %1. Not "
            "verified by Virtual Serial Studio.")
            .arg(author),
        560);
    QVBoxLayout* body = parts.body;

    QFormLayout* form = make_form(this);
    auto* who = make_plain_label(key.author, this, text_style::body, true);
    who->setObjectName(QStringLiteral("keyAuthor"));
    add_row(form,
            QCoreApplication::translate("PluginKeyImportDialog", "Author"),
            who);
    auto* id = make_fixed_label(plain_text(key.key_id), this);
    id->setObjectName(QStringLiteral("keyId"));
    add_row(form,
            QCoreApplication::translate("PluginKeyImportDialog", "Key id"), id);

    auto* print_row = new QWidget(this);
    auto* print_layout = new QHBoxLayout(print_row);
    print_layout->setContentsMargins(0, 0, 0, 0);
    print_layout->setSpacing(6);
    fingerprint_ =
        make_fixed_label(grouped_fingerprint(key.fingerprint), print_row, true);
    fingerprint_->setObjectName(QStringLiteral("keyFingerprint"));
    fingerprint_->setAccessibleName(spoken_fingerprint(key.fingerprint));
    print_layout->addWidget(fingerprint_, 0, Qt::AlignVCenter);
    auto* copy = new CopyButton(print_row);
    copy->setObjectName(QStringLiteral("keyFingerprintCopy"));
    copy->set_copy_text(
        grouped_fingerprint(key.fingerprint),
        QCoreApplication::translate("PluginKeyImportDialog", "fingerprint"));
    connect(copy, &CopyButton::copied, this,
            [this]
            {
                ToastManager::instance().show(
                    this, QCoreApplication::translate("PluginKeyImportDialog",
                                                      "Fingerprint copied."));
            });
    print_layout->addWidget(copy, 0, Qt::AlignVCenter);
    print_layout->addStretch(1);
    add_row(form,
            QCoreApplication::translate("PluginKeyImportDialog", "Fingerprint"),
            print_row);

    auto* file = make_fixed_label(plain_text(key.path), this);
    file->setWordWrap(true);
    file->setObjectName(QStringLiteral("keyFile"));
    add_row(form, QCoreApplication::translate("PluginKeyImportDialog", "File"),
            file);
    body->addLayout(form);

    compare_note_ = make_plain_note(this);
    compare_note_->setObjectName(QStringLiteral("keyCompareNote"));
    compare_note_->set_kind(NoteBanner::kind::warn);
    compare_note_->set_text(QCoreApplication::translate(
        "PluginKeyImportDialog",
        "Compare the fingerprint with the one the author published, on their "
        "website or in a message from them. If they differ, do not approve the "
        "key."));
    body->addWidget(compare_note_);

    inside_note_ = make_plain_note(this);
    inside_note_->setObjectName(QStringLiteral("keyInsideNote"));
    inside_note_->set_kind(NoteBanner::kind::warn);
    inside_note_->set_text(QCoreApplication::translate(
        "PluginKeyImportDialog",
        "This key file came with the plugin. Anyone who changed the plugin "
        "could have changed this file too, so compare the fingerprint with one "
        "from somewhere else."));
    inside_note_->setVisible(key.inside_plugin_folder);
    body->addWidget(inside_note_);

    confirm_ =
        new QCheckBox(QCoreApplication::translate(
                          "PluginKeyImportDialog",
                          "The fingerprint is the one the author published"),
                      this);
    confirm_->setObjectName(QStringLiteral("keyConfirm"));
    confirm_->setFont(Theme::instance().font(text_style::body));
    body->addWidget(confirm_);
    body->addStretch(1);

    cancel_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginKeyImportDialog", "Cancel"), nullptr,
        QStringLiteral("keyCancel"));
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    approve_ = footer_button(parts.footer,
                             QCoreApplication::translate(
                                 "PluginKeyImportDialog", "Approve this key"),
                             "primary", QStringLiteral("keyApprove"));
    approve_->setEnabled(false);
    connect(confirm_, &QCheckBox::toggled, approve_, &QPushButton::setEnabled);
    connect(approve_, &QPushButton::clicked, this, [this] { approve(); });
    setTabOrder(cancel_, confirm_);
    setTabOrder(confirm_, approve_);
    focus_cancel(this, cancel_);
}

void PluginKeyImportDialog::approve()
{
    if (!confirm_->isChecked())
        return;
    backend_.approve_key(key_.path, key_.fingerprint);
    accept();
}

// -- PluginNoIsolationDialog --------------------------------------------------

PluginNoIsolationDialog::PluginNoIsolationDialog(PluginBackend& backend,
                                                 const plugin_view& plugin,
                                                 QWidget* parent)
    : QDialog(parent), backend_(backend), plugin_(plugin)
{
    setObjectName(QStringLiteral("pluginNoIsolationDialog"));
    frame parts = build_frame(this, no_isolation_title(plugin_), {}, 520);
    QVBoxLayout* body = parts.body;

    QStringList lines;
    lines << QCoreApplication::translate(
        "PluginNoIsolationDialog",
        "It will run with your own access to your files, the network and other "
        "programs.");
    if (plugin_.trust == plugin_trust::unsigned_plugin ||
        plugin_.trust == plugin_trust::unknown_key)
    {
        lines << QCoreApplication::translate(
            "PluginNoIsolationDialog",
            "Studio cannot tell who made it or whether it was changed.");
    }
    note_ = make_plain_note(this);
    note_->setObjectName(QStringLiteral("isolationNote"));
    note_->set_kind(NoteBanner::kind::danger);
    note_->set_text(lines.join(QLatin1Char('\n')));
    body->addWidget(note_);

    auto* name =
        make_plain_label(plugin_.name, this, text_style::heading, true);
    body->addWidget(name);
    QFormLayout* form = make_form(this);
    auto* version = make_fixed_label(plain_text(plugin_.version), this);
    add_row(form,
            QCoreApplication::translate("PluginNoIsolationDialog", "Version"),
            version);
    auto* print = make_fixed_label(plugin_.fingerprint, this);
    print->setObjectName(QStringLiteral("isolationFingerprint"));
    add_row(
        form,
        QCoreApplication::translate("PluginNoIsolationDialog", "Fingerprint"),
        print);
    body->addLayout(form);
    body->addWidget(make_plain_label(
        QCoreApplication::translate(
            "PluginNoIsolationDialog",
            "This applies to this version of this plugin only. A changed "
            "plugin asks again."),
        this));

    confirm_ = new QCheckBox(
        QCoreApplication::translate(
            "PluginNoIsolationDialog",
            "I understand that Studio is not keeping this plugin apart"),
        this);
    confirm_->setObjectName(QStringLiteral("isolationConfirm"));
    confirm_->setFont(Theme::instance().font(text_style::body));
    body->addWidget(confirm_);
    body->addStretch(1);

    cancel_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginNoIsolationDialog", "Cancel"),
        nullptr, QStringLiteral("isolationCancel"));
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    run_ = footer_button(parts.footer,
                         QCoreApplication::translate("PluginNoIsolationDialog",
                                                     "Run without isolation"),
                         "danger", QStringLiteral("isolationRun"));
    run_->setEnabled(false);
    connect(confirm_, &QCheckBox::toggled, run_, &QPushButton::setEnabled);
    connect(run_, &QPushButton::clicked, this, [this] { run(); });
    setTabOrder(cancel_, confirm_);
    setTabOrder(confirm_, run_);
    focus_cancel(this, cancel_);
    connect(&backend_, &PluginBackend::snapshot_changed, this,
            &PluginNoIsolationDialog::on_snapshot_changed);
}

void PluginNoIsolationDialog::run()
{
    if (!confirm_->isChecked())
        return;
    backend_.set_no_isolation(plugin_.id, plugin_.hash, true);
    accept();
}

void PluginNoIsolationDialog::on_snapshot_changed()
{
    const plugin_snapshot_ptr snapshot = backend_.snapshot();
    const plugin_view* now =
        snapshot ? find_plugin(*snapshot, plugin_) : nullptr;
    if (now == nullptr || now->hash != plugin_.hash || !now->override_allowed)
        reject();
}

// -- PluginInstallDialog ------------------------------------------------------

PluginInstallDialog::PluginInstallDialog(PluginBackend& backend,
                                         const plugin_inspection_view& found,
                                         QWidget* parent)
    : QDialog(parent), backend_(backend), found_(found)
{
    setObjectName(QStringLiteral("pluginInstallDialog"));
    using problem = plugin_inspection_view::problem;
    const plugin_view& plugin = found_.plugin;
    const bool is_plugin = found_.failure != problem::not_a_plugin;
    frame parts = build_frame(
        this,
        is_plugin
            ? QCoreApplication::translate("PluginInstallDialog", "Add %1?")
                  .arg(plain_text(plugin.name))
            : QCoreApplication::translate("PluginInstallDialog",
                                          "Add a plugin"),
        QCoreApplication::translate(
            "PluginInstallDialog",
            "Studio copies the folder into your own plugin folder. Nothing "
            "runs."),
        560);
    QVBoxLayout* body = parts.body;

    // What is wrong, if anything: the sentences of the spec where it has them.
    QString refusal;
    switch (found_.failure)
    {
    case problem::not_a_plugin:
        refusal = QCoreApplication::translate(
            "PluginsDialog",
            "That folder is not a plugin. A plugin's folder holds a file named "
            "plugin.json.");
        break;
    case problem::already_installed:
        refusal = QCoreApplication::translate(
                      "PluginsDialog",
                      "%1 is already installed for you. Remove it first to add "
                      "a different copy.")
                      .arg(plain_text(plugin.name));
        break;
    case problem::cannot_write:
        refusal = QCoreApplication::translate(
            "PluginsDialog",
            "Your administrator installs the plugins on this computer.");
        break;
    case problem::none:
        if (!found_.can_install)
        {
            refusal = QCoreApplication::translate("PluginInstallDialog",
                                                  "Studio cannot add this "
                                                  "plugin.");
            const plugin_note_text why = note_for(plugin);
            if (why.shown)
                refusal += QLatin1Char('\n') + why.text;
        }
        break;
    }
    note_ = make_plain_note(this);
    note_->setObjectName(QStringLiteral("installNote"));
    if (!refusal.isEmpty())
    {
        note_->set_kind(NoteBanner::kind::danger);
        note_->set_text(refusal);
    }
    else
    {
        // It can be added: what it will be, in the spec's own words.
        const plugin_note_text what = note_for(plugin);
        note_->set_kind(what.shown ? what.kind : NoteBanner::kind::info);
        note_->set_text(what.shown ? what.text
                                   : QCoreApplication::translate(
                                         "PluginInstallDialog",
                                         "Nothing is wrong with it."));
    }
    body->addWidget(note_);

    if (is_plugin && found_.failure != problem::cannot_write)
    {
        auto* name =
            make_plain_label(plugin.name, this, text_style::heading, true);
        body->addWidget(name);
        QFormLayout* form = make_form(this);
        const auto row = [&](const QString& label, const QString& value,
                             const QString& object_name, bool fixed)
        {
            if (plain_text(value).isEmpty())
                return;
            QLabel* text =
                fixed ? make_fixed_label(plain_text(value), this)
                      : make_plain_label(value, this, text_style::body, true);
            text->setObjectName(object_name);
            if (fixed)
                text->setWordWrap(true);
            add_row(form, label, text);
        };
        row(QCoreApplication::translate("PluginsDialog", "Version"),
            plugin.version, QStringLiteral("installVersion"), true);
        row(QCoreApplication::translate("PluginsDialog", "Made by"),
            plugin.trust == plugin_trust::official ? display_author(plugin)
                                                   : plugin.author,
            QStringLiteral("installAuthor"), false);
        row(QCoreApplication::translate("PluginsDialog", "Signature"),
            signature_sentence(plugin), QStringLiteral("installSignature"),
            false);
        row(QCoreApplication::translate("PluginsDialog", "Signing key"),
            plugin.key_id, QStringLiteral("installKey"), true);
        row(QCoreApplication::translate("PluginsDialog", "Fingerprint"),
            plugin.fingerprint, QStringLiteral("installFingerprint"), true);
        body->addLayout(form);
    }

    // Installing for everyone needs rights Studio never asks for: the command
    // is shown as text to copy.
    const QString folder = found_.folder;
    command_ = QStringLiteral("vss_cli plugin install ") +
               (folder.contains(QLatin1Char(' '))
                    ? QLatin1Char('"') + folder + QLatin1Char('"')
                    : folder) +
               QStringLiteral(" --for-everyone");
    if (found_.failure != problem::not_a_plugin)
    {
        body->addWidget(make_plain_label(
            QCoreApplication::translate(
                "PluginInstallDialog",
                "To install it for everyone on this computer, an administrator "
                "runs this command. Studio never asks for administrator "
                "rights."),
            this));
        auto* code = new CodeBlock(this);
        code->setObjectName(QStringLiteral("installCommand"));
        code->set_text(plain_text(command_),
                       QCoreApplication::translate("PluginInstallDialog",
                                                   "Install command"));
        body->addWidget(code);
    }
    body->addStretch(1);

    cancel_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginInstallDialog", "Cancel"), nullptr,
        QStringLiteral("installCancel"));
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    add_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginInstallDialog", "Add plugin"),
        "primary", QStringLiteral("installAdd"));
    add_->setEnabled(found_.can_install &&
                     found_.failure == plugin_inspection_view::problem::none);
    connect(add_, &QPushButton::clicked, this, [this] { add(); });
    setTabOrder(cancel_, add_);
    focus_cancel(this, cancel_);
}

void PluginInstallDialog::add()
{
    if (!add_->isEnabled())
        return;
    backend_.add_plugin(found_.folder);
    accept();
}

// -- PluginRemoveDialog -------------------------------------------------------

PluginRemoveDialog::PluginRemoveDialog(PluginBackend& backend,
                                       const plugin_view& plugin,
                                       QWidget* parent)
    : QDialog(parent), backend_(backend), plugin_(plugin)
{
    setObjectName(QStringLiteral("pluginRemoveDialog"));
    frame parts = build_frame(
        this,
        QCoreApplication::translate("PluginsDialog", "Remove %1?")
            .arg(plain_text(plugin_.name)),
        QCoreApplication::translate(
            "PluginsDialog",
            "Studio deletes the plugin's folder. Setups that use it "
            "keep their parts, which stop working until the plugin is "
            "installed again. Its saved data is kept unless you tick the box."),
        520);
    delete_data_ =
        new QCheckBox(QCoreApplication::translate("PluginsDialog",
                                                  "Also delete its saved data"),
                      this);
    delete_data_->setObjectName(QStringLiteral("removeDeleteData"));
    delete_data_->setFont(Theme::instance().font(text_style::body));
    parts.body->addWidget(delete_data_);
    parts.body->addStretch(1);

    cancel_ = footer_button(
        parts.footer, QCoreApplication::translate("PluginsDialog", "Cancel"),
        nullptr, QStringLiteral("removeCancel"));
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    remove_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginsDialog", "Remove plugin"), "danger",
        QStringLiteral("removeConfirm"));
    connect(remove_, &QPushButton::clicked, this, [this] { remove(); });
    setTabOrder(cancel_, delete_data_);
    setTabOrder(delete_data_, remove_);
    focus_cancel(this, cancel_);
}

void PluginRemoveDialog::remove()
{
    backend_.remove_plugin(plugin_.id, plugin_.hash, delete_data_->isChecked());
    accept();
}

// -- Settings for a new instance ---------------------------------------------

PluginInstanceDialog::PluginInstanceDialog(const plugin_view& plugin,
                                           const plugin_addition& point,
                                           const QStringList& place_names,
                                           QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("pluginInstanceDialog"));
    frame parts = build_frame(
        this,
        QCoreApplication::translate("PluginInstanceDialog", "Add %1")
            .arg(plain_text(point.type_name)),
        QCoreApplication::translate("PluginInstanceDialog",
                                    "%1 needs these settings first.")
            .arg(plain_text(plugin.name)),
        520);
    QVBoxLayout* body = parts.body;
    QFormLayout* form = make_form(this);
    for (const plugin_parameter_ask& ask : point.asks)
    {
        QWidget* field = nullptr;
        if (ask.place_name)
        {
            auto* box = new QComboBox(this);
            box->setEditable(true);
            box->addItems(place_names);
            box->setCurrentIndex(-1);
            box->setInsertPolicy(QComboBox::NoInsert);
            connect(box, &QComboBox::currentTextChanged, this,
                    [this] { update_add(); });
            field = box;
        }
        else
        {
            auto* edit = new QLineEdit(this);
            connect(edit, &QLineEdit::textChanged, this,
                    [this] { update_add(); });
            field = edit;
        }
        field->setObjectName(QStringLiteral("instanceField"));
        field->setProperty("settingId", ask.id);
        field->setAccessibleName(plain_text(ask.label));
        if (!ask.help.isEmpty())
            field->setAccessibleDescription(plain_text(ask.help));
        fields_.append({ask.id, field});
        auto* name = make_plain_label(ask.label, this, text_style::body, true);
        form->addRow(name, field);
        if (!ask.help.isEmpty())
            form->addRow(
                QString(),
                make_muted_label(ask.help, this, text_style::caption, true));
    }
    body->addLayout(form);
    body->addWidget(make_muted_label(
        QCoreApplication::translate(
            "PluginInstanceDialog",
            "These words come from the plugin."),
        this));
    body->addStretch(1);

    cancel_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginInstanceDialog", "Cancel"), nullptr,
        QStringLiteral("instanceCancel"));
    add_ = footer_button(
        parts.footer, QCoreApplication::translate("PluginInstanceDialog", "Add"),
        "primary", QStringLiteral("instanceAdd"));
    connect(cancel_, &QPushButton::clicked, this, &QDialog::reject);
    connect(add_, &QPushButton::clicked, this, &QDialog::accept);
    add_->setEnabled(false);
    if (!fields_.isEmpty())
        setTabOrder(fields_.last().second, cancel_);
    setTabOrder(cancel_, add_);
    if (!fields_.isEmpty())
        fields_.first().second->setFocus(Qt::OtherFocusReason);
}

QMap<QString, QString> PluginInstanceDialog::values() const
{
    QMap<QString, QString> out;
    for (const auto& each : fields_)
    {
        QString text;
        if (const auto* box = qobject_cast<QComboBox*>(each.second))
            text = box->currentText();
        else if (const auto* line = qobject_cast<QLineEdit*>(each.second))
            text = line->text();
        out.insert(each.first, text.trimmed());
    }
    return out;
}

QWidget* PluginInstanceDialog::field(const QString& id) const
{
    for (const auto& each : fields_)
    {
        if (each.first == id)
            return each.second;
    }
    return nullptr;
}

void PluginInstanceDialog::update_add()
{
    // The fields are made before the button is.
    if (add_ == nullptr)
        return;
    bool all = true;
    const QMap<QString, QString> given = values();
    for (auto it = given.begin(); it != given.end(); ++it)
        all = all && !it.value().isEmpty();
    add_->setEnabled(all);
}

// -- A plugin shows a code ----------------------------------------------------

namespace
{

/// "4 min 30 s", "5 min", "45 s".
QString time_text(int seconds)
{
    const int clamped = std::max(0, seconds);
    const int minutes = clamped / 60;
    const int rest = clamped % 60;
    if (minutes > 0 && rest > 0)
        return QCoreApplication::translate("PluginSecretDialog", "%1 min %2 s")
            .arg(minutes)
            .arg(rest);
    if (minutes > 0)
        return QCoreApplication::translate("PluginSecretDialog", "%1 min")
            .arg(minutes);
    return QCoreApplication::translate("PluginSecretDialog", "%1 s")
        .arg(clamped);
}

/// A box with a border and a heading that sets what a plugin said apart from
/// Studio's own text.
QFrame* make_plugin_says_box(QWidget* parent, const QString& heading,
                             QVBoxLayout*& inside)
{
    auto* box = new QFrame(parent);
    box->setProperty("note", QStringLiteral("info"));
    polish(box);
    inside = new QVBoxLayout(box);
    inside->setContentsMargins(12, 10, 12, 10);
    inside->setSpacing(6);
    inside->addWidget(make_muted_label(heading, box, text_style::caption));
    return box;
}

/// The characters of `text` with a space between each, for a screen reader.
QString spaced(const QString& text)
{
    QString out;
    out.reserve(text.size() * 2);
    for (const QChar each : text)
    {
        if (!out.isEmpty())
            out += QLatin1Char(' ');
        out += each;
    }
    return out;
}

} // namespace

PluginSecretDialog::PluginSecretDialog(const plugin_secret_request& request,
                                       int remaining_seconds, QWidget* parent)
    : QDialog(parent), remaining_(remaining_seconds)
{
    setObjectName(QStringLiteral("pluginSecretDialog"));
    frame parts = build_frame(
        this,
        QCoreApplication::translate("PluginSecretDialog", "Code from a plugin"),
        QCoreApplication::translate("PluginSecretDialog",
                                    "%1 asks you to read this.")
            .arg(plain_text(request.plugin_name)),
        480);
    QVBoxLayout* body = parts.body;

    auto* id =
        make_muted_label(request.plugin_id, this, text_style::caption, true);
    id->setObjectName(QStringLiteral("secretPluginId"));
    body->addWidget(id);

    QVBoxLayout* inside = nullptr;
    QFrame* box = make_plugin_says_box(
        this,
        QCoreApplication::translate("PluginSecretDialog", "The plugin says"),
        inside);
    box->setObjectName(QStringLiteral("secretBox"));
    label_ = make_plain_label(request.label, box, text_style::body, true);
    label_->setObjectName(QStringLiteral("secretLabel"));
    inside->addWidget(label_);

    code_ = new QPlainTextEdit(box);
    code_->setObjectName(QStringLiteral("secretCode"));
    code_->setReadOnly(true);
    code_->setUndoRedoEnabled(false);
    code_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    code_->setWordWrapMode(QTextOption::WrapAnywhere);
    code_->setFont(scaled_font(Theme::instance().font(text_style::data), 1.5));
    code_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    code_->setPlainText(request.text);
    // Room for the whole code at the large size, up to a few lines.
    const int line = QFontMetrics(code_->font()).lineSpacing();
    const int advance = std::max(
        1, QFontMetrics(code_->font()).horizontalAdvance(QLatin1Char('0')));
    const int per_line = std::max(8, 380 / advance);
    const int lines =
        std::clamp(static_cast<int>(request.text.size()) / per_line + 1, 1, 8);
    code_->setFixedHeight(line * lines + 16);
    code_->setAccessibleName(plain_text(request.label));
    code_->setAccessibleDescription(spaced(request.text));
    inside->addWidget(code_);
    body->addWidget(box);

    time_ = make_plain_label({}, this);
    time_->setObjectName(QStringLiteral("secretTime"));
    body->addWidget(time_);
    body->addWidget(make_muted_label(
        QCoreApplication::translate(
            "PluginSecretDialog",
            "Studio does not keep this code. Only give it to someone you "
            "mean to."),
        this));
    body->addStretch(1);

    copy_ = footer_button(
        parts.footer, QCoreApplication::translate("PluginSecretDialog", "Copy"),
        "quiet", QStringLiteral("secretCopy"));
    close_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginSecretDialog", "Close"), "primary",
        QStringLiteral("secretClose"));
    close_->setDefault(true);
    close_->setAutoDefault(true);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    // The text to copy is held for the button only, and is dropped with the
    // code.
    copy_text_ = request.text;
    copied_clock_ = new QTimer(this);
    copied_clock_->setSingleShot(true);
    copied_clock_->setInterval(2000);
    connect(copied_clock_, &QTimer::timeout, this,
            [this]
            {
                copy_->setText(
                    QCoreApplication::translate("PluginSecretDialog", "Copy"));
            });
    connect(copy_, &QPushButton::clicked, this,
            [this]
            {
                if (copy_text_.isEmpty())
                    return;
                QGuiApplication::clipboard()->setText(copy_text_);
                seen_ = true;
                copy_->setText(QCoreApplication::translate("PluginSecretDialog",
                                                           "Copied"));
                copied_clock_->start();
            });
    setTabOrder(code_, copy_);
    setTabOrder(copy_, close_);
    close_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, this,
                       [this] { close_->setFocus(Qt::OtherFocusReason); });

    update_time_line();
    clock_ = new QTimer(this);
    clock_->setInterval(1000);
    connect(clock_, &QTimer::timeout, this, &PluginSecretDialog::tick);
    if (remaining_ > 0)
        clock_->start();
    else
        remove_code();
}

PluginSecretDialog::~PluginSecretDialog()
{
    remove_code();
}

void PluginSecretDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    shown_ = true;
}

void PluginSecretDialog::done(int result)
{
    // Closing it, by Close, Escape or the window's own button, while the code
    // is still there, is having seen it.
    if (!abandoned_ && shown_ && remaining_ > 0)
        seen_ = true;
    remove_code();
    QDialog::done(result);
}

void PluginSecretDialog::tick()
{
    if (remaining_ <= 0)
        return;
    --remaining_;
    update_time_line();
    // Announced at a minute, half a minute, ten seconds and the end, not
    // every second.
    if (remaining_ == 60 || remaining_ == 30 || remaining_ == 10 ||
        remaining_ == 0)
        announce_polite(time_, time_->text());
    if (remaining_ <= 0)
    {
        clock_->stop();
        remove_code();
        Q_EMIT timed_out_now();
    }
}

void PluginSecretDialog::update_time_line()
{
    if (remaining_ > 0)
    {
        time_->setText(
            QCoreApplication::translate("PluginSecretDialog", "Good for %1.")
                .arg(time_text(remaining_)));
    }
    else
    {
        time_->setText(QCoreApplication::translate(
            "PluginSecretDialog",
            "This code is no longer good. Ask the plugin for a new one."));
    }
}

void PluginSecretDialog::remove_code()
{
    if (code_ != nullptr)
    {
        code_->clear();
        code_->setVisible(false);
        code_->setAccessibleDescription({});
    }
    copy_text_.clear();
    if (copy_ != nullptr)
        copy_->setEnabled(false);
    if (time_ != nullptr && remaining_ <= 0)
        update_time_line();
}

// -- A plugin asks for a control session --------------------------------------

QString control_scope_sentence(const QString& scope, bool* amber)
{
    const auto mark = [amber](bool value)
    {
        if (amber != nullptr)
            *amber = value;
    };
    mark(false);
    if (scope == QLatin1String("monitor"))
        return QCoreApplication::translate("PluginControlDialog",
                                           "See the ports, links and their "
                                           "state");
    if (scope == QLatin1String("monitor.data"))
    {
        mark(true);
        return QCoreApplication::translate(
            "PluginControlDialog", "See the bytes that pass through the ports");
    }
    if (scope == QLatin1String("control.ports"))
        return QCoreApplication::translate("PluginControlDialog",
                                           "Add, change and remove ports and "
                                           "links");
    if (scope == QLatin1String("control.data"))
    {
        mark(true);
        return QCoreApplication::translate("PluginControlDialog",
                                           "Send bytes into the ports");
    }
    if (scope == QLatin1String("control.faults"))
        return QCoreApplication::translate("PluginControlDialog",
                                           "Cause faults on ports and links");
    if (scope == QLatin1String("control.apply"))
        return QCoreApplication::translate("PluginControlDialog",
                                           "Apply a whole setup");
    if (scope == QLatin1String("record"))
    {
        mark(true);
        return QCoreApplication::translate("PluginControlDialog",
                                           "Start and stop recordings");
    }
    mark(true);
    return QCoreApplication::translate("PluginControlDialog",
                                       "%1. Studio does not describe this one.")
        .arg(plain_text(scope));
}

PluginControlDialog::PluginControlDialog(const plugin_control_request& request,
                                         QWidget* parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("pluginControlDialog"));
    frame parts = build_frame(
        this,
        QCoreApplication::translate("PluginControlDialog",
                                    "Allow remote control?"),
        QCoreApplication::translate(
            "PluginControlDialog",
            "%1 asks to use this computer's ports for another computer.")
            .arg(plain_text(request.plugin_name)),
        560);
    QVBoxLayout* body = parts.body;

    const QString peer = plain_text(request.peer).trimmed();
    auto* who = make_plain_label(
        peer.isEmpty()
            ? QCoreApplication::translate("PluginControlDialog",
                                          "It did not say which computer.")
            : QCoreApplication::translate("PluginControlDialog",
                                          "It says the other computer is: %1")
                  .arg(peer),
        this, text_style::body, true);
    who->setObjectName(QStringLiteral("controlWho"));
    body->addWidget(who);

    const QString reason = plain_text(request.reason).trimmed();
    if (!reason.isEmpty())
    {
        QVBoxLayout* inside = nullptr;
        QFrame* box = make_plugin_says_box(
            this,
            QCoreApplication::translate("PluginControlDialog",
                                        "The plugin's own words"),
            inside);
        box->setObjectName(QStringLiteral("controlBox"));
        auto* why =
            make_plain_label(quoted(reason), box, text_style::body, true);
        why->setObjectName(QStringLiteral("controlWhy"));
        inside->addWidget(why);
        body->addWidget(box);
    }

    body->addWidget(make_plain_label(
        QCoreApplication::translate("PluginControlDialog",
                                    "It asks to be allowed to:"),
        this, text_style::heading));
    QStringList seen;
    bool any_data = false;
    for (const QString& scope : request.scopes)
    {
        if (seen.contains(scope))
            continue;
        seen << scope;
        bool amber = false;
        const QString sentence = control_scope_sentence(scope, &amber);
        any_data = any_data || amber;
        sentences_ << sentence;
        QFrame* row = make_permission_row(sentence, amber, true, {}, {}, this);
        row->setObjectName(QStringLiteral("controlScope"));
        row->setProperty("scope", scope);
        if (amber)
            row->setAccessibleDescription(QCoreApplication::translate(
                "PluginControlDialog",
                "This lets the other side see or send data."));
        body->addWidget(row);
    }
    if (any_data)
    {
        data_note_ = make_plain_note(this);
        data_note_->setObjectName(QStringLiteral("controlDataNote"));
        data_note_->set_kind(NoteBanner::kind::warn);
        data_note_->set_text(QCoreApplication::translate(
            "PluginControlDialog",
            "The other computer can read or write what your ports carry."));
        body->addWidget(data_note_);
    }

    const int life = std::max(0, request.life_seconds);
    const QString length =
        (life % 3600 == 0 && life >= 3600)
            ? QCoreApplication::translate("PluginControlDialog", "%n hour(s)",
                                          nullptr, life / 3600)
            : QCoreApplication::translate("PluginControlDialog", "%n minute(s)",
                                          nullptr,
                                          std::max(1, (life + 59) / 60));
    life_ = make_plain_label(
        QCoreApplication::translate(
            "PluginControlDialog",
            "This lasts %1, or until the plugin stops. You are asked again "
            "each time.")
            .arg(length),
        this);
    life_->setObjectName(QStringLiteral("controlLife"));
    body->addWidget(life_);
    body->addStretch(1);

    refuse_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginControlDialog", "Refuse"), nullptr,
        QStringLiteral("controlRefuse"));
    allow_ = footer_button(
        parts.footer,
        QCoreApplication::translate("PluginControlDialog", "Allow"), "primary",
        QStringLiteral("controlAllow"));
    connect(refuse_, &QPushButton::clicked, this, &QDialog::reject);
    connect(allow_, &QPushButton::clicked, this, &QDialog::accept);
    setTabOrder(refuse_, allow_);
    focus_cancel(this, refuse_);
}

} // namespace vss::gui::support
