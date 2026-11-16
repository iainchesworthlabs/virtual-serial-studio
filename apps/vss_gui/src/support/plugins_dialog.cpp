//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/plugins_dialog.hpp"

#include "support/licence_text.hpp"
#include "support/plugin_dialogs.hpp"
#include "support/plugin_widgets.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"

#include <vss/core/profile.hpp>

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>

namespace vss::gui::support
{
namespace
{

constexpr int kListWidth = 320;
constexpr int kMaxLogLines = 200;

QString instance_state_text(plugin_instance_state state)
{
    switch (state)
    {
    case plugin_instance_state::not_running:
        return QCoreApplication::translate("PluginsDialog", "Not running");
    case plugin_instance_state::waiting:
        return QCoreApplication::translate("PluginsDialog", "Waiting");
    case plugin_instance_state::starting:
        return QCoreApplication::translate("PluginsDialog", "Starting");
    case plugin_instance_state::running:
        return QCoreApplication::translate("PluginsDialog", "Running");
    case plugin_instance_state::stopping:
        return QCoreApplication::translate("PluginsDialog", "Stopping");
    case plugin_instance_state::refused:
        return QCoreApplication::translate("PluginsDialog", "Refused");
    }
    return {};
}

QPushButton* make_button(const QString& text, const char* role, QWidget* parent,
                         const QString& object_name = {})
{
    QPushButton* button = make_dialog_button(text, role, parent);
    button->setAutoDefault(false);
    if (!object_name.isEmpty())
        button->setObjectName(object_name);
    return button;
}

QString open_path_text(const plugin_view& plugin)
{
    return plugin.folder;
}

} // namespace

// -- one row of the list ------------------------------------------------------

class PluginsDialog::Row final : public QWidget
{
public:
    Row(bool copy, QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(copy ? 28 : 8, 6, 8, 6);
        layout->setSpacing(8);

        auto* texts = new QVBoxLayout();
        texts->setSpacing(2);
        name = new ElidedLabel(this);
        name->setFont(Theme::instance().font(text_style::label));
        name->setObjectName(QStringLiteral("pluginRowName"));
        texts->addWidget(name);
        second = new ElidedLabel(this);
        second->setFont(Theme::instance().font(text_style::caption));
        second->setProperty("muted", true);
        second->setObjectName(QStringLiteral("pluginRowSecond"));
        texts->addWidget(second);
        layout->addLayout(texts, 1);

        // The chip above and the switch below it, so that the name keeps the
        // room in a list that is 320 wide.
        auto* state = new QVBoxLayout();
        state->setSpacing(4);
        chip = new StatusChip(this);
        chip->set_small(true);
        chip->setObjectName(QStringLiteral("pluginRowChip"));
        state->addWidget(chip, 0, Qt::AlignRight);

        if (!copy)
        {
            auto* toggle = new QHBoxLayout();
            toggle->setSpacing(6);
            toggle->addStretch(1);
            word = new QLabel(this);
            word->setFont(Theme::instance().font(text_style::caption));
            word->setObjectName(QStringLiteral("pluginRowWord"));
            toggle->addWidget(word, 0, Qt::AlignVCenter);
            toggle_switch = new Switch(QString(), this);
            toggle_switch->setObjectName(QStringLiteral("pluginRowSwitch"));
            toggle->addWidget(toggle_switch, 0, Qt::AlignVCenter);
            state->addLayout(toggle);
        }
        layout->addLayout(state, 0);
        // The list item carries the accessible name of the row; the parts of
        // it are not read again.
        setFocusPolicy(Qt::NoFocus);
    }

    /// Called when the row's size wants to change, such as when the text gets
    /// larger, so the list can give it the room.
    std::function<void()> on_size_change;

    void update_from(const plugin_view& plugin, bool can_run)
    {
        name->set_full_text(plugin.name);
        second->set_full_text(row_second_line(plugin));
        // The elided labels read as part of the row, not on their own.
        name->setToolTip(plain_text(plugin.name));
        chip->set_kind(chip_kind(plugin.state));
        chip->set_text(state_text(plugin.state));
        if (toggle_switch == nullptr)
            return;
        const bool usable = switch_usable(plugin);
        toggle_switch->setEnabled(usable);
        toggle_switch->blockSignals(true);
        toggle_switch->setChecked(switch_shown_on(plugin));
        toggle_switch->blockSignals(false);
        word->setText(
            toggle_switch->isChecked()
                ? QCoreApplication::translate("PluginsDialog", "On")
                : QCoreApplication::translate("PluginsDialog", "Off"));
        toggle_switch->setAccessibleName(plain_text(plugin.name));
        QString reason;
        if (!usable)
            reason = note_for(plugin).text;
        else if (!can_run && !switch_shown_on(plugin))
            reason = not_available_in_build();
        toggle_switch->setToolTip(reason);
        toggle_switch->setAccessibleDescription(reason);
    }

public:
    ElidedLabel* name = nullptr;
    ElidedLabel* second = nullptr;
    StatusChip* chip = nullptr;
    QLabel* word = nullptr;
    Switch* toggle_switch = nullptr;

protected:
    bool event(QEvent* event) override
    {
        const bool result = QWidget::event(event);
        if ((event->type() == QEvent::FontChange ||
             event->type() == QEvent::LayoutRequest) &&
            on_size_change)
            on_size_change();
        return result;
    }
};

// -- construction -------------------------------------------------------------

PluginsDialog::PluginsDialog(PluginBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend), snapshot_(backend.snapshot())
{
    build();
    refresh_all();
    connect(&backend_, &PluginBackend::snapshot_changed, this,
            &PluginsDialog::on_snapshot_changed);
    connect(&backend_, &PluginBackend::request_finished, this,
            &PluginsDialog::on_request_finished);
    connect(&backend_, &PluginBackend::key_file_read, this,
            &PluginsDialog::on_key_file_read);
    connect(&backend_, &PluginBackend::plugin_inspected, this,
            &PluginsDialog::on_plugin_inspected);
}

void PluginsDialog::build()
{
    VSS_PROFILE_ZONE("gui.support.plugins.build");
    setObjectName(QStringLiteral("pluginsDialog"));
    const QString title = dialog_title();
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    const int width =
        Theme::instance().metric(QStringLiteral("layout.dialogWidths.wide"));
    resize(width, 700);
    setMinimumWidth(width);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // -- heading and the header chip
    auto* header = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header);
    header_layout->setContentsMargins(24, 20, 24, 10);
    header_layout->setSpacing(4);
    auto* top_row = new QHBoxLayout();
    top_row->setSpacing(12);
    auto* heading = make_plain_label(title, header, text_style::title);
    heading->setObjectName(QStringLiteral("dialogHeading"));
    top_row->addWidget(heading, 0, Qt::AlignVCenter);
    header_chip_ = new StatusChip(header);
    header_chip_->setObjectName(QStringLiteral("pluginsHeaderChip"));
    top_row->addWidget(header_chip_, 0, Qt::AlignVCenter);
    top_row->addStretch(1);
    header_layout->addLayout(top_row);
    header_layout->addWidget(make_muted_label(dialog_sub(), header));

    top_note_ = make_plain_note(header);
    top_note_->setObjectName(QStringLiteral("pluginsTopNote"));
    top_note_->hide();
    connect(top_note_, &NoteBanner::action_triggered, this,
            [this]
            {
                if (snapshot_ &&
                    snapshot_->banner == plugin_banner::policy_untrusted)
                {
                    QGuiApplication::clipboard()->setText(
                        plain_text(snapshot_->policy_detail));
                }
                else
                    Q_EMIT licence_requested();
            });
    header_layout->addWidget(top_note_);

    message_note_ = make_plain_note(header);
    message_note_->setObjectName(QStringLiteral("pluginsMessageNote"));
    message_note_->hide();
    header_layout->addWidget(message_note_);
    outer->addWidget(header);

    // -- the body: the list on the left, the detail on the right
    auto* body = new QHBoxLayout();
    body->setContentsMargins(24, 0, 24, 8);
    body->setSpacing(16);

    // The left column scrolls on its own, so that at large text its buttons
    // never lie over the list.
    auto* left_scroll = new QScrollArea(this);
    left_scroll->setObjectName(QStringLiteral("pluginsLeftScroll"));
    left_scroll->setFrameShape(QFrame::NoFrame);
    left_scroll->setWidgetResizable(true);
    left_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    left_scroll->setFocusPolicy(Qt::NoFocus);
    left_scroll->setMinimumWidth(kListWidth);
    left_scroll->setMaximumWidth(kListWidth * 2);
    auto* left = new QWidget(left_scroll);
    left_scroll->setWidget(left);
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(8);

    filter_ = new QLineEdit(left);
    filter_->setObjectName(QStringLiteral("pluginsFilter"));
    filter_->setPlaceholderText(
        QCoreApplication::translate("PluginsDialog", "Filter plugins"));
    filter_->setAccessibleName(
        QCoreApplication::translate("PluginsDialog", "Filter plugins"));
    filter_->setClearButtonEnabled(true);
    left_layout->addWidget(filter_);

    list_ = new QListWidget(left);
    list_->setObjectName(QStringLiteral("pluginsList"));
    list_->setAccessibleName(
        QCoreApplication::translate("PluginsDialog", "Installed plugins"));
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setFocusPolicy(Qt::StrongFocus);
    left_layout->addWidget(list_, 1);

    empty_ = new QWidget(left);
    empty_->setObjectName(QStringLiteral("pluginsEmpty"));
    auto* empty_layout = new QVBoxLayout(empty_);
    empty_layout->setContentsMargins(8, 16, 8, 16);
    empty_layout->setSpacing(8);
    empty_title_ =
        make_plain_label(QCoreApplication::translate(
                             "PluginsDialog", "No plugins are installed"),
                         empty_, text_style::heading);
    empty_title_->setObjectName(QStringLiteral("pluginsEmptyTitle"));
    empty_layout->addWidget(empty_title_);
    empty_text_ = make_muted_label(
        QCoreApplication::translate(
            "PluginsDialog",
            "Plugins are folders that you or your administrator put in "
            "the plugin folder. Studio does not download anything."),
        empty_);
    empty_text_->setObjectName(QStringLiteral("pluginsEmptyText"));
    empty_layout->addWidget(empty_text_);
    empty_button_ = make_button(
        QCoreApplication::translate("PluginsDialog", "Open the plugin folder"),
        nullptr, empty_, QStringLiteral("pluginsEmptyOpen"));
    connect(empty_button_, &QPushButton::clicked, this,
            [this] { open_plugin_folder(); });
    empty_layout->addWidget(empty_button_, 0, Qt::AlignLeft);
    empty_layout->addStretch(1);
    left_layout->addWidget(empty_, 1);
    empty_->hide();

    add_ = make_button(
        QCoreApplication::translate("PluginsDialog", "Add a plugin..."),
        "quiet", left, QStringLiteral("pluginsAdd"));
    connect(add_, &QPushButton::clicked, this, [this] { add_plugin(); });
    import_ = make_button(
        QCoreApplication::translate("PluginsDialog", "Import a key..."),
        "quiet", left, QStringLiteral("pluginsImportKey"));
    connect(import_, &QPushButton::clicked, this, [this] { import_key(); });
    folder_ = make_button(
        QCoreApplication::translate("PluginsDialog", "Open the plugin folder"),
        "quiet", left, QStringLiteral("pluginsOpenFolder"));
    connect(folder_, &QPushButton::clicked, this,
            [this] { open_plugin_folder(); });
    check_ =
        make_button(QCoreApplication::translate("PluginsDialog", "Check again"),
                    "quiet", left, QStringLiteral("pluginsCheckAgain"));
    connect(check_, &QPushButton::clicked, this, [this] { check_again(); });
    for (QPushButton* button : {add_, import_, folder_, check_})
        left_layout->addWidget(button, 0, Qt::AlignLeft);

    auto* machine_row = new QWidget(left);
    auto* machine_layout = new QHBoxLayout(machine_row);
    machine_layout->setContentsMargins(0, 0, 0, 0);
    machine_layout->setSpacing(4);
    machine_line_ =
        make_muted_label(QString(), machine_row, text_style::caption);
    machine_line_->setObjectName(QStringLiteral("pluginsMachineLine"));
    machine_line_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    machine_layout->addWidget(machine_line_, 1);
    machine_copy_ = new CopyButton(machine_row);
    machine_copy_->setObjectName(QStringLiteral("pluginsMachineCopy"));
    connect(machine_copy_, &CopyButton::copied, this,
            [this]
            {
                ToastManager::instance().show(
                    this, QCoreApplication::translate("PluginsDialog",
                                                      "Path copied."));
            });
    machine_layout->addWidget(machine_copy_, 0, Qt::AlignTop);
    left_layout->addWidget(machine_row);
    body->addWidget(left_scroll);

    detail_scroll_ = new QScrollArea(this);
    detail_scroll_->setObjectName(QStringLiteral("pluginsDetailScroll"));
    detail_scroll_->setFrameShape(QFrame::NoFrame);
    detail_scroll_->setWidgetResizable(true);
    detail_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    detail_scroll_->setFocusPolicy(Qt::StrongFocus);
    detail_scroll_->setAccessibleName(
        QCoreApplication::translate("PluginsDialog", "Plugin details"));
    detail_ = new QWidget(detail_scroll_);
    detail_scroll_->setWidget(detail_);
    body->addWidget(detail_scroll_, 1);
    outer->addLayout(body, 1);

    // -- footer
    QHBoxLayout* footer_layout = nullptr;
    QFrame* footer = make_dialog_footer(this, footer_layout);
    remove_ = make_button(
        QCoreApplication::translate("PluginsDialog", "Remove plugin..."),
        "quiet", footer, QStringLiteral("pluginsRemove"));
    connect(remove_, &QPushButton::clicked, this,
            [this] { remove_selected(); });
    footer_layout->insertWidget(0, remove_);
    copy_ = make_button(
        QCoreApplication::translate("PluginsDialog", "Copy details"), "quiet",
        footer, QStringLiteral("pluginsCopyDetails"));
    connect(copy_, &QPushButton::clicked, this, [this] { copy_details(); });
    footer_layout->addWidget(copy_);
    close_ = make_button(QCoreApplication::translate("PluginsDialog", "Close"),
                         "primary", footer, QStringLiteral("pluginsClose"));
    close_->setDefault(true);
    connect(close_, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close_);
    outer->addWidget(footer);

    connect(filter_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
    connect(list_, &QListWidget::currentItemChanged, this,
            [this] { on_current_row_changed(); });

    // Tab order: the list, then the detail, then the footer.
    setTabOrder(filter_, list_);
    setTabOrder(list_, add_);
    setTabOrder(add_, import_);
    setTabOrder(import_, folder_);
    setTabOrder(folder_, check_);
    setTabOrder(check_, detail_scroll_);
    setTabOrder(detail_scroll_, remove_);
    setTabOrder(remove_, copy_);
    setTabOrder(copy_, close_);

    // Initial focus is the list.
    list_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, list_,
                       [this] { list_->setFocus(Qt::OtherFocusReason); });
}

// -- the data -----------------------------------------------------------------

QString PluginsDialog::key_of(const plugin_view& plugin) const
{
    return plugin.id + QLatin1Char('|') + plugin.folder;
}

const plugin_view* PluginsDialog::selected() const
{
    if (!snapshot_)
        return nullptr;
    for (const plugin_view& each : snapshot_->plugins)
    {
        if (key_of(each) == selected_key_)
            return &each;
    }
    return nullptr;
}

void PluginsDialog::on_snapshot_changed()
{
    refresh_all();
}

void PluginsDialog::refresh_all()
{
    snapshot_ = backend_.snapshot();
    if (!snapshot_)
        snapshot_ = std::make_shared<const plugin_snapshot>();
    const plugin_snapshot& snapshot = *snapshot_;
    refresh_header(snapshot);
    refresh_list(snapshot);
    refresh_detail();
}

void PluginsDialog::refresh_header(const plugin_snapshot& snapshot)
{
    int on = 0;
    int attention = 0;
    for (const plugin_view& each : snapshot.plugins)
    {
        if (switch_shown_on(each))
            ++on;
        if (needs_attention(each.state))
            ++attention;
    }
    header_chip_->set_text(header_chip_text(on, attention));
    header_chip_->set_kind(attention > 0 ? StatusChip::kind::waiting
                                         : StatusChip::kind::healthy);

    const banner_text banner = top_note_text(snapshot.banner);
    top_note_->setVisible(banner.shown);
    if (banner.shown)
    {
        top_note_->set_kind(banner.kind);
        if (top_note_->text() != banner.text)
            top_note_->set_text(banner.text);
        if (banner.action.isEmpty())
            top_note_->clear_action();
        else if (top_note_->action_button() == nullptr ||
                 top_note_->action_button()->text() != banner.action)
            top_note_->set_action(banner.action);
        if (top_note_->action_button() != nullptr)
            top_note_->action_button()->setVisible(!banner.action.isEmpty());
    }

    const bool user_folder_usable = snapshot.user_plugins_allowed;
    const QString reason = QCoreApplication::translate(
        "PluginsDialog",
        "Your administrator installs the plugins on this computer.");
    add_->setEnabled(user_folder_usable);
    folder_->setEnabled(user_folder_usable);
    empty_button_->setEnabled(user_folder_usable);
    for (QPushButton* button : {add_, folder_, empty_button_})
    {
        button->setToolTip(user_folder_usable ? QString() : reason);
        button->setAccessibleDescription(user_folder_usable ? QString()
                                                            : reason);
    }

    const QString machine = plain_text(snapshot.machine_folder);
    machine_line_->setText(
        QCoreApplication::translate(
            "PluginsDialog",
            "To install a plugin for everyone on this computer, an "
            "administrator puts its folder in %1.")
            .arg(wrappable_text(machine)));
    machine_copy_->set_copy_text(
        machine, QCoreApplication::translate("PluginsDialog", "folder path"));
    machine_copy_->setVisible(!machine.isEmpty());
}

namespace
{

struct ordered_row
{
    const plugin_view* plugin = nullptr;
    bool copy = false;
};

std::vector<ordered_row> order_rows(const plugin_snapshot& snapshot)
{
    std::vector<const plugin_view*> main;
    std::multimap<QString, const plugin_view*> copies;
    for (const plugin_view& each : snapshot.plugins)
    {
        bool has_used_copy = false;
        if (each.state == plugin_state::not_used)
        {
            for (const plugin_view& other : snapshot.plugins)
            {
                if (&other != &each && other.id == each.id &&
                    other.state != plugin_state::not_used)
                    has_used_copy = true;
            }
        }
        if (has_used_copy)
            copies.emplace(each.id, &each);
        else
            main.push_back(&each);
    }
    std::stable_sort(main.begin(), main.end(),
                     [](const plugin_view* left, const plugin_view* right)
                     {
                         const bool left_first = needs_attention(left->state);
                         const bool right_first = needs_attention(right->state);
                         if (left_first != right_first)
                             return left_first;
                         const int by_name = QString::compare(
                             plain_text(left->name), plain_text(right->name),
                             Qt::CaseInsensitive);
                         if (by_name != 0)
                             return by_name < 0;
                         return left->id < right->id;
                     });
    std::vector<ordered_row> rows;
    for (const plugin_view* each : main)
    {
        rows.push_back({each, false});
        const auto range = copies.equal_range(each->id);
        for (auto copy = range.first; copy != range.second; ++copy)
            rows.push_back({copy->second, true});
    }
    return rows;
}

} // namespace

void PluginsDialog::refresh_list(const plugin_snapshot& snapshot)
{
    const std::vector<ordered_row> rows = order_rows(snapshot);

    // The same rows in the same order: change them in place, so a switch that
    // has the focus keeps it.
    bool same = static_cast<int>(rows.size()) == list_->count();
    for (std::size_t index = 0; same && index < rows.size(); ++index)
    {
        const QListWidgetItem* item = list_->item(static_cast<int>(index));
        same = item->data(Qt::UserRole).toString() ==
                   key_of(*rows[index].plugin) &&
               item->data(Qt::UserRole + 1).toBool() == rows[index].copy;
    }

    const QSignalBlocker blocker(list_);
    if (!same)
    {
        const bool had_focus = list_->hasFocus();
        rebuilding_ = true;
        list_->clear();
        for (const ordered_row& row : rows)
        {
            auto* item = new QListWidgetItem(list_);
            item->setData(Qt::UserRole, key_of(*row.plugin));
            item->setData(Qt::UserRole + 1, row.copy);
            auto* widget = new Row(row.copy, list_);
            item->setSizeHint(widget->sizeHint());
            list_->setItemWidget(item, widget);
            widget->on_size_change = [this] { refresh_row_sizes(); };
            if (widget->toggle_switch != nullptr)
            {
                const QString id = row.plugin->id;
                const QString folder = row.plugin->folder;
                connect(widget->toggle_switch, &QCheckBox::clicked, this,
                        [this, id, folder](bool on)
                        { on_switch_clicked(id, folder, on); });
            }
        }
        rebuilding_ = false;
        if (had_focus)
            list_->setFocus(Qt::OtherFocusReason);
    }
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        QListWidgetItem* item = list_->item(static_cast<int>(index));
        auto* widget = static_cast<Row*>(list_->itemWidget(item));
        widget->update_from(*rows[index].plugin, snapshot.can_run_plugins);
        const QString name = row_accessible_name(*rows[index].plugin);
        item->setData(Qt::AccessibleTextRole, name);
        item->setData(Qt::AccessibleDescriptionRole,
                      rows[index].plugin->state == plugin_state::not_used
                          ? note_for(*rows[index].plugin).text
                          : QString());
    }
    refresh_row_sizes();

    // Selection: keep the row, else the one that was just added, else the
    // first.
    int target = -1;
    for (int index = 0; index < list_->count(); ++index)
    {
        if (list_->item(index)->data(Qt::UserRole).toString() == selected_key_)
            target = index;
    }
    if (waiting_for_add_)
    {
        for (int index = 0; index < list_->count(); ++index)
        {
            const QString key =
                list_->item(index)->data(Qt::UserRole).toString();
            if (std::find(before_add_.begin(), before_add_.end(), key) ==
                before_add_.end())
            {
                target = index;
                waiting_for_add_ = false;
                break;
            }
        }
    }
    if (target < 0 && list_->count() > 0)
        target = 0;
    if (target >= 0)
    {
        list_->setCurrentRow(target);
        selected_key_ = list_->item(target)->data(Qt::UserRole).toString();
    }
    else
    {
        selected_key_.clear();
    }
    apply_filter();
}

void PluginsDialog::refresh_row_sizes()
{
    if (rebuilding_)
        return;
    int rows_height = 0;
    int shown = 0;
    for (int index = 0; index < list_->count(); ++index)
    {
        QListWidgetItem* item = list_->item(index);
        QWidget* widget = list_->itemWidget(item);
        if (widget == nullptr)
            continue;
        const QSize hint(0, widget->sizeHint().height());
        if (item->sizeHint() != hint)
            item->setSizeHint(hint);
        if (shown < 4)
        {
            rows_height += hint.height();
            ++shown;
        }
    }
    // Room for a few rows however large the text is.
    list_->setMinimumHeight(rows_height + 2 * list_->frameWidth() + 4);
}

void PluginsDialog::apply_filter()
{
    const QString needle = plain_text(filter_->text()).trimmed();
    int visible = 0;
    for (int index = 0; index < list_->count(); ++index)
    {
        QListWidgetItem* item = list_->item(index);
        const QString key = item->data(Qt::UserRole).toString();
        bool matches = needle.isEmpty();
        if (!matches && snapshot_)
        {
            for (const plugin_view& each : snapshot_->plugins)
            {
                if (key_of(each) == key &&
                    plain_text(each.name).contains(needle, Qt::CaseInsensitive))
                    matches = true;
            }
        }
        item->setHidden(!matches);
        if (matches)
            ++visible;
    }
    const bool looking = snapshot_ && snapshot_->looking;
    const bool none_installed = list_->count() == 0 && !looking;
    const bool no_match = !none_installed && visible == 0;
    const bool waiting = list_->count() == 0 && looking;
    list_->setVisible(!none_installed && !no_match && !waiting);
    empty_->setVisible(none_installed || no_match || waiting);
    empty_title_->setVisible(none_installed);
    empty_button_->setVisible(none_installed);
    if (waiting)
    {
        empty_text_->setText(QCoreApplication::translate(
            "PluginsDialog", "Looking for plugins..."));
    }
    else if (no_match)
    {
        empty_text_->setText(QCoreApplication::translate(
                                 "PluginsDialog", "No plugin matches %1.")
                                 .arg(plain_text(filter_->text())));
    }
    else
    {
        empty_text_->setText(QCoreApplication::translate(
            "PluginsDialog",
            "Plugins are folders that you or your administrator put in the "
            "plugin folder. Studio does not download anything."));
    }
    // A selection that is hidden by the filter stays selected; the detail
    // belongs to it.
}

void PluginsDialog::on_current_row_changed()
{
    QListWidgetItem* item = list_->currentItem();
    selected_key_ =
        item != nullptr ? item->data(Qt::UserRole).toString() : QString();
    refresh_detail();
}

Switch* PluginsDialog::row_switch(const QString& plugin_id,
                                  const QString& folder) const
{
    for (int index = 0; index < list_->count(); ++index)
    {
        const QString key = list_->item(index)->data(Qt::UserRole).toString();
        const bool by_id = folder.isEmpty()
                               ? key.startsWith(plugin_id + QLatin1Char('|'))
                               : key == plugin_id + QLatin1Char('|') + folder;
        if (by_id)
        {
            auto* row =
                static_cast<Row*>(list_->itemWidget(list_->item(index)));
            return row != nullptr ? row->toggle_switch : nullptr;
        }
    }
    return nullptr;
}

QStringList PluginsDialog::row_keys() const
{
    QStringList keys;
    for (int index = 0; index < list_->count(); ++index)
        keys << list_->item(index)->data(Qt::UserRole).toString();
    return keys;
}

bool PluginsDialog::select(const QString& plugin_id, const QString& folder)
{
    for (int index = 0; index < list_->count(); ++index)
    {
        const QString key = list_->item(index)->data(Qt::UserRole).toString();
        const bool match = folder.isEmpty()
                               ? key.startsWith(plugin_id + QLatin1Char('|'))
                               : key == plugin_id + QLatin1Char('|') + folder;
        if (match)
        {
            list_->setCurrentRow(index);
            return true;
        }
    }
    return false;
}

QString PluginsDialog::details_text() const
{
    const plugin_view* plugin = selected();
    return plugin != nullptr ? copy_details_text(*plugin) : QString();
}

// -- the detail ---------------------------------------------------------------

QString PluginsDialog::detail_signature(const plugin_view& plugin,
                                        const plugin_snapshot& snapshot)
{
    QStringList parts;
    parts << plugin.id << plugin.name << plugin.version << plugin.author
          << plugin.description << plugin.folder << plugin.key_id
          << plugin.key_name << plugin.key_approved_on.toString(Qt::ISODate)
          << plugin.hash << plugin.reason_code << plugin.detail
          << plugin.limit_name << plugin.capability_name
          << plugin.studio_version << plugin.expires.toString(Qt::ISODate)
          << plugin.used_copy_folder << plugin.used_copy_author
          << plugin.used_copy_id << plugin.isolation_missing
          << plugin.override_on.toString(Qt::ISODate) << plugin.runtime_name
          << plugin.status_text;
    parts << QString::number(static_cast<int>(plugin.state))
          << QString::number(static_cast<int>(plugin.note))
          << QString::number(static_cast<int>(plugin.trust))
          << QString::number(static_cast<int>(plugin.installed_for))
          << QString::number(static_cast<int>(plugin.isolation))
          << QString::number(static_cast<int>(plugin.timing))
          << QString::number(static_cast<int>(plugin.licence_reason))
          << QString::number(plugin.has_licence_report)
          << QString::number(static_cast<int>(plugin.licence_report_reason))
          << QString::number(static_cast<int>(plugin.used_copy_trust))
          << QString::number(plugin.back_off_seconds)
          << QString::number(plugin.attempt)
          << QString::number(plugin.failure_count)
          << QString::number(plugin.budget_us)
          << QString::number(plugin.wished_on)
          << QString::number(plugin.override_active)
          << QString::number(plugin.override_allowed)
          << QString::number(plugin.newer_than_used_copy)
          << QString::number(snapshot.can_run_plugins)
          << QString::number(snapshot.user_plugins_allowed);
    for (const plugin_addition& each : plugin.adds)
        parts << QString::number(static_cast<int>(each.kind)) << each.type_name
              << each.type_id << QString::number(each.max_instances)
              << QString::number(each.needs_settings);
    for (const plugin_instance_view& each : plugin.instances)
    {
        parts << QString::number(each.number) << each.point_id
              << QString::number(static_cast<int>(each.state)) << each.refusal
              << each.reason << QString::number(each.settings_pending);
    }
    for (const plugin_permission_view& each : plugin.permissions)
    {
        parts << each.id << QString::number(each.required) << each.reason
              << each.scope.join(QLatin1Char('\x1f'))
              << QString::number(each.turned_down)
              << QString::number(each.is_new);
    }
    for (const plugin_use& each : plugin.used_in)
        parts << each.text << each.target_id;
    for (const plugin_log_line& each : plugin.log)
        parts << each.time.toString(Qt::ISODate) << each.text;
    return parts.join(QLatin1Char('\x1e'));
}

void PluginsDialog::add_instance_of(const plugin_view& plugin,
                                     const plugin_addition& point)
{
    // A point with nothing to ask for is made at once. One with settings to
    // ask for asks first, and nothing is made if the person cancels.
    if (point.asks.empty())
    {
        backend_.add_instance(plugin.id, point.type_id, {});
        return;
    }
    auto* ask = new PluginInstanceDialog(plugin, point, backend_.place_names(),
                                         this);
    connect(ask, &QDialog::finished, ask, &QObject::deleteLater);
    const QString plugin_id = plugin.id;
    const QString point_id = point.type_id;
    connect(ask, &QDialog::accepted, this,
            [this, ask, plugin_id, point_id]
            { backend_.add_instance(plugin_id, point_id, ask->values()); });
    ask->open();
}

void PluginsDialog::refresh_detail()
{
    const plugin_view* plugin = selected();
    const plugin_snapshot& snapshot = *snapshot_;

    // The state of the selected plugin, announced once when it changes.
    if (plugin != nullptr)
    {
        const QString state_key =
            key_of(*plugin) + QLatin1Char('|') +
            QString::number(static_cast<int>(plugin->state));
        if (!announced_state_key_.isEmpty() &&
            announced_state_key_.section(QLatin1Char('|'), 0, 1) ==
                key_of(*plugin) &&
            announced_state_key_ != state_key && isVisible())
        {
            announce_polite(
                this,
                QCoreApplication::translate("PluginsDialog", "%1, %2")
                    .arg(plain_text(plugin->name), state_text(plugin->state)));
        }
        announced_state_key_ = state_key;
    }
    else
    {
        announced_state_key_.clear();
    }

    // Footer.
    remove_->setVisible(true);
    QString remove_reason;
    bool can_remove = plugin != nullptr &&
                      plugin->state != plugin_state::not_installed &&
                      plugin->installed_for == plugin_installed_for::you_only;
    if (plugin != nullptr &&
        plugin->installed_for == plugin_installed_for::everyone)
        remove_reason = QCoreApplication::translate(
            "PluginsDialog", "Installed by your administrator");
    else if (plugin != nullptr &&
             plugin->installed_for == plugin_installed_for::with_studio)
        remove_reason =
            QCoreApplication::translate("PluginsDialog", "Came with Studio");
    remove_->setEnabled(can_remove);
    remove_->setToolTip(remove_reason);
    remove_->setAccessibleDescription(remove_reason);
    copy_->setEnabled(plugin != nullptr);

    const QString signature =
        plugin != nullptr ? detail_signature(*plugin, snapshot) : QString();
    if (signature == shown_detail_signature_ && detail_ != nullptr &&
        detail_->layout() != nullptr)
        return;
    shown_detail_signature_ = signature;

    const bool had_focus = detail_ != nullptr &&
                           detail_->isAncestorOf(QApplication::focusWidget());
    const int scroll_position = detail_scroll_->verticalScrollBar()->value();

    auto* root = new QWidget();
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 8, 8);
    layout->setSpacing(10);
    detail_scroll_->setWidget(root);
    detail_ = root;

    if (plugin == nullptr)
    {
        auto* none = make_muted_label(
            QCoreApplication::translate("PluginsDialog",
                                        "Select a plugin to see its details."),
            root);
        none->setObjectName(QStringLiteral("pluginsNoSelection"));
        layout->addWidget(none);
        layout->addStretch(1);
        return;
    }
    const plugin_view& view = *plugin;
    const plugin_view copy_of_view = view;

    // -- heading: name, the Plugin mark, the state chip
    auto* heading_row = new QHBoxLayout();
    heading_row->setSpacing(8);
    auto* name = new ElidedLabel(root);
    name->setFont(Theme::instance().font(text_style::title));
    name->setObjectName(QStringLiteral("pluginDetailName"));
    name->set_full_text(view.name);
    heading_row->addWidget(name, 1, Qt::AlignVCenter);
    auto* mark = new Tag(plugin_mark_text(), Tag::variant::n, root);
    mark->setObjectName(QStringLiteral("pluginDetailMark"));
    heading_row->addWidget(mark, 0, Qt::AlignVCenter);
    auto* chip =
        new StatusChip(state_text(view.state), chip_kind(view.state), root);
    chip->setObjectName(QStringLiteral("pluginDetailChip"));
    heading_row->addWidget(chip, 0, Qt::AlignVCenter);
    layout->addLayout(heading_row);

    // -- why and what to do
    const plugin_note_text note = note_for(view);
    if (note.shown)
    {
        auto* banner = make_plain_note(root);
        banner->setObjectName(QStringLiteral("pluginDetailNote"));
        banner->set_kind(note.kind);
        banner->set_text(note.text);
        layout->addWidget(banner);
        if (!note.actions.empty())
        {
            // The actions wrap to another line at large text.
            auto* actions_box = new QWidget(root);
            auto* actions = new FlowLayout(nullptr);
            actions_box->setLayout(actions);
            for (const plugin_action action : note.actions)
            {
                QPushButton* button =
                    make_button(action_text(action), "quiet", root,
                                QStringLiteral("pluginAction.%1")
                                    .arg(static_cast<int>(action)));
                connect(button, &QPushButton::clicked, this,
                        [this, action, copy_of_view]
                        { do_action(action, copy_of_view); });
                button->setParent(actions_box);
                actions->addWidget(button);
            }
            layout->addWidget(actions_box);
        }
    }
    else if (view.state == plugin_state::off && !snapshot.can_run_plugins)
    {
        // A build that cannot run plugins: the wish is kept and the plugin
        // stays off, and it says why.
        auto* banner = make_plain_note(root);
        banner->setObjectName(QStringLiteral("pluginDetailNote"));
        banner->set_kind(NoteBanner::kind::info);
        banner->set_text(not_available_in_build());
        layout->addWidget(banner);
    }

    // -- the definition list
    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(8);
    const auto row =
        [&](const QString& label, QWidget* value, const QString& object_name)
    {
        value->setObjectName(object_name);
        form->addRow(make_muted_label(label, root), value);
    };

    if (!view.adds.empty())
    {
        QStringList lines;
        for (const plugin_addition& each : view.adds)
            lines << addition_text(each);
        row(QCoreApplication::translate("PluginsDialog", "What it adds"),
            make_plain_label(lines.join(QLatin1Char('\n')), root,
                             text_style::body, true),
            QStringLiteral("pluginDetailAdds"));
    }
    if (!plain_text(view.description).isEmpty())
    {
        row(QCoreApplication::translate("PluginsDialog", "Description"),
            make_plain_label(view.description, root, text_style::body, true),
            QStringLiteral("pluginDetailDescription"));
    }
    if (!plain_text(view.version).isEmpty())
    {
        row(QCoreApplication::translate("PluginsDialog", "Version"),
            make_fixed_label(plain_text(view.version), root),
            QStringLiteral("pluginDetailVersion"));
    }
    row(QCoreApplication::translate("PluginsDialog", "Made by"),
        make_plain_label(view.trust == plugin_trust::official
                             ? display_author(view)
                             : view.author,
                         root, text_style::body, true),
        QStringLiteral("pluginDetailAuthor"));
    // What the running plugin last said of itself: its own words, as text.
    if (!plain_text(view.status_text).isEmpty())
    {
        row(QCoreApplication::translate("PluginsDialog", "Status"),
            make_plain_label(view.status_text, root, text_style::body, true),
            QStringLiteral("pluginDetailStatus"));
    }

    // Signature, with the badge for an official plugin only.
    if (view.state != plugin_state::not_installed)
    {
        auto* signature_box = new QWidget(root);
        auto* signature_layout = new QVBoxLayout(signature_box);
        signature_layout->setContentsMargins(0, 0, 0, 0);
        signature_layout->setSpacing(4);
        auto* sentence_row = new QHBoxLayout();
        sentence_row->setSpacing(8);
        QLabel* sentence =
            make_plain_label(signature_sentence(view), signature_box);
        sentence->setObjectName(QStringLiteral("pluginDetailSignatureText"));
        sentence_row->addWidget(sentence, 1);
        if (view.trust == plugin_trust::official)
        {
            // Only the host's own trust level draws this; nothing a plugin
            // says about itself can.
            auto* badge =
                new Tag(official_badge_text(), Tag::variant::ok, signature_box);
            badge->setObjectName(QStringLiteral("pluginOfficialBadge"));
            badge->setAccessibleName(official_badge_text());
            sentence_row->addWidget(badge, 0, Qt::AlignTop);
        }
        signature_layout->addLayout(sentence_row);
        if (view.trust == plugin_trust::unknown_key)
        {
            QPushButton* import_button =
                make_button(QCoreApplication::translate(
                                "PluginsDialog", "Import the author's key..."),
                            "quiet", signature_box,
                            QStringLiteral("pluginDetailImportKey"));
            connect(import_button, &QPushButton::clicked, this,
                    [this] { import_key(); });
            signature_layout->addWidget(import_button, 0, Qt::AlignLeft);
        }
        signature_layout->addWidget(make_muted_label(
            QCoreApplication::translate(
                "PluginsDialog",
                "A signature shows who made a plugin and that its files "
                "are unchanged. It does not show that the plugin is "
                "safe."),
            signature_box, text_style::caption));
        const QString runtime = runtime_line(view);
        if (!runtime.isEmpty())
        {
            auto* warn = make_plain_note(signature_box);
            warn->setObjectName(QStringLiteral("pluginDetailRuntime"));
            warn->set_kind(NoteBanner::kind::warn);
            warn->set_text(runtime);
            signature_layout->addWidget(warn);
        }
        row(QCoreApplication::translate("PluginsDialog", "Signature"),
            signature_box, QStringLiteral("pluginDetailSignature"));
    }

    if (view.trust == plugin_trust::third_party && !view.key_id.isEmpty())
    {
        auto* key_box = new QWidget(root);
        auto* key_layout = new QVBoxLayout(key_box);
        key_layout->setContentsMargins(0, 0, 0, 0);
        key_layout->setSpacing(4);
        const QString key_text =
            view.key_approved_on.isValid()
                ? QCoreApplication::translate("PluginsDialog",
                                              "%1, approved on %2")
                      .arg(plain_text(view.key_id),
                           QLocale().toString(view.key_approved_on,
                                              QLocale::LongFormat))
                : plain_text(view.key_id);
        auto* key_label = make_plain_label(key_text, key_box);
        key_label->setObjectName(QStringLiteral("pluginDetailKeyText"));
        key_layout->addWidget(key_label);
        QPushButton* remove_key_button = make_button(
            QCoreApplication::translate("PluginsDialog", "Remove this key..."),
            "quiet", key_box, QStringLiteral("pluginDetailRemoveKey"));
        connect(remove_key_button, &QPushButton::clicked, this,
                [this, copy_of_view] { remove_key(copy_of_view); });
        key_layout->addWidget(remove_key_button, 0, Qt::AlignLeft);
        row(QCoreApplication::translate("PluginsDialog", "Signing key"),
            key_box, QStringLiteral("pluginDetailKey"));
    }

    if (!view.fingerprint.isEmpty())
    {
        auto* print_box = new QWidget(root);
        auto* print_layout = new QHBoxLayout(print_box);
        print_layout->setContentsMargins(0, 0, 0, 0);
        print_layout->setSpacing(6);
        auto* print = make_fixed_label(plain_text(view.fingerprint), print_box);
        print->setObjectName(QStringLiteral("pluginDetailFingerprintText"));
        print_layout->addWidget(print, 0, Qt::AlignVCenter);
        auto* copy_button = new CopyButton(print_box);
        copy_button->setObjectName(
            QStringLiteral("pluginDetailFingerprintCopy"));
        copy_button->set_copy_text(
            plain_text(view.fingerprint),
            QCoreApplication::translate("PluginsDialog", "fingerprint"));
        connect(copy_button, &CopyButton::copied, this,
                [this]
                {
                    ToastManager::instance().show(
                        this, QCoreApplication::translate(
                                  "PluginsDialog", "Fingerprint copied."));
                });
        print_layout->addWidget(copy_button, 0, Qt::AlignVCenter);
        print_layout->addStretch(1);
        row(QCoreApplication::translate("PluginsDialog", "Fingerprint"),
            print_box, QStringLiteral("pluginDetailFingerprint"));
    }

    if (view.expires.isValid())
    {
        row(QCoreApplication::translate("PluginsDialog", "Expires"),
            make_plain_label(
                QLocale().toString(view.expires, QLocale::LongFormat), root),
            QStringLiteral("pluginDetailExpires"));
    }

    if (view.state == plugin_state::needs_licence)
    {
        row(QCoreApplication::translate("PluginsDialog", "Licence"),
            make_plain_label(
                reason_sentence(view.licence_reason, view.licence_status),
                root),
            QStringLiteral("pluginDetailLicence"));
    }
    else if (view.trust == plugin_trust::official &&
             (view.state == plugin_state::available ||
              view.state == plugin_state::starting ||
              view.state == plugin_state::off ||
              view.state == plugin_state::stopped))
    {
        row(QCoreApplication::translate("PluginsDialog", "Licence"),
            make_plain_label(QCoreApplication::translate(
                                 "PluginsDialog", "Included in your licence"),
                             root),
            QStringLiteral("pluginDetailLicence"));
    }

    if (view.state != plugin_state::not_installed)
    {
        auto* timing = make_plain_label(timing_text(view.timing), root);
        timing->setToolTip(timing_sentence(view));
        timing->setAccessibleDescription(timing_sentence(view));
        row(QCoreApplication::translate("PluginsDialog", "Timing"), timing,
            QStringLiteral("pluginDetailTiming"));

        auto* isolation_box = new QWidget(root);
        auto* isolation_layout = new QHBoxLayout(isolation_box);
        isolation_layout->setContentsMargins(0, 0, 0, 0);
        isolation_layout->setSpacing(8);
        auto* isolation = make_plain_label(isolation_text(view), isolation_box);
        isolation->setObjectName(QStringLiteral("pluginDetailIsolationText"));
        isolation_layout->addWidget(isolation, 1);
        if (view.override_active)
        {
            QPushButton* undo = make_button(
                QCoreApplication::translate("PluginsDialog", "Undo"), "quiet",
                isolation_box, QStringLiteral("pluginDetailUndoIsolation"));
            connect(undo, &QPushButton::clicked, this,
                    [this, copy_of_view]
                    {
                        backend_.set_no_isolation(copy_of_view.id,
                                                  copy_of_view.hash, false);
                    });
            isolation_layout->addWidget(undo, 0, Qt::AlignTop);
        }
        row(QCoreApplication::translate("PluginsDialog", "Isolation"),
            isolation_box, QStringLiteral("pluginDetailIsolation"));

        row(QCoreApplication::translate("PluginsDialog", "Installed for"),
            make_plain_label(installed_for_text(view.installed_for), root),
            QStringLiteral("pluginDetailInstalledFor"));
    }

    if (!view.folder.isEmpty())
    {
        auto* folder_box = new QWidget(root);
        auto* folder_layout = new QVBoxLayout(folder_box);
        folder_layout->setContentsMargins(0, 0, 0, 0);
        folder_layout->setSpacing(4);
        auto* path_row = new QHBoxLayout();
        path_row->setSpacing(6);
        auto* path = make_fixed_label(plain_text(view.folder), folder_box);
        path->setWordWrap(true);
        path->setObjectName(QStringLiteral("pluginDetailFolderText"));
        path_row->addWidget(path, 1);
        auto* copy_button = new CopyButton(folder_box);
        copy_button->setObjectName(QStringLiteral("pluginDetailFolderCopy"));
        copy_button->set_copy_text(
            plain_text(view.folder),
            QCoreApplication::translate("PluginsDialog", "folder path"));
        connect(copy_button, &CopyButton::copied, this,
                [this]
                {
                    ToastManager::instance().show(
                        this, QCoreApplication::translate("PluginsDialog",
                                                          "Path copied."));
                });
        path_row->addWidget(copy_button, 0, Qt::AlignTop);
        folder_layout->addLayout(path_row);
        QPushButton* show = make_button(
            QCoreApplication::translate("PluginsDialog", "Show in folder"),
            "quiet", folder_box, QStringLiteral("pluginDetailShowFolder"));
        connect(show, &QPushButton::clicked, this,
                [this, copy_of_view]
                {
                    if (opener_)
                        opener_(open_path_text(copy_of_view));
                    else
                        QDesktopServices::openUrl(
                            QUrl::fromLocalFile(open_path_text(copy_of_view)));
                });
        folder_layout->addWidget(show, 0, Qt::AlignLeft);
        row(QCoreApplication::translate("PluginsDialog", "Folder"), folder_box,
            QStringLiteral("pluginDetailFolder"));
    }
    layout->addLayout(form);

    // -- what it may do
    if (!view.permissions.empty())
    {
        auto* permissions_heading = make_plain_label(
            QCoreApplication::translate("PluginsDialog", "What it may do"),
            root, text_style::heading);
        permissions_heading->setObjectName(QStringLiteral("pluginDetailMayDo"));
        layout->addWidget(permissions_heading);
        for (const plugin_permission_view& each : view.permissions)
        {
            const QString reason =
                each.reason.isEmpty()
                    ? QString()
                    : QCoreApplication::translate("PluginsDialog",
                                                  "The plugin says: %1")
                          .arg(quoted(each.reason));
            QFrame* line = make_permission_row(
                permission_sentence(each), is_amber_permission(each.id),
                !each.turned_down,
                each.turned_down ? QCoreApplication::translate("PluginsDialog",
                                                               "Not allowed")
                                 : QString(),
                reason, root);
            line->setObjectName(QStringLiteral("permission.") + each.id);
            layout->addWidget(line);
        }
        QPushButton* change = make_button(
            QCoreApplication::translate("PluginsDialog",
                                        "Change what it may do..."),
            "quiet", root, QStringLiteral("pluginDetailChangePermissions"));
        const bool by_administrator =
            view.installed_for == plugin_installed_for::everyone;
        const bool decidable = view.state == plugin_state::needs_decision ||
                               view.state == plugin_state::off ||
                               view.state == plugin_state::available ||
                               view.state == plugin_state::starting ||
                               view.state == plugin_state::stopped ||
                               view.state == plugin_state::needs_licence;
        change->setEnabled(!by_administrator && decidable);
        if (by_administrator)
        {
            change->setToolTip(QCoreApplication::translate(
                "PluginsDialog", "Set by your administrator"));
            change->setAccessibleDescription(QCoreApplication::translate(
                "PluginsDialog", "Set by your administrator"));
        }
        connect(change, &QPushButton::clicked, this,
                [this, copy_of_view] { review(copy_of_view, true); });
        layout->addWidget(change, 0, Qt::AlignLeft);
    }

    // -- what the plugin said of its licence check
    if (view.has_licence_report &&
        view.licence_report_reason != vss::license::reason::ok)
    {
        QString text =
            QCoreApplication::translate(
                "PluginsDialog", "This plugin did not accept the licence: %1")
                .arg(reason_sentence(view.licence_report_reason,
                                     view.licence_status));
        // Studio looked and found nothing wrong, so one of them is wrong.
        if (view.licence_reason == vss::license::reason::ok &&
            view.state != plugin_state::needs_licence)
        {
            text += QLatin1Char(' ') +
                    QCoreApplication::translate(
                        "PluginsDialog",
                        "Studio and the plugin disagree. Check that Studio "
                        "and the plugin are both up to date.");
        }
        auto* report = make_plain_note(root);
        report->setObjectName(QStringLiteral("pluginDetailLicenceReport"));
        report->set_kind(NoteBanner::kind::warn);
        report->set_text(text);
        layout->addWidget(report);
    }

    // -- what it runs
    if (!view.adds.empty())
    {
        auto* runs_heading = make_plain_label(
            QCoreApplication::translate("PluginsDialog", "What it runs"), root,
            text_style::heading);
        layout->addWidget(runs_heading);
        if (view.instances.empty())
        {
            auto* none =
                make_muted_label(QCoreApplication::translate(
                                     "PluginsDialog", "Nothing is set up yet."),
                                 root);
            none->setObjectName(QStringLiteral("pluginDetailNoInstances"));
            layout->addWidget(none);
        }
        for (const plugin_instance_view& instance : view.instances)
        {
            auto* row_box = new QVBoxLayout();
            row_box->setSpacing(2);
            auto* row_line = new QHBoxLayout();
            row_line->setSpacing(8);
            auto* point_label = make_plain_label(instance.point_name, root,
                                                 text_style::body, true);
            point_label->setObjectName(QStringLiteral("pluginInstanceName"));
            row_line->addWidget(point_label, 1);
            auto* state_label =
                make_muted_label(instance_state_text(instance.state), root);
            state_label->setObjectName(QStringLiteral("pluginInstanceState"));
            row_line->addWidget(state_label, 0, Qt::AlignTop);
            QPushButton* take_away = make_button(
                QCoreApplication::translate("PluginsDialog", "Remove"), "quiet",
                root, QStringLiteral("pluginInstanceRemove"));
            take_away->setAccessibleName(
                QCoreApplication::translate("PluginsDialog", "Remove %1")
                    .arg(plain_text(instance.point_name)));
            const QString plugin_id = view.id;
            const quint64 number = instance.number;
            connect(take_away, &QPushButton::clicked, this,
                    [this, plugin_id, number]
                    { backend_.remove_instance(plugin_id, number); });
            row_line->addWidget(take_away, 0, Qt::AlignTop);
            row_box->addLayout(row_line);
            if (instance.state == plugin_instance_state::refused)
            {
                row_box->addWidget(make_muted_label(instance.refusal, root,
                                                    text_style::caption));
                if (!instance.reason.trimmed().isEmpty())
                {
                    auto* says = make_plain_label(
                        QCoreApplication::translate("PluginsDialog",
                                                    "The plugin says: %1")
                            .arg(quoted(instance.reason)),
                        root, text_style::caption, true);
                    says->setObjectName(QStringLiteral("pluginInstanceSays"));
                    row_box->addWidget(says);
                }
            }
            if (instance.settings_pending)
            {
                row_box->addWidget(make_muted_label(
                    QCoreApplication::translate("PluginsDialog",
                                                "Sending the new settings"),
                    root, text_style::caption));
            }
            layout->addLayout(row_box);
        }
        // What can still be added: the points that have room, and can be
        // started without settings Studio cannot ask for yet.
        QPushButton* add_instance =
            make_button(QCoreApplication::translate("PluginsDialog", "Add..."),
                        "quiet", root, QStringLiteral("pluginInstanceAdd"));
        auto* menu = new QMenu(add_instance);
        int offered = 0;
        for (const plugin_addition& point : view.adds)
        {
            int have = 0;
            for (const plugin_instance_view& each : view.instances)
            {
                if (each.point_id == point.type_id)
                    ++have;
            }
            const bool room = have < point.max_instances;
            QAction* entry = menu->addAction(plain_text(point.type_name));
            entry->setEnabled(room && !point.needs_settings);
            if (point.needs_settings)
                entry->setToolTip(QCoreApplication::translate(
                    "PluginsDialog",
                    "This needs settings that Studio cannot ask for yet."));
            else if (!room)
                entry->setToolTip(QCoreApplication::translate(
                    "PluginsDialog", "The plugin allows no more of these."));
            if (room && !point.needs_settings)
                ++offered;
            const plugin_view plugin_copy = view;
            const plugin_addition point_copy = point;
            connect(entry, &QAction::triggered, this,
                    [this, plugin_copy, point_copy]
                    { add_instance_of(plugin_copy, point_copy); });
        }
        add_instance->setMenu(menu);
        const bool usable = view.state != plugin_state::cannot_be_used &&
                            view.state != plugin_state::needs_newer_studio &&
                            view.state != plugin_state::blocked &&
                            view.state != plugin_state::not_used;
        add_instance->setEnabled(usable && offered > 0);
        layout->addWidget(add_instance, 0, Qt::AlignLeft);
    }

    // -- where it is used
    auto* used_heading = make_plain_label(
        QCoreApplication::translate("PluginsDialog", "Used in this setup"),
        root, text_style::heading);
    layout->addWidget(used_heading);
    if (view.used_in.empty())
    {
        auto* none =
            make_muted_label(QCoreApplication::translate(
                                 "PluginsDialog", "Not used in this setup."),
                             root);
        none->setObjectName(QStringLiteral("pluginDetailNotUsed"));
        layout->addWidget(none);
    }
    for (const plugin_use& use : view.used_in)
    {
        auto* use_row = new QHBoxLayout();
        use_row->setSpacing(8);
        use_row->addWidget(
            make_plain_label(use.text, root, text_style::body, true), 1);
        QPushButton* show =
            make_button(QCoreApplication::translate("PluginsDialog", "Show"),
                        "quiet", root, QStringLiteral("pluginDetailShowUse"));
        show->setAccessibleDescription(plain_text(use.text));
        const QString target = use.target_id;
        connect(show, &QPushButton::clicked, this,
                [this, target] { Q_EMIT show_on_canvas(target); });
        use_row->addWidget(show, 0, Qt::AlignTop);
        layout->addLayout(use_row);
    }

    // -- the log
    if (!view.log.empty())
    {
        const int shown =
            std::min<int>(kMaxLogLines, static_cast<int>(view.log.size()));
        auto* log = new Disclosure(
            QCoreApplication::translate("PluginsDialog", "Recent messages (%1)")
                .arg(shown),
            root);
        log->setObjectName(QStringLiteral("pluginDetailLog"));
        auto* content = new QWidget();
        auto* content_layout = new QVBoxLayout(content);
        content_layout->setContentsMargins(0, 4, 0, 0);
        content_layout->setSpacing(6);
        auto* text = new QPlainTextEdit(content);
        text->setObjectName(QStringLiteral("pluginDetailLogText"));
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        text->setFont(Theme::instance().font(text_style::data));
        text->setAccessibleName(
            QCoreApplication::translate("PluginsDialog", "Messages from %1")
                .arg(plain_text(view.name)));
        text->setMinimumHeight(120);
        QStringList lines;
        const int first = static_cast<int>(view.log.size()) - shown;
        for (int index = first; index < static_cast<int>(view.log.size());
             ++index)
        {
            const plugin_log_line& line =
                view.log[static_cast<std::size_t>(index)];
            // The host's log lines carry no time; a line that has one shows it.
            if (line.time.isValid())
                lines << QStringLiteral("%1 %2").arg(
                    QLocale().toString(line.time, QLocale::ShortFormat),
                    plain_text(line.text));
            else
                lines << plain_text(line.text);
        }
        text->setPlainText(lines.join(QLatin1Char('\n')));
        content_layout->addWidget(text);
        QPushButton* copy_log = make_button(
            QCoreApplication::translate("PluginsDialog", "Copy"), "quiet",
            content, QStringLiteral("pluginDetailCopyLog"));
        connect(copy_log, &QPushButton::clicked, this,
                [this, text]
                {
                    QGuiApplication::clipboard()->setText(text->toPlainText());
                    ToastManager::instance().show(
                        this, QCoreApplication::translate("PluginsDialog",
                                                          "Copied."));
                });
        content_layout->addWidget(copy_log, 0, Qt::AlignLeft);
        content_layout->addWidget(make_muted_label(
            QCoreApplication::translate("PluginsDialog",
                                        "These lines come from the plugin."),
            content, text_style::caption));
        log->set_content(content);
        layout->addWidget(log);
    }
    layout->addStretch(1);

    detail_scroll_->verticalScrollBar()->setValue(scroll_position);
    if (had_focus)
        list_->setFocus(Qt::OtherFocusReason);
}

// -- actions ------------------------------------------------------------------

void PluginsDialog::do_action(plugin_action action, const plugin_view& plugin)
{
    switch (action)
    {
    case plugin_action::start_now:
    case plugin_action::start_again:
        backend_.start_again(plugin.id);
        Q_EMIT status_message(
            QCoreApplication::translate("PluginsDialog", "%1 started again.")
                .arg(plain_text(plugin.name)));
        break;
    case plugin_action::review_and_decide:
        review(plugin);
        break;
    case plugin_action::show_used_copy:
        select(plugin.used_copy_id.isEmpty() ? plugin.id : plugin.used_copy_id,
               plugin.used_copy_folder);
        break;
    case plugin_action::copy_details:
        copy_details();
        break;
    case plugin_action::run_without_isolation:
        run_without_isolation(plugin);
        break;
    case plugin_action::licence:
        Q_EMIT licence_requested();
        break;
    case plugin_action::show_log:
    {
        if (auto* log = detail_->findChild<Disclosure*>(
                QStringLiteral("pluginDetailLog")))
        {
            log->set_expanded(true);
            if (auto* text = log->findChild<QPlainTextEdit*>())
                text->setFocus(Qt::OtherFocusReason);
        }
        break;
    }
    }
}

void PluginsDialog::on_switch_clicked(const QString& plugin_id,
                                      const QString& folder, bool on)
{
    const plugin_view* plugin = nullptr;
    for (const plugin_view& each : snapshot_->plugins)
    {
        if (each.id == plugin_id && each.folder == folder)
            plugin = &each;
    }
    if (plugin == nullptr)
        return;
    const plugin_view view = *plugin;
    if (view.state == plugin_state::needs_decision)
    {
        // The decision comes first; the switch stays as it was until the
        // host says otherwise.
        if (Switch* box = row_switch(plugin_id, folder))
        {
            const QSignalBlocker blocker(box);
            box->setChecked(false);
        }
        if (on)
            review(view);
        return;
    }
    backend_.set_enabled(view.id, view.hash, on);
    if (snapshot_->can_run_plugins)
    {
        Q_EMIT status_message(
            on ? QCoreApplication::translate("PluginsDialog", "%1 is on.")
                     .arg(plain_text(view.name))
               : QCoreApplication::translate("PluginsDialog", "%1 is off.")
                     .arg(plain_text(view.name)));
    }
}

void PluginsDialog::review(const plugin_view& plugin, bool permissions_step)
{
    PluginDecisionDialog dialog(backend_, plugin,
                                permissions_step
                                    ? PluginDecisionDialog::mode::permissions
                                    : PluginDecisionDialog::mode::decide,
                                this);
    connect(&dialog, &PluginDecisionDialog::import_key_requested, this,
            [this] { QTimer::singleShot(0, this, [this] { import_key(); }); });
    dialog.exec();
}

void PluginsDialog::run_without_isolation(const plugin_view& plugin)
{
    PluginNoIsolationDialog dialog(backend_, plugin, this);
    dialog.exec();
}

void PluginsDialog::remove_selected()
{
    const plugin_view* plugin = selected();
    if (plugin == nullptr || !remove_->isEnabled())
        return;
    const plugin_view view = *plugin;
    PluginRemoveDialog dialog(backend_, view, this);
    dialog.exec();
}

void PluginsDialog::remove_key(const plugin_view& plugin)
{
    ConfirmDialog dialog(
        QCoreApplication::translate("PluginsDialog", "Remove the key of %1?")
            .arg(display_author(plugin)),
        QCoreApplication::translate(
            "PluginsDialog",
            "Plugins signed with this key stop running and ask again."),
        QCoreApplication::translate("PluginsDialog", "Remove key"),
        QCoreApplication::translate("PluginsDialog", "Cancel"), true, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    backend_.remove_key(plugin.key_id);
}

void PluginsDialog::copy_details()
{
    const QString text = details_text();
    if (text.isEmpty() && snapshot_ &&
        snapshot_->banner == plugin_banner::policy_untrusted)
        QGuiApplication::clipboard()->setText(
            plain_text(snapshot_->policy_detail));
    else
        QGuiApplication::clipboard()->setText(text);
    ToastManager::instance().show(
        this, QCoreApplication::translate("PluginsDialog", "Details copied."));
}

void PluginsDialog::add_plugin()
{
    if (!add_->isEnabled())
        return;
    QString folder;
    if (folder_picker_)
        folder = folder_picker_();
    else
        folder = QFileDialog::getExistingDirectory(
            this, QCoreApplication::translate("PluginsDialog",
                                              "Choose a plugin's folder"));
    if (folder.isEmpty())
        return;
    clear_message();
    before_add_.clear();
    for (int index = 0; index < list_->count(); ++index)
        before_add_.push_back(
            list_->item(index)->data(Qt::UserRole).toString());
    // The host looks first, and the person sees what it found before
    // anything is copied.
    pending_inspection_ = backend_.inspect_plugin(folder);
}

void PluginsDialog::on_plugin_inspected(quint64 request,
                                        const plugin_inspection_view& found)
{
    if (request != pending_inspection_)
        return;
    pending_inspection_ = 0;
    waiting_for_add_ = true;
    PluginInstallDialog dialog(backend_, found, this);
    if (dialog.exec() != QDialog::Accepted)
        waiting_for_add_ = false;
}

void PluginsDialog::import_key()
{
    QString path;
    if (key_picker_)
    {
        path = key_picker_();
    }
    else
    {
        path = QFileDialog::getOpenFileName(
            this,
            QCoreApplication::translate("PluginsDialog",
                                        "Choose an author's public key file"),
            QString(),
            QCoreApplication::translate("PluginsDialog",
                                        "Plugin key files (*.vsskey.json)"));
    }
    if (path.isEmpty())
        return;
    clear_message();
    // A file with a control character or a direction mark in its name is
    // refused before anything reads it.
    const QString name = QFileInfo(path).fileName();
    if (plain_text(name) != name)
    {
        show_message(
            NoteBanner::kind::warn,
            QCoreApplication::translate("PluginsDialog",
                                        "That file is not a plugin key file."));
        return;
    }
    pending_key_request_ = backend_.read_key_file(path);
}

void PluginsDialog::open_plugin_folder()
{
    if (!snapshot_ || !snapshot_->user_plugins_allowed)
        return;
    const QString folder = snapshot_->user_folder;
    if (folder.isEmpty())
        return;
    if (opener_)
        opener_(folder);
    else
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
}

void PluginsDialog::check_again()
{
    backend_.rescan();
}

void PluginsDialog::show_message(NoteBanner::kind kind, const QString& text)
{
    message_note_->set_kind(kind);
    message_note_->set_text(text);
    message_note_->show();
}

void PluginsDialog::clear_message()
{
    message_note_->hide();
}

void PluginsDialog::on_key_file_read(quint64 request,
                                     const plugin_key_file_view& key)
{
    if (request != pending_key_request_)
        return;
    pending_key_request_ = 0;
    using problem = plugin_key_file_view::problem;
    switch (key.failure)
    {
    case problem::none:
    {
        PluginKeyImportDialog dialog(backend_, key, this);
        dialog.exec();
        break;
    }
    case problem::not_a_key_file:
    case problem::text_not_plain:
        show_message(
            NoteBanner::kind::warn,
            QCoreApplication::translate("PluginsDialog",
                                        "That file is not a plugin key file."));
        break;
    case problem::id_in_use:
        show_message(
            NoteBanner::kind::warn,
            QCoreApplication::translate(
                "PluginsDialog",
                "A key with the id %1 is already approved. An id cannot "
                "change its key.")
                .arg(plain_text(key.key_id)));
        break;
    case problem::id_is_official_or_administrators:
        show_message(NoteBanner::kind::warn,
                     QCoreApplication::translate(
                         "PluginsDialog",
                         "The id %1 belongs to a key that is already on this "
                         "computer.")
                         .arg(plain_text(key.key_id)));
        break;
    }
}

void PluginsDialog::on_request_finished(const plugin_request_result& result)
{
    switch (result.kind)
    {
    case plugin_request_kind::rescan:
        ToastManager::instance().show(
            this, result.count == 1
                      ? QCoreApplication::translate("PluginsDialog",
                                                    "Checked. 1 plugin found.")
                      : QCoreApplication::translate(
                            "PluginsDialog", "Checked. %n plugins found.",
                            nullptr, result.count));
        break;
    case plugin_request_kind::add_plugin:
        if (result.error == plugin_request_error::none)
        {
            ToastManager::instance().show(
                this, QCoreApplication::translate(
                          "PluginsDialog",
                          "Added for you only. Review it before it runs."));
            break;
        }
        waiting_for_add_ = false;
        if (result.error == plugin_request_error::already_installed)
        {
            show_message(
                NoteBanner::kind::warn,
                QCoreApplication::translate(
                    "PluginsDialog",
                    "%1 is already installed for you. Remove it first to "
                    "add a different copy.")
                    .arg(plain_text(result.plugin_name)));
        }
        else if (result.error == plugin_request_error::not_a_plugin)
        {
            show_message(
                NoteBanner::kind::warn,
                QCoreApplication::translate(
                    "PluginsDialog",
                    "That folder is not a plugin. A plugin's folder holds "
                    "a file named plugin.json."));
        }
        break;
    case plugin_request_kind::approve_key:
        if (result.error == plugin_request_error::none)
        {
            ToastManager::instance().show(
                this, QCoreApplication::translate("PluginsDialog",
                                                  "The key of %1 is approved.")
                          .arg(plain_text(result.author)));
        }
        else
        {
            show_message(
                NoteBanner::kind::warn,
                QCoreApplication::translate(
                    "PluginsDialog", "That file is not a plugin key file."));
        }
        break;
    case plugin_request_kind::decide:
    case plugin_request_kind::set_enabled:
    case plugin_request_kind::remove_key:
    case plugin_request_kind::set_no_isolation:
    case plugin_request_kind::remove_plugin:
    case plugin_request_kind::start_again:
    case plugin_request_kind::add_instance:
    case plugin_request_kind::remove_instance:
        break;
    }
}

void PluginsDialog::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_F6 && event->modifiers() == Qt::NoModifier)
    {
        move_focus_between_panes();
        event->accept();
        return;
    }
    QDialog::keyPressEvent(event);
}

void PluginsDialog::move_focus_between_panes()
{
    QWidget* focus = QApplication::focusWidget();
    const bool in_detail =
        focus != nullptr &&
        (detail_scroll_ == focus || detail_scroll_->isAncestorOf(focus));
    if (in_detail)
    {
        list_->setFocus(Qt::TabFocusReason);
        return;
    }
    // The first control of the detail, else the detail itself, which scrolls.
    for (QWidget* child : detail_->findChildren<QWidget*>())
    {
        if (child->isVisibleTo(this) && child->isEnabled() &&
            (child->focusPolicy() & Qt::TabFocus) != 0)
        {
            child->setFocus(Qt::TabFocusReason);
            return;
        }
    }
    detail_scroll_->setFocus(Qt::TabFocusReason);
}

} // namespace vss::gui::support
