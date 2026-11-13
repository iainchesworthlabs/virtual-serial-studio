//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "shell/shell_window.hpp"
#include "shell/accessibility.hpp"

#include "advanced/add_fixture_dialog.hpp"
#include "advanced/benchmark_dialog.hpp"
#include "advanced/event_log.hpp"
#include "advanced/export_command_dialog.hpp"
#include "advanced/json_editor.hpp"
#include "advanced/owner_bar.hpp"
#include "advanced/wiring_dialog.hpp"
#include "backend/share_edit.hpp"
#include "dialogs/connect_ports.hpp"
#include "dialogs/network_endpoint.hpp"
#include "dialogs/recent_files.hpp"
#include "dialogs/share_wizard.hpp"
#include "dialogs/templates_dialog.hpp"
#include "flows/add_port_dialog.hpp"
#include "flows/attach_dialog.hpp"
#include "flows/com_number_dialog.hpp"
#include "flows/common.hpp"
#include "flows/pair_dialog.hpp"
#include "flows/port_ready_dialog.hpp"
#include "flows/release_port_dialog.hpp"
#include "flows/rename_dialog.hpp"
#include "flows/watch_dialog.hpp"
#include "panes/fixture_behaviour.hpp"
#include "panes/inspector_panel.hpp"
#include "panes/monitor_panel.hpp"
#include "panes/ports_panel.hpp"
#include "panes/send_panel.hpp"
#include "shell/canvas_controller.hpp"
#include "shell/command_palette.hpp"
#include "shell/format.hpp"
#include "shell/launcher.hpp"
#include "shell/port_warnings.hpp"
#include "shell/setup_group.hpp"
#include "shell/status_bar.hpp"
#include "shell/studio_tray.hpp"
#include "shell/undo_steps.hpp"
#include "simple/job_grid.hpp"
#include "simple/simple_page.hpp"
#include "support/about_dialog.hpp"
#include "support/build_info.hpp"
#include "support/diagnostic_bundle_dialog.hpp"
#include "support/help_contents_dialog.hpp"
#include "support/running_setup.hpp"
#include "support/settings.hpp"
#include "support/settings_dialog.hpp"
#include "support/driver_setup_dialog.hpp"
#include "support/setup_check_dialog.hpp"
#include "support/setup_facts.hpp"
#include "support/shortcuts_dialog.hpp"
#include "support/sign_in_start.hpp"
#include "support/troubleshoot_dialog.hpp"
#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "topology/links_list.hpp"
#include "topology/toolbox.hpp"
#include "topology/topology_text.hpp"
#include "topology/topology_view.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/dialogs.hpp"
#include "widgets/notes.hpp"
#include "widgets/surfaces.hpp"

#include <memory>
#include <vss/config/document.hpp>
#include <vss/studio/studio.hpp>

#include <vss/core/profile.hpp>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSet>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <optional>

namespace vss::gui
{
using namespace support;

namespace
{

constexpr const char* kRecentFiles = "ui/recentFiles";

QString document_name(const QString& path)
{
    return path.isEmpty() ? QObject::tr("untitled")
                          : QFileInfo(path).fileName();
}

} // namespace

ShellWindow::ShellWindow(StudioBackend& backend, QWidget* parent)
    : QMainWindow(parent), backend_(backend)
{
    Theme::instance().initialise();
    setObjectName(QStringLiteral("ShellWindow"));
    setMinimumSize(Theme::instance().size(QStringLiteral("layout.windowMin")));
    resize(Theme::instance().size(QStringLiteral("layout.windowDefault")));
    setDockOptions(QMainWindow::AllowTabbedDocks | QMainWindow::AnimatedDocks);
    setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);
    backend_.set_refresh_interval(load_ui_settings().refresh_ms);

    build_actions();
    build_menus();
    build_toolbar();
    build_central();
    build_docks();
    build_status();
    connect_backend();

    apply_theme_setting();
    apply_density_setting();
    apply_menu_palette();

    registry_.set_level(level_.level());
    {
        const QSignalBlocker blocker(advanced_action_);
        advanced_action_->setChecked(level_.advanced());
    }
    status_->set_advanced(level_.advanced());

    restore_layout();
    apply_compact();
    update_content();
    update_actions();
    update_owner_bar();
    update_title();
    last_signature_ = document_signature();

    connect(&level_, &LevelService::changed, this,
            &ShellWindow::on_level_changed);
    connect(&selection_, &SelectionService::port_changed, this,
            [this](vss::endpoint_id)
            {
                sync_selection();
                update_actions();
                update_owner_bar();
            });
    connect(&selection_, &SelectionService::link_changed, this,
            [this](vss::link_id)
            {
                sync_selection();
                update_actions();
                update_owner_bar();
            });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this]
            {
                launcher_->refresh();
                apply_menu_palette();
                update();
            });
}

ShellWindow::~ShellWindow()
{
    // Child docks emit visibilityChanged while QObject tears the window down.
    // Their lambdas use members that C++ destroys before the QObject base, so
    // sever every child-to-this connection first.
    const auto children = findChildren<QObject*>();
    for (QObject* child : children)
        QObject::disconnect(child, nullptr, this, nullptr);
    QObject::disconnect(&Theme::instance(), nullptr, this, nullptr);
    if (recreate_thread_ != nullptr)
    {
        recreate_thread_->wait();
        recreate_thread_ = nullptr;
    }
    monitor_action_ = nullptr;
    advanced_action_ = nullptr;
}

void ShellWindow::build_actions()
{
    const auto add =
        [this](const QString& id, const QString& text, const QString& keywords,
               const QString& icon, const QKeySequence& shortcut,
               action_level level, std::function<void()> handler,
               bool checkable = false, std::function<bool()> enabled = {},
               bool palette = true)
    {
        registry_.add({id, text, keywords, icon, shortcut, level, checkable,
                       std::move(handler), std::move(enabled), palette});
    };
    // File
    add(QStringLiteral("file.new"), tr("New"), tr("new document setup"),
        QStringLiteral("new-file"), QKeySequence::New, action_level::simple,
        [this] { new_document(); });
    add(QStringLiteral("file.open"), tr("Open"), tr("open file setup"),
        QStringLiteral("open"), QKeySequence::Open, action_level::simple,
        [this] { open_document(); });
    add(QStringLiteral("file.recent"), tr("Open recent"), tr("recent file"), {},
        {}, action_level::simple,
        [this]
        {
            const QStringList recent =
                QSettings().value(QLatin1String(kRecentFiles)).toStringList();
            if (!recent.isEmpty())
                open_path(recent.first());
        });
    add(QStringLiteral("file.templates"), tr("Setups and templates"),
        tr("template setup"), {}, {}, action_level::simple,
        [this] { open_templates(); });
    add(QStringLiteral("file.save"), tr("Save"), tr("save file setup"),
        QStringLiteral("save"), QKeySequence::Save, action_level::simple,
        [this] { save_document(); });
    add(QStringLiteral("file.saveAs"), tr("Save as"), tr("save as file"), {},
        QKeySequence::SaveAs, action_level::simple,
        [this] { save_document_as(); });
    add(QStringLiteral("file.exportCommand"), tr("Export as command"),
        tr("cli command"), {}, {}, action_level::advanced,
        [this] { open_export_command(); });
    add(QStringLiteral("file.bundle"), tr("Diagnostic bundle"),
        tr("support zip"), QStringLiteral("export"), {}, action_level::simple,
        [this] { open_bundle(); });
    add(QStringLiteral("file.quit"), tr("Quit"), tr("exit"), {},
        QKeySequence::Quit, action_level::simple,
        [this]
        {
            if (background_)
                quit_studio();
            else
                close();
        });

    // Edit
    // The engine cannot restore a snapshot exactly: a restored port gets a new
    // identity and, for a pseudo terminal, a new path.  Undo and Redo therefore
    // reverse only changes that have an exact inverse (connecting and removing
    // a link, changing an owner); see design-deviations.md.
    add(
        QStringLiteral("edit.undo"), tr("Undo"), tr("undo stack restore"), {},
        QKeySequence::Undo, action_level::simple, [this] { undo_last(); },
        false, [this] { return backend_.caps().undo && undo_.can_undo(); });
    add(
        QStringLiteral("edit.redo"), tr("Redo"), tr("redo stack restore"), {},
        QKeySequence::Redo, action_level::simple, [this] { redo_last(); },
        false, [this] { return backend_.caps().undo && undo_.can_redo(); });
    add(
        QStringLiteral("edit.remove"), tr("Remove selected"),
        tr("delete remove"), QStringLiteral("remove"),
        QKeySequence(Qt::Key_Delete), action_level::simple,
        [this] { remove_selection(); }, false,
        [this] { return selection_.port() != 0 || selection_.link() != 0; });

    // Ports
    add(QStringLiteral("ports.add"), tr("Add port"), tr("new virtual create"),
        QStringLiteral("add"), QKeySequence(QStringLiteral("Ctrl+Shift+N")),
        action_level::simple, [this] { show_add_port(); });
    add(QStringLiteral("ports.virtual"), tr("Virtual port"),
        tr("new virtual create"), {}, {}, action_level::simple,
        [this] { show_add_port(); });
    add(QStringLiteral("ports.pair"), tr("Linked pair"),
        tr("pair two null modem"), QStringLiteral("connect"), {},
        action_level::simple, [this] { show_pair(); });
    add(QStringLiteral("ports.attach"), tr("Attach real device"),
        tr("serial device"), QStringLiteral("device"), {}, action_level::simple,
        [this] { show_attach_device(); });
    add(
        QStringLiteral("ports.network"), tr("Network endpoint"),
        tr("tcp udp rfc2217"), {}, {}, action_level::advanced,
        [this] { open_network_endpoint(); }, false,
        [this] { return backend_.caps().bridge; });
    add(QStringLiteral("ports.mock"), tr("Mock fixture"),
        tr("fixture mock script"), QStringLiteral("fixture"), {},
        action_level::advanced, [this] { open_add_fixture(); });
    add(
        QStringLiteral("ports.copyName"), tr("Copy port name"),
        tr("clipboard path"), QStringLiteral("copy"),
        QKeySequence(QStringLiteral("Ctrl+Shift+C")), action_level::simple,
        [this] { copy_selected_port(); }, false,
        [this] { return selection_.port() != 0; });
    add(
        QStringLiteral("ports.rename"), tr("Rename port"),
        tr("name change relabel"), {}, QKeySequence(Qt::Key_F2),
        action_level::simple, [this] { show_rename(selection_.port()); }, false,
        [this]
        { return backend_.caps().rename_port && selection_.port() != 0; });
    add(
        QStringLiteral("ports.remove"), tr("Remove port"), tr("delete remove"),
        QStringLiteral("remove"), {}, action_level::simple,
        [this] { remove_selected_port(); }, false,
        [this] { return selection_.port() != 0; });
    add(
        QStringLiteral("topology.removeSetup"), tr("Remove setup"),
        tr("delete remove teardown close pair share attach everything"),
        QStringLiteral("remove"), {}, action_level::simple,
        [this] { remove_selected_setup(); }, false,
        [this] { return selection_.port() != 0 || selection_.link() != 0; });

    // Topology
    add(
        QStringLiteral("topology.connect"), tr("Connect ports"),
        tr("link split join redirect monitor"), QStringLiteral("connect"),
        QKeySequence(QStringLiteral("Ctrl+L")), action_level::simple,
        [this] { open_connect_ports(); }, false,
        [this] { return backend_.ports().size() >= 2; });
    add(
        QStringLiteral("topology.share"), tr("Share a device"),
        tr("split listeners"), QStringLiteral("share"), {},
        action_level::simple, [this] { open_share_wizard(); }, false,
        [this] { return backend_.caps().share; });
    // The design's Advanced toolbar adds Share.  It is a placement-only
    // duplicate of the Simple Topology action, so it stays out of the palette.
    add(
        QStringLiteral("topology.shareToolbar"), tr("Share a device"),
        tr("split listeners"), QStringLiteral("share"), {},
        action_level::advanced, [this] { open_share_wizard(); }, false, {},
        false);
    add(QStringLiteral("topology.watch"), tr("Watch traffic"),
        tr("monitor copy"), QStringLiteral("watch"), {}, action_level::simple,
        [this] { show_watch(); });
    add(
        QStringLiteral("topology.addProgram"), tr("Add a program port"),
        tr("share attach more listener another"), {}, {}, action_level::simple,
        [this] { add_program_port(); }, false,
        [this] { return share_link_of_selection() != 0; });
    add(
        QStringLiteral("topology.removeLink"), tr("Remove link"),
        tr("delete disconnect"), QStringLiteral("remove"), {},
        action_level::simple, [this] { remove_selected_link(); }, false,
        [this] { return selection_.link() != 0; });
    add(QStringLiteral("topology.wiring"), tr("Link wiring"),
        tr("line matrix faults"), {}, {}, action_level::advanced,
        [this] { open_wiring(); });
    add(QStringLiteral("topology.owner"), tr("Owner"), tr("policy arbitration"),
        {}, {}, action_level::advanced, [this] { show_owner_controls(); });
    add(QStringLiteral("topology.fit"), tr("Fit"), tr("zoom"),
        QStringLiteral("fit"), QKeySequence(QStringLiteral("Ctrl+0")),
        action_level::advanced,
        [this]
        {
            if (topology_ != nullptr)
                topology_->fit_content();
        });
    add(QStringLiteral("topology.addPort"), tr("Add port"),
        tr("canvas new port"), QStringLiteral("port"), {},
        action_level::advanced,
        [this]
        {
            if (canvas_ != nullptr)
                canvas_->add_item(canvas_item::port);
        });
    add(QStringLiteral("topology.addFixture"), tr("Add fixture"),
        tr("canvas new mock fixture"), QStringLiteral("fixture"), {},
        action_level::advanced,
        [this]
        {
            if (canvas_ != nullptr)
                canvas_->add_item(canvas_item::fixture);
        });
    add(QStringLiteral("topology.addPair"), tr("Add pair"),
        tr("canvas bundle two ports null modem"), QStringLiteral("pair"), {},
        action_level::advanced,
        [this]
        {
            if (canvas_ != nullptr)
                canvas_->add_item(canvas_item::pair);
        });
    add(
        QStringLiteral("edit.duplicate"), tr("Duplicate"),
        tr("copy ports canvas"), {}, QKeySequence(QStringLiteral("Ctrl+D")),
        action_level::advanced,
        [this]
        {
            if (canvas_ != nullptr && topology_ != nullptr)
                canvas_->duplicate(topology_->selected_ports());
        },
        false,
        [this]
        {
            return topology_ != nullptr &&
                   !topology_->selected_ports().isEmpty();
        });
    add(
        QStringLiteral("view.toolbox"), tr("Show toolbox"),
        tr("canvas tiles palette"), {},
        QKeySequence(QStringLiteral("Ctrl+Shift+B")), action_level::advanced,
        [] {}, true);
    add(QStringLiteral("topology.tidy"), tr("Tidy layout"),
        tr("arrange align overlap"), {}, {}, action_level::advanced,
        [this]
        {
            if (topology_ != nullptr)
                topology_->tidy_layout();
        });

    // Tools
    add(QStringLiteral("tools.search"), tr("Search actions"),
        tr("command palette"), QStringLiteral("search"),
        QKeySequence(QStringLiteral("Ctrl+K")), action_level::simple,
        [this] { open_palette(); });
    add(QStringLiteral("tools.setupCheck"), tr("Setup check"),
        tr("driver com0com permissions"), {}, {}, action_level::simple,
        [this] { open_setup_check(); });
    add(QStringLiteral("tools.benchmark"), tr("Benchmark"),
        tr("throughput speed"), {}, {}, action_level::advanced,
        [this] { open_benchmark(); });
    add(QStringLiteral("tools.refresh"), tr("Refresh now"), tr("reload"),
        QStringLiteral("refresh"), QKeySequence(Qt::Key_F5),
        action_level::simple,
        [this]
        {
            backend_.refresh();
            status_->refresh();
            status_->showMessage(tr("Refreshed."), 2000);
        });
    add(QStringLiteral("tools.settings"), tr("Settings"),
        tr("preferences theme density refresh baud"),
        QStringLiteral("settings"), QKeySequence(QStringLiteral("Ctrl+,")),
        action_level::simple, [this] { open_settings(); });

    // View
    add(
        QStringLiteral("view.advanced"), tr("Advanced mode"),
        tr("simple expert level"), {},
        QKeySequence(QStringLiteral("Ctrl+Shift+A")), action_level::simple,
        [] {}, true);
    add(
        QStringLiteral("view.monitor"), tr("Monitor"), tr("traffic"), {}, {},
        action_level::simple, [] {}, true);
    add(
        QStringLiteral("view.links"), tr("Show links as a list"),
        tr("links topology list accessible"), {}, {}, action_level::simple,
        [] {}, true);
    add(
        QStringLiteral("view.log"), tr("Log"), tr("events"), {}, {},
        action_level::advanced, [] {}, true);
    add(
        QStringLiteral("view.json"), tr("Topology JSON"), tr("editor schema"),
        {}, {}, action_level::advanced, [] {}, true);
    add(
        QStringLiteral("view.theme.system"), tr("Follow the system"),
        tr("theme colour scheme"), {}, {}, action_level::simple,
        []
        {
            Theme::instance().set_mode(theme_mode::system);
            QSettings().setValue(QStringLiteral("ui/theme"),
                                 QStringLiteral("system"));
        },
        true);
    add(
        QStringLiteral("view.theme.light"), tr("Light"), tr("theme"), {}, {},
        action_level::simple,
        []
        {
            Theme::instance().set_mode(theme_mode::light);
            QSettings().setValue(QStringLiteral("ui/theme"),
                                 QStringLiteral("light"));
        },
        true);
    add(
        QStringLiteral("view.theme.dark"), tr("Dark"), tr("theme"), {}, {},
        action_level::simple,
        []
        {
            Theme::instance().set_mode(theme_mode::dark);
            QSettings().setValue(QStringLiteral("ui/theme"),
                                 QStringLiteral("dark"));
        },
        true);
    add(
        QStringLiteral("view.density.comfortable"), tr("Comfortable"),
        tr("density spacing"), {}, {}, action_level::simple,
        []
        {
            Theme::instance().set_density(density_mode::comfortable);
            QSettings().setValue(QStringLiteral("ui/density"),
                                 QStringLiteral("comfortable"));
        },
        true);
    add(
        QStringLiteral("view.density.compact"), tr("Compact"),
        tr("density spacing"), {}, {}, action_level::simple,
        []
        {
            Theme::instance().set_density(density_mode::compact);
            QSettings().setValue(QStringLiteral("ui/density"),
                                 QStringLiteral("compact"));
        },
        true);
    add(QStringLiteral("view.resetLayout"), tr("Reset layout"),
        tr("docks window"), {}, {}, action_level::simple,
        [this] { reset_layout(); });
    add(QStringLiteral("view.showLauncher"), tr("Show launcher"),
        tr("empty start"), {}, {}, action_level::simple,
        [this] { show_launcher(); });

    // Help
    add(QStringLiteral("help.contents"), tr("Help contents"),
        tr("manual help topics"), {}, QKeySequence(Qt::Key_F1),
        action_level::simple, [this] { open_help_contents(); });
    add(QStringLiteral("help.why"), tr("Why is nothing arriving?"),
        tr("troubleshoot no data"), {}, {}, action_level::simple,
        [this] { open_troubleshoot(); });
    add(QStringLiteral("help.shortcuts"), tr("Keyboard shortcuts"), tr("keys"),
        {}, {}, action_level::simple, [this] { open_shortcuts(); });
    add(QStringLiteral("help.environment"), tr("Copy environment"),
        tr("build bug report"), QStringLiteral("copy"), {},
        action_level::simple, [this] { copy_environment(); });
    add(QStringLiteral("help.about"), tr("About"),
        tr("version licence copyright"), {}, {}, action_level::simple,
        [this] { open_about(); });

    connect(&undo_, &UndoService::changed, this, [this] { update_actions(); });

    advanced_action_ = registry_.action(QStringLiteral("view.advanced"));
    monitor_action_ = registry_.action(QStringLiteral("view.monitor"));
    links_action_ = registry_.action(QStringLiteral("view.links"));
    log_action_ = registry_.action(QStringLiteral("view.log"));
    json_action_ = registry_.action(QStringLiteral("view.json"));
    if (QAction* toolbox = registry_.action(QStringLiteral("view.toolbox")))
    {
        toolbox->setChecked(true);
        connect(toolbox, &QAction::toggled, this,
                [this] { sync_canvas_editing(); });
    }
    if (QAction* network = registry_.action(QStringLiteral("ports.network"));
        network != nullptr && !backend_.caps().bridge)
    {
        network->setToolTip(tr("Not available in this build"));
    }
    if (QAction* share = registry_.action(QStringLiteral("topology.share"));
        share != nullptr && !backend_.caps().share)
    {
        share->setToolTip(tr("Not available in this build"));
    }
    connect(advanced_action_, &QAction::toggled, this,
            [this](bool on) { level_.set_advanced(on); });

    auto* theme_group = new QActionGroup(this);
    theme_group->setExclusive(true);
    theme_group->addAction(
        registry_.action(QStringLiteral("view.theme.system")));
    theme_group->addAction(
        registry_.action(QStringLiteral("view.theme.light")));
    theme_group->addAction(registry_.action(QStringLiteral("view.theme.dark")));

    auto* density_group = new QActionGroup(this);
    density_group->setExclusive(true);
    density_group->addAction(
        registry_.action(QStringLiteral("view.density.comfortable")));
    density_group->addAction(
        registry_.action(QStringLiteral("view.density.compact")));
}

void ShellWindow::build_menus()
{
    const auto add = [this](QMenu* menu, const QString& id)
    {
        if (QAction* action = registry_.action(id))
            menu->addAction(action);
    };
    const auto add_menu = [this](const QString& title)
    { return menuBar()->addMenu(title); };

    QMenu* file = add_menu(tr("&File"));
    add(file, QStringLiteral("file.new"));
    add(file, QStringLiteral("file.open"));
    add(file, QStringLiteral("file.recent"));
    add(file, QStringLiteral("file.templates"));
    file->addSeparator();
    add(file, QStringLiteral("file.save"));
    add(file, QStringLiteral("file.saveAs"));
    file->addSeparator();
    add(file, QStringLiteral("file.exportCommand"));
    add(file, QStringLiteral("file.bundle"));
    file->addSeparator();
    add(file, QStringLiteral("file.quit"));

    QMenu* edit = add_menu(tr("&Edit"));
    add(edit, QStringLiteral("edit.undo"));
    add(edit, QStringLiteral("edit.redo"));
    edit->addSeparator();
    add(edit, QStringLiteral("edit.remove"));
    add(edit, QStringLiteral("edit.duplicate"));
    add(edit, QStringLiteral("topology.removeSetup"));

    QMenu* ports = add_menu(tr("&Ports"));
    add(ports, QStringLiteral("ports.add"));
    add(ports, QStringLiteral("ports.virtual"));
    add(ports, QStringLiteral("ports.pair"));
    add(ports, QStringLiteral("ports.attach"));
    ports->addSeparator();
    add(ports, QStringLiteral("ports.network"));
    add(ports, QStringLiteral("ports.mock"));
    ports->addSeparator();
    add(ports, QStringLiteral("ports.copyName"));
    if (backend_.caps().rename_port)
        add(ports, QStringLiteral("ports.rename"));
    add(ports, QStringLiteral("ports.remove"));
    add(ports, QStringLiteral("topology.removeSetup"));

    QMenu* topology = add_menu(tr("&Topology"));
    add(topology, QStringLiteral("topology.connect"));
    add(topology, QStringLiteral("topology.share"));
    add(topology, QStringLiteral("topology.watch"));
    add(topology, QStringLiteral("topology.addProgram"));
    topology->addSeparator();
    add(topology, QStringLiteral("topology.removeLink"));
    add(topology, QStringLiteral("topology.wiring"));
    add(topology, QStringLiteral("topology.owner"));
    topology->addSeparator();
    add(topology, QStringLiteral("topology.addPort"));
    add(topology, QStringLiteral("topology.addFixture"));
    add(topology, QStringLiteral("topology.addPair"));
    topology->addSeparator();
    add(topology, QStringLiteral("topology.tidy"));
    add(topology, QStringLiteral("topology.fit"));

    QMenu* tools = add_menu(tr("&Tools"));
    add(tools, QStringLiteral("tools.search"));
    add(tools, QStringLiteral("tools.setupCheck"));
    add(tools, QStringLiteral("tools.benchmark"));
    tools->addSeparator();
    add(tools, QStringLiteral("tools.refresh"));
    add(tools, QStringLiteral("tools.settings"));

    QMenu* view = add_menu(tr("&View"));
    add(view, QStringLiteral("view.advanced"));
    view->addSeparator();
    add(view, QStringLiteral("view.monitor"));
    add(view, QStringLiteral("view.links"));
    add(view, QStringLiteral("view.toolbox"));
    add(view, QStringLiteral("view.log"));
    add(view, QStringLiteral("view.json"));
    view->addSeparator();
    QMenu* theme = view->addMenu(tr("Theme"));
    add(theme, QStringLiteral("view.theme.system"));
    add(theme, QStringLiteral("view.theme.light"));
    add(theme, QStringLiteral("view.theme.dark"));
    QMenu* density = view->addMenu(tr("Density"));
    add(density, QStringLiteral("view.density.comfortable"));
    add(density, QStringLiteral("view.density.compact"));
    view->addSeparator();
    add(view, QStringLiteral("view.resetLayout"));
    add(view, QStringLiteral("view.showLauncher"));

    QMenu* help = add_menu(tr("&Help"));
    add(help, QStringLiteral("help.contents"));
    add(help, QStringLiteral("help.why"));
    add(help, QStringLiteral("help.shortcuts"));
    add(help, QStringLiteral("help.environment"));
    add(help, QStringLiteral("help.about"));
}

void ShellWindow::build_toolbar()
{
    // The tool row sits at the top of the topology pane, next to what it acts
    // on; it is not a window-wide toolbar.  New, Open and Save live in File
    // with their shortcuts.  See docs/10-design-language.md.
    toolbar_ = new QToolBar(tr("Main toolbar"), this);
    toolbar_->setObjectName(QStringLiteral("mainToolbar"));
    toolbar_->setMovable(false);
    toolbar_->setFloatable(false);
    toolbar_->setIconSize(QSize(16, 16));
    toolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar_->setStyleSheet(QStringLiteral(
        "QToolBar#mainToolbar { background: transparent; padding: 0; }"));

    const auto add = [this](const QString& id)
    {
        if (QAction* action = registry_.action(id))
            toolbar_->addAction(action);
    };
    add(QStringLiteral("ports.attach"));
    add(QStringLiteral("topology.connect"));
    toolbar_->addSeparator();
    add(QStringLiteral("ports.mock"));
    add(QStringLiteral("topology.shareToolbar"));
    add(QStringLiteral("topology.fit"));
    toolbar_->addSeparator();
    add(QStringLiteral("ports.remove"));
    add(QStringLiteral("topology.removeSetup"));

    // Menus, the action search and the mode switch share one row: the search
    // and the switch are the menu bar's right-hand corner widget.
    auto* corner = new QWidget(this);
    corner->setObjectName(QStringLiteral("headerCorner"));
    auto* corner_layout = new QHBoxLayout(corner);
    corner_layout->setContentsMargins(0, 0, Theme::instance().space(1), 0);
    corner_layout->setSpacing(Theme::instance().space(2));

    search_ = new QLineEdit(corner);
    search_->setProperty("search", true);
    search_->setPlaceholderText(tr("Search actions"));
    search_->setAccessibleName(tr("Search actions"));
    search_->setClearButtonEnabled(true);
    search_->setFixedWidth(280);
    search_->setFont(Theme::instance().font(text_style::body));
    // The search field leads with the magnifier from the icon set.
    search_
        ->addAction(IconProvider::instance().icon(QStringLiteral("search"),
                                                  colour_role::ink3, 16),
                    QLineEdit::LeadingPosition)
        ->setText(tr("Search actions"));
    corner_layout->addWidget(search_);
    connect(search_, &QLineEdit::returnPressed, this,
            [this] { open_palette(); });
    menuBar()->setCornerWidget(corner, Qt::TopRightCorner);
}

void ShellWindow::build_central()
{
    // The banners sit above the body, so they show in Simple and Advanced
    // mode alike: one offers to start again what was running when the user
    // last quit (hidden until background running finds such a setup), the
    // other offers to recreate ports a restart took away.
    auto* body = new QWidget(this);
    auto* body_layout = new QVBoxLayout(body);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);
    resume_banner_ = new NoteBanner(NoteBanner::kind::info, QString(), body);
    resume_banner_->setObjectName(QStringLiteral("resumeBanner"));
    resume_banner_->set_action(tr("Start again"),
                               [this] { start_running_setup(false); });
    resume_banner_->setVisible(false);
    body_layout->addWidget(resume_banner_);
    lost_banner_ = new NoteBanner(
        NoteBanner::kind::danger,
        tr("Some COM ports were lost. The helper service or the driver was "
           "restarted and removed the ports your programs had open."),
        body);
    lost_banner_->set_action(tr("Recreate ports"),
                             [this] { recreate_lost_ports(); });
    lost_banner_->setVisible(false);
    body_layout->addWidget(lost_banner_);
    central_ = new QStackedWidget(body);
    body_layout->addWidget(central_, 1);
    setCentralWidget(body);

    launcher_ = new Launcher(backend_, central_);
    connect(launcher_, &Launcher::job_requested, this,
            [this](const QString& job) { run_job(job); });
    connect(launcher_, &Launcher::setup_check_requested, this,
            [this] { open_setup_check(); });
    connect(launcher_, &Launcher::driver_setup_requested, this,
            [this] { open_driver_setup(); });
    connect(launcher_, &Launcher::browse_requested, this,
            [this] { open_document(); });
    connect(launcher_, &Launcher::open_recent_requested, this,
            [this](const QString& path) { open_path(path); });
    connect(launcher_, &Launcher::advanced_requested, this,
            [this] { level_.set_advanced(true); });
    central_->addWidget(launcher_);

    // Simple mode is the setups list and a guided page per job; the dock
    // grid and the canvas are Advanced mode (screens/08-simple-mode.md).
    simple_page_ = new simple::SimplePage(backend_, central_);
    connect(simple_page_, &simple::SimplePage::check_requested, this,
            [this] { open_setup_check(); });
    connect(simple_page_, &simple::SimplePage::driver_setup_requested, this,
            [this] { open_driver_setup(); });
    connect(simple_page_, &simple::SimplePage::open_requested, this,
            [this] { open_document(); });
    connect(simple_page_, &simple::SimplePage::save_requested, this,
            [this] { save_document(); });
    connect(simple_page_, &simple::SimplePage::examples_requested, this,
            [this] { open_templates(); });
    connect(simple_page_, &simple::SimplePage::advanced_requested, this,
            [this] { level_.set_advanced(true); });
    connect(simple_page_, &simple::SimplePage::troubleshoot_requested, this,
            [this] { open_troubleshoot(); });
    connect(simple_page_, &simple::SimplePage::remove_setup_requested, this,
            [this](vss::endpoint_id port)
            {
                remove_setup(
                    find_setup(backend_.ports(), backend_.links(), port));
            });
    connect(simple_page_, &simple::SimplePage::remove_all_requested, this,
            [this] { remove_all_setups(); });
    connect(simple_page_, &simple::SimplePage::setup_selected, this,
            [this](vss::endpoint_id port)
            {
                selection_.select_link(0);
                selection_.select_port(port);
            });
    connect(simple_page_, &simple::SimplePage::message, this,
            [this](const QString& text)
            {
                status_->showMessage(text, 6000);
                log_local(text);
            });
    central_->addWidget(simple_page_);

    workspace_ = new QWidget(central_);
    auto* layout = new QVBoxLayout(workspace_);
    // The main window separators give the 8 px gap between panes.
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(Theme::instance().space(1));

    auto* card = new QFrame(workspace_);

    card->setObjectName(QStringLiteral("topologyPane"));
    card->setProperty("pane", true);
    polish(card);
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(1, 1, 1, 1);
    card_layout->setSpacing(0);

    // The tool row heads the pane, where a dock would have its title.
    auto* tools = new QWidget(card);
    auto* tools_layout = new QHBoxLayout(tools);
    tools_layout->setContentsMargins(Theme::instance().space(1), 6,
                                     Theme::instance().space(1), 6);
    tools_layout->setSpacing(0);
    toolbar_->setParent(tools);
    tools_layout->addWidget(toolbar_, 1);
    card_layout->addWidget(tools);

    topology_ = new topology::TopologyView(card);
    topology_->set_capabilities(backend_.caps());
    topology_->set_link_style(load_ui_settings().links);
    // Inset the diagram so its square view stays inside the pane's corners.
    // In Advanced mode the toolbox rail sits on the left of the canvas.
    auto* canvas_layout = new QHBoxLayout;
    const int inset = Theme::instance().space(1);
    canvas_layout->setContentsMargins(inset, 0, inset, inset);
    canvas_layout->setSpacing(inset);
    toolbox_ = new topology::Toolbox(card);
    toolbox_->setVisible(false);
    canvas_layout->addWidget(toolbox_);
    canvas_layout->addWidget(topology_, 1);
    card_layout->addLayout(canvas_layout, 1);
    layout->addWidget(card, 1);

    // The link bar under the diagram.  WP4 owns the diagram; the bar and its
    // owner arbitration belong to this package and can move with the dock
    // without changing its API.
    owner_bar_ = new OwnerBar(backend_, workspace_);
    connect(owner_bar_, &OwnerBar::message, this,
            [this](const QString& text)
            {
                status_->showMessage(text, 6000);
                log_local(text);
            });
    connect(
        owner_bar_, &OwnerBar::owner_changed, this,
        [this](vss::link_id id, vss::endpoint_id before, vss::endpoint_id after)
        {
            if (const auto link = backend_.link(id))
                undo_.push(owner_step(backend_, *link, before, after,
                                      tr("Change owner")));
        });
    layout->addWidget(owner_bar_);
    central_->addWidget(workspace_);

    connect(topology_, &topology::TopologyView::port_selected, this,
            [this](vss::endpoint_id id)
            {
                if (id != 0)
                {
                    selection_.select_link(0);
                    selection_.select_port(id);
                }
                else
                {
                    selection_.select_port(0);
                }
            });
    connect(topology_, &topology::TopologyView::link_selected, this,
            [this](vss::link_id id)
            {
                if (id != 0)
                {
                    selection_.select_port(0);
                    selection_.select_link(id);
                }
                else
                {
                    selection_.select_link(0);
                }
            });
    connect(topology_, &topology::TopologyView::inspect_requested, this,
            [this](vss::endpoint_id) { focus_inspector(); });
    connect(topology_, &topology::TopologyView::copy_name_requested, this,
            [this](vss::endpoint_id id) { copy_port_name(id); });
    connect(topology_, &topology::TopologyView::connect_requested, this,
            [this](vss::endpoint_id id) { connect_from_port(id); });
    connect(topology_, &topology::TopologyView::remove_port_requested, this,
            [this](vss::endpoint_id id) { remove_port(id); });
    connect(topology_, &topology::TopologyView::remove_link_requested, this,
            [this](vss::link_id id) { remove_link(id); });
    connect(topology_, &topology::TopologyView::rename_requested, this,
            [this](vss::endpoint_id id) { show_rename(id); });
    connect(topology_, &topology::TopologyView::status_message, this,
            [this](const QString& text) { status_->showMessage(text, 4000); });
    connect(topology_, &topology::TopologyView::selection_changed, this,
            [this](int) { update_actions(); });

    canvas_ = new CanvasController(backend_, undo_, topology_, toolbox_, this);
    connect(canvas_, &CanvasController::message, this,
            [this](const QString& text)
            {
                status_->showMessage(text, 6000);
                log_local(text);
            });
    connect(canvas_, &CanvasController::driver_setup_requested, this,
            [this] { open_driver_setup(); });
    connect(canvas_, &CanvasController::batch_finished, this,
            [this]
            {
                // A canvas step keeps the history: it was recorded by name.
                std::vector<vss::endpoint_id> ports;
                for (const auto& port : backend_.ports())
                    ports.push_back(port.id);
                std::sort(ports.begin(), ports.end());
                history_ports_ = std::move(ports);
                on_backend_changed();
            });
    canvas_->set_flow_opener(
        [this](canvas_item item)
        {
            switch (item)
            {
            case canvas_item::device:
                show_attach_device();
                break;
            case canvas_item::network:
                open_network_endpoint();
                break;
            case canvas_item::share:
                open_share_wizard();
                break;
            default:
                break;
            }
        });
    sync_canvas_editing();
}

void ShellWindow::build_docks()
{
    // Every dock is drawn as a pane: a rounded surface on the window ground
    // with its own header.  The dock's title bar is replaced by an empty
    // widget, so there are no float or close buttons; View shows and hides
    // the panes and Reset layout puts them back.
    const auto make_pane_dock = [this](const QString& object_name,
                                       const QString& title, QWidget* page,
                                       PaneHeader** header)
    {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(object_name);
        dock->setAllowedAreas(Qt::AllDockWidgetAreas);
        dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
        dock->setTitleBarWidget(new QWidget(dock));
        auto* pane = new Pane(title, dock);
        pane->setObjectName(object_name + QStringLiteral("Pane"));
        pane->set_body(page);
        if (header != nullptr)
            *header = pane->header();
        dock->setWidget(pane);
        return dock;
    };

    ports_panel_ = new PortsPanel(backend_, selection_, this);
    inspector_panel_ = new InspectorPanel(backend_, this);
    send_panel_ = new SendPanel(backend_, this);
    monitor_panel_ = new MonitorPanel(backend_, this);
    behaviours_ = new FixtureBehaviours(backend_, this);
    inspector_panel_->set_behaviours(behaviours_);

    ports_dock_ = make_pane_dock(QStringLiteral("portsDock"), tr("Ports"),
                                 ports_panel_, &ports_header_);
    inspector_dock_ =
        make_pane_dock(QStringLiteral("inspectorDock"), tr("Inspector"),
                       inspector_panel_, &inspector_header_);
    send_dock_ = make_pane_dock(QStringLiteral("sendDock"), tr("Send"),
                                send_panel_, &send_header_);
    monitor_dock_ = make_pane_dock(QStringLiteral("monitorDock"), tr("Monitor"),
                                   monitor_panel_, &monitor_header_);

    // The Monitor controls share the pane's title row while it is wide enough.
    monitor_panel_->attach_header(monitor_header_);

    // The Ports pane carries its own Add port button.
    if (QAction* add = registry_.action(QStringLiteral("ports.add")))
    {
        auto* button = new QToolButton(ports_header_);
        button->setDefaultAction(add);
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setObjectName(QStringLiteral("portsAddButton"));
        button->setProperty("role", QStringLiteral("secondary"));
        ports_header_->add_action(button);
    }

    ports_dock_->setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.ports.default")));
    inspector_dock_->setMinimumWidth(
        Theme::instance().metric(QStringLiteral("layout.inspector.default")));
    send_dock_->setMinimumHeight(
        Theme::instance().metric(QStringLiteral("layout.send.default")) + 24);
    monitor_dock_->setMinimumHeight(
        Theme::instance().metric(QStringLiteral("layout.monitor.default")));

    addDockWidget(Qt::LeftDockWidgetArea, ports_dock_);
    addDockWidget(Qt::RightDockWidgetArea, inspector_dock_);
    addDockWidget(Qt::RightDockWidgetArea, send_dock_);
    splitDockWidget(inspector_dock_, send_dock_, Qt::Vertical);
    addDockWidget(Qt::BottomDockWidgetArea, monitor_dock_);

    connect(ports_panel_, &PortsPanel::count_changed, this,
            [this](int) { update_dock_titles(); });
    connect(ports_panel_, &PortsPanel::inspect_requested, this,
            [this](vss::endpoint_id id)
            {
                selection_.select_port(id);
                focus_inspector();
            });
    connect(ports_panel_, &PortsPanel::remove_requested, this,
            [this](vss::endpoint_id id)
            {
                selection_.select_port(id);
                remove_selected_port();
            });
    ports_panel_->set_menu_state_provider([this](vss::endpoint_id id)
                                          { return port_menu_state_for(id); });
    topology_->set_menu_state_provider([this](vss::endpoint_id id)
                                       { return port_menu_state_for(id); });
    connect(ports_panel_, &PortsPanel::port_action_requested, this,
            [this](vss::endpoint_id id, port_menu_action action)
            { run_port_action(id, action); });
    connect(topology_, &topology::TopologyView::port_action_requested, this,
            [this](vss::endpoint_id id, port_menu_action action)
            { run_port_action(id, action); });
    connect(behaviours_, &FixtureBehaviours::error_message, this,
            [this](const QString& message)
            { ToastManager::instance().show(this, message); });
    connect(ports_panel_, &PortsPanel::path_copied, this,
            [this](const QString& path)
            { status_->showMessage(tr("Copied %1.").arg(path), 2500); });
    connect(ports_panel_, &PortsPanel::status_message, this,
            [this](const QString& message)
            { status_->showMessage(message, 3000); });
    connect(inspector_panel_, &InspectorPanel::troubleshoot_requested, this,
            [this] { open_troubleshoot(); });
    connect(inspector_panel_, &InspectorPanel::advanced_tools_requested, this,
            [this] { open_wiring(); });
    connect(inspector_panel_, &InspectorPanel::com_number_requested, this,
            [this](vss::endpoint_id id) { show_com_number(id); });
    connect(inspector_panel_, &InspectorPanel::release_port_requested, this,
            [this](vss::endpoint_id id) { show_release_port(id); });
    connect(inspector_panel_, &InspectorPanel::error_message, this,
            [this](const QString& message)
            { ToastManager::instance().show(this, message); });
    connect(send_panel_, &SendPanel::sent, this,
            [this](int bytes, const QString& name)
            {
                if (bytes > 0)
                    status_->showMessage(
                        tr("Queued %1 bytes to %2.").arg(bytes).arg(name),
                        3000);
            });
    connect(send_panel_, &SendPanel::status_message, this,
            [this](const QString& message)
            { status_->showMessage(message, 3000); });
    connect(send_panel_, &SendPanel::error_message, this,
            [this](const QString& message)
            { ToastManager::instance().show(this, message); });
    connect(monitor_panel_, &MonitorPanel::port_selected, this,
            [this](vss::endpoint_id id) { selection_.select_port(id); });
    connect(monitor_panel_, &MonitorPanel::status_message, this,
            [this](const QString& message)
            { status_->showMessage(message, 3000); });
    connect(monitor_panel_, &MonitorPanel::error_message, this,
            [this](const QString& message)
            { ToastManager::instance().show(this, message); });

    connect(&selection_, &SelectionService::port_changed, this,
            [this](vss::endpoint_id id)
            {
                inspector_panel_->set_selected_port(id);
                send_panel_->set_selected_port(id);
                monitor_panel_->set_selected_port(id);
                update_dock_titles();
            });
    inspector_panel_->set_selected_port(selection_.port());
    send_panel_->set_selected_port(selection_.port());
    monitor_panel_->set_selected_port(selection_.port());

    if (QAction* action = monitor_action_)
    {
        action->setChecked(true);
        connect(action, &QAction::toggled, this,
                [this](bool shown)
                {
                    if (monitor_dock_ != nullptr)
                        monitor_dock_->setVisible(shown);
                });
    }
    connect(monitor_dock_, &QDockWidget::visibilityChanged, this,
            [this](bool visible)
            {
                if (monitor_action_ == nullptr)
                    return;
                const QSignalBlocker blocker(monitor_action_);
                monitor_action_->setChecked(visible);
            });

    ports_panel_->set_advanced(level_.advanced());
    inspector_panel_->set_advanced(level_.advanced());
    send_panel_->set_advanced(level_.advanced());
    monitor_panel_->set_show_advanced_tabs(false);
    monitor_panel_->set_advanced(level_.advanced());
    update_dock_titles();

    links_dock_ = new QDockWidget(tr("Links"), this);
    links_dock_->setObjectName(QStringLiteral("linksDock"));
    links_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    links_ = new topology::LinksList(links_dock_);
    links_dock_->setWidget(links_);
    addDockWidget(Qt::BottomDockWidgetArea, links_dock_);
    tabifyDockWidget(monitor_dock_, links_dock_);
    links_dock_->hide();

    connect(links_, &topology::LinksList::link_selected, this,
            [this](vss::link_id id)
            {
                if (id != 0)
                {
                    selection_.select_port(0);
                    selection_.select_link(id);
                }
                else
                {
                    selection_.select_link(0);
                }
            });
    if (links_action_ != nullptr)
    {
        links_action_->setChecked(false);
        connect(links_action_, &QAction::toggled, this,
                [this](bool shown) { show_links_dock(shown); });
    }
    connect(links_dock_, &QDockWidget::visibilityChanged, this,
            [this](bool visible)
            {
                if (links_action_ == nullptr)
                    return;
                const QSignalBlocker blocker(links_action_);
                links_action_->setChecked(visible);
            });

    // Advanced bottom tabs.  Log and Topology JSON are tabified with the
    // Monitor dock; WP3 owns the Monitor pane, this package owns these two.
    event_log_ = new EventLogPanel(this);
    log_dock_ = new QDockWidget(tr("Log"), this);
    log_dock_->setObjectName(QStringLiteral("logDock"));
    log_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    log_dock_->setWidget(event_log_);
    log_dock_->setMinimumHeight(
        Theme::instance().metric(QStringLiteral("layout.monitor.min")));

    json_editor_ = new TopologyJsonEditor(backend_, this);
    connect(json_editor_, &TopologyJsonEditor::message, this,
            [this](const QString& text)
            {
                if (status_ != nullptr)
                    status_->showMessage(text, 6000);
            });
    connect(json_editor_, &TopologyJsonEditor::applied, this,
            [this] { log_local(tr("Applied the topology JSON.")); });
    json_dock_ = new QDockWidget(tr("Topology JSON"), this);
    json_dock_->setObjectName(QStringLiteral("jsonDock"));
    json_dock_->setAllowedAreas(Qt::AllDockWidgetAreas);
    json_dock_->setWidget(json_editor_);
    json_dock_->setMinimumHeight(
        Theme::instance().metric(QStringLiteral("layout.monitor.min")));

    addDockWidget(Qt::BottomDockWidgetArea, log_dock_);
    addDockWidget(Qt::BottomDockWidgetArea, json_dock_);
    tabifyDockWidget(monitor_dock_, log_dock_);
    tabifyDockWidget(log_dock_, json_dock_);
    monitor_dock_->raise();

    const auto wire_tab = [this](QAction* action, QDockWidget* dock)
    {
        if (action == nullptr || dock == nullptr)
            return;
        {
            const QSignalBlocker blocker(action);
            action->setChecked(true);
        }
        connect(action, &QAction::toggled, this,
                [this, dock](bool shown)
                {
                    if (dock == nullptr)
                        return;
                    dock->setVisible(shown && level_.advanced());
                    // The Log and the JSON editor are tabs behind the Monitor:
                    // choosing one from the View menu has to bring it up, or
                    // the command seems to do nothing.
                    if (shown && level_.advanced())
                        dock->raise();
                });
        connect(dock, &QDockWidget::visibilityChanged, this,
                [this, action](bool visible)
                {
                    // Hiding every advanced dock in Simple mode is the shell's
                    // own action; keep the user's tab choice for later.
                    if (!level_.advanced())
                        return;
                    const QSignalBlocker blocker(action);
                    action->setChecked(visible);
                });
    };
    wire_tab(log_action_, log_dock_);
    wire_tab(json_action_, json_dock_);

    resizeDocks(
        {ports_dock_},
        {Theme::instance().metric(QStringLiteral("layout.ports.default"))},
        Qt::Horizontal);
    resizeDocks(
        {inspector_dock_},
        {Theme::instance().metric(QStringLiteral("layout.inspector.default"))},
        Qt::Horizontal);
    resizeDocks(
        {send_dock_},
        {Theme::instance().metric(QStringLiteral("layout.send.default"))},
        Qt::Vertical);
    resizeDocks(
        {monitor_dock_},
        {Theme::instance().metric(QStringLiteral("layout.monitor.default"))},
        Qt::Vertical);
}

void ShellWindow::build_status()
{
    status_ = new StatusBar(backend_, this);
    setStatusBar(status_);
    connect(status_, &StatusBar::level_requested, this,
            [this](bool advanced) { level_.set_advanced(advanced); });
    status_->set_advanced(level_.advanced());
    menuBar()->setFont(Theme::instance().font(text_style::body));
    // One header row: the menus on the left, the action search and the
    // Simple and Advanced switch on the right.
    menuBar()->setFixedHeight(
        Theme::instance().metric(QStringLiteral("header")));
    if (QWidget* corner = menuBar()->cornerWidget(Qt::TopRightCorner))
    {
        if (auto* corner_layout = qobject_cast<QHBoxLayout*>(corner->layout()))
            corner_layout->addWidget(status_->release_level_switch(corner));
    }
    toolbar_->setFixedHeight(
        Theme::instance().metric(QStringLiteral("buttonSmall")) + 4);
    // The panes sit 8 px in from the window edge, like the gaps between
    // them.
    const int gutter = Theme::instance().space(1);
    setContentsMargins(gutter, 0, gutter, 0);
    status_->setFixedHeight(
        Theme::instance().metric(QStringLiteral("statusBar")));
}

void ShellWindow::connect_backend()
{
    connect(&backend_, &StudioBackend::ports_changed, this,
            [this] { update_lost_banner(); });
    connect(&backend_, &StudioBackend::ports_changed, this,
            &ShellWindow::on_backend_changed);
    connect(&backend_, &StudioBackend::links_changed, this,
            &ShellWindow::on_backend_changed);
    connect(&backend_, &StudioBackend::stats_changed, this,
            [this]
            {
                VSS_PROFILE_ZONE("gui.shell.on_stats_changed");
                status_->refresh();
            });
    connect(&backend_, &StudioBackend::lines_changed, this,
            [this](vss::endpoint_id) { status_->refresh(); });
    connect(&backend_, &StudioBackend::batch_ready, this,
            [this](const event_batch& events)
            {
                VSS_PROFILE_ZONE("gui.shell.batch_support_log");
                support_log_.append(events);
            });
    connect(&backend_, &StudioBackend::batch_ready, this,
            [this](const event_batch& events)
            {
                VSS_PROFILE_ZONE("gui.shell.batch_event_log");
                if (event_log_ != nullptr)
                    event_log_->append(events);
            });
}

void ShellWindow::on_backend_changed()
{
    VSS_PROFILE_ZONE("gui.shell.on_backend_changed");
    if (canvas_ != nullptr && canvas_->in_batch())
        return;
    // A port that is added or removed has an identity the history cannot
    // restore, so the history only ever describes the ports it was made on.
    std::vector<vss::endpoint_id> ports;
    for (const auto& port : backend_.ports())
        ports.push_back(port.id);
    std::sort(ports.begin(), ports.end());
    if (ports != history_ports_)
    {
        history_ports_ = std::move(ports);
        undo_.clear();
    }
    if (!backend_.ports().empty())
        launcher_forced_ = false;
    QString signature;
    {
        VSS_PROFILE_ZONE("gui.shell.document_signature");
        signature = document_signature();
    }
    if (!loading_ && signature != last_signature_)
        set_dirty(true);
    last_signature_ = std::move(signature);
    {
        VSS_PROFILE_ZONE("gui.shell.status_refresh");
        status_->refresh();
    }
    {
        VSS_PROFILE_ZONE("gui.shell.update_content");
        update_content();
    }
    {
        VSS_PROFILE_ZONE("gui.shell.update_owner_bar");
        update_owner_bar();
    }
    if (json_editor_ != nullptr)
    {
        VSS_PROFILE_ZONE("gui.shell.json_reload");
        json_editor_->reload_if_clean();
    }
    validate_selection();
    {
        VSS_PROFILE_ZONE("gui.shell.update_dock_titles");
        update_dock_titles();
    }
    {
        VSS_PROFILE_ZONE("gui.shell.update_actions");
        update_actions();
    }
    update_resume_banner();
    {
        VSS_PROFILE_ZONE("gui.shell.update_tray");
        update_tray();
    }
    schedule_running_save();
}

void ShellWindow::on_level_changed(bool advanced)
{
    registry_.set_level(level_.level());
    if (advanced_action_ != nullptr)
    {
        const QSignalBlocker blocker(advanced_action_);
        advanced_action_->setChecked(advanced);
    }
    status_->set_advanced(advanced);
    sync_canvas_editing();
    if (ports_panel_ != nullptr)
        ports_panel_->set_advanced(advanced);
    if (inspector_panel_ != nullptr)
        inspector_panel_->set_advanced(advanced);
    if (send_panel_ != nullptr)
        send_panel_->set_advanced(advanced);
    if (monitor_panel_ != nullptr)
        monitor_panel_->set_advanced(advanced);
    if (send_dock_ != nullptr)
    {
        const int base =
            Theme::instance().metric(QStringLiteral("layout.send.default"));
        // The "Sending from" line is 24 high; Advanced adds the repeat and
        // saved-sends rows, and a line for a running repeat.
        send_dock_->setMinimumHeight(advanced ? base + 24 + 68 + 30
                                              : base + 24);
    }
    if (monitor_dock_ != nullptr)
    {
        const int base =
            Theme::instance().metric(QStringLiteral("layout.monitor.default"));
        monitor_dock_->setMinimumHeight(advanced ? base + 34 : base);
    }
    update_dock_titles();
    update_content();
    update_owner_bar();
    update_actions();
}

void ShellWindow::update_level_ui()
{
    status_->set_advanced(level_.advanced());
}

void ShellWindow::update_content()
{
    if (!level_.advanced())
    {
        central_->setCurrentWidget(simple_page_);
        for (QDockWidget* dock :
             {ports_dock_, inspector_dock_, send_dock_, monitor_dock_,
              links_dock_, log_dock_, json_dock_})
        {
            if (dock != nullptr)
                dock->hide();
        }
        sync_selection();
        status_->refresh();
        return;
    }
    // Advanced mode builds on the canvas, so an empty setup shows the empty
    // canvas with its toolbox; the launcher appears only from View, Show
    // launcher.
    if (launcher_forced_)
    {
        central_->setCurrentWidget(launcher_);
        launcher_->refresh();
        for (QDockWidget* dock :
             {ports_dock_, inspector_dock_, send_dock_, monitor_dock_,
              links_dock_, log_dock_, json_dock_})
        {
            if (dock != nullptr)
                dock->hide();
        }
    }
    else
    {
        central_->setCurrentWidget(workspace_);
        refresh_topology();
        for (QDockWidget* dock :
             {ports_dock_, inspector_dock_, send_dock_, monitor_dock_})
        {
            if (dock != nullptr)
                dock->show();
        }
        const bool links_shown =
            links_action_ != nullptr && links_action_->isChecked();
        if (links_dock_ != nullptr)
            links_dock_->setVisible(links_shown);
        sync_advanced_docks();
        if (monitor_action_ != nullptr)
        {
            const QSignalBlocker blocker(monitor_action_);
            monitor_action_->setChecked(monitor_dock_->isVisible());
        }
    }
    sync_selection();
    status_->refresh();
}

void ShellWindow::update_lost_banner()
{
    if (lost_banner_ == nullptr)
        return;
    const bool lost =
        backend_.caps().recreate_ports && !backend_.lost_ports().empty();
    lost_banner_->setVisible(lost);
    update_tray();
}

void ShellWindow::recreate_lost_ports()
{
    if (recreate_thread_ != nullptr || lost_banner_ == nullptr)
        return;

    // Making a port takes a second or two, so it runs off the GUI thread.
    if (lost_banner_->action_button() != nullptr)
        lost_banner_->action_button()->setEnabled(false);
    auto outcome = std::make_shared<vss::result<std::size_t>>(
        vss::make_error_code(vss::errc::unsupported));
    recreate_thread_ = QThread::create(
        [this, outcome] { *outcome = backend_.recreate_lost_ports(); });
    connect(recreate_thread_, &QThread::finished, this,
            [this, outcome]
            {
                recreate_thread_->deleteLater();
                recreate_thread_ = nullptr;
                if (lost_banner_->action_button() != nullptr)
                    lost_banner_->action_button()->setEnabled(true);
                backend_.refresh();
                update_lost_banner();
                const QString text =
                    *outcome ? tr("Ports recreated.")
                             : tr("Could not recreate every port: %1")
                                   .arg(error_text(outcome->error()));
                status_->showMessage(text, 6000);
                log_local(text);
            });
    recreate_thread_->start();
}

void ShellWindow::update_title()
{
    QString name = title_override_.isEmpty() ? document_name(document_path_)
                                             : title_override_;
    if (dirty_)
        name += QLatin1Char('*');
    setWindowTitle(tr("%1 - Virtual Serial Studio").arg(name));
    if (json_editor_ != nullptr)
        json_editor_->set_document_path(document_path_);
    // The saved setup's name and the unsaved mark are part of what the
    // running setup keeps.
    schedule_running_save();
}

void ShellWindow::update_actions()
{
    registry_.refresh();
    update_undo_actions();
    update_title();
}

void ShellWindow::update_undo_actions()
{
    const bool supported = backend_.caps().undo;
    const struct
    {
        const char* id = nullptr;
        QString plain;
        QString with_step;
        QString step;
    } labels[] = {{"edit.undo", tr("Undo"), tr("Undo %1"), undo_.undo_text()},
                  {"edit.redo", tr("Redo"), tr("Redo %1"), undo_.redo_text()}};
    for (const auto& label : labels)
    {
        QAction* action = registry_.action(QString::fromLatin1(label.id));
        if (action == nullptr)
            continue;
        action->setText(label.step.isEmpty() ? label.plain
                                             : label.with_step.arg(label.step));
        action->setToolTip(supported ? action->text()
                                     : tr("Not available in this build"));
        action->setStatusTip(supported
                                 ? tr("Reverses connecting or removing a link, "
                                      "changing an owner and renaming a port.")
                                 : tr("This build cannot reverse changes."));
    }
}

void ShellWindow::undo_last()
{
    const QString text = undo_.undo_text();
    if (undo_.undo())
        status_->showMessage(tr("Undid %1.").arg(text), 4000);
    else if (!text.isEmpty())
        status_->showMessage(
            tr("Could not undo %1. The setup has changed since.").arg(text),
            6000);
}

void ShellWindow::redo_last()
{
    const QString text = undo_.redo_text();
    if (undo_.redo())
        status_->showMessage(tr("Redid %1.").arg(text), 4000);
    else if (!text.isEmpty())
        status_->showMessage(
            tr("Could not redo %1. The setup has changed since.").arg(text),
            6000);
}

void ShellWindow::update_status()
{
    status_->refresh();
}

void ShellWindow::update_dock_titles()
{
    if (ports_dock_ == nullptr)
        return;
    const auto ports = backend_.ports();
    ports_dock_->setWindowTitle(
        tr("Ports %1").arg(static_cast<qulonglong>(ports.size())));
    QString inspector = tr("Inspector");
    if (selection_.port() != 0)
    {
        if (const auto port = backend_.port(selection_.port()))
            inspector = tr("Inspector %1").arg(port->name);
    }
    inspector_dock_->setWindowTitle(inspector);

    // The Ports pane header shows the count as a badge rather than running
    // it into the title.  The inspector shows the port's name in its body.
    if (ports_header_ != nullptr)
        ports_header_->set_count(static_cast<int>(ports.size()));
}

QString ShellWindow::document_signature() const
{
    QStringList parts;
    for (const auto& port : backend_.ports())
    {
        parts << QStringLiteral("P%1:%2:%3:%4")
                     .arg(static_cast<qulonglong>(port.id))
                     .arg(port.name, port.path)
                     .arg(static_cast<int>(port.kind));
    }
    for (const auto& link : backend_.links())
    {
        QStringList sources;
        QStringList targets;
        for (const auto id : link.sources)
            sources << QString::number(static_cast<qulonglong>(id));
        for (const auto id : link.targets)
            targets << QString::number(static_cast<qulonglong>(id));
        parts << QStringLiteral("L%1:%2:%3:%4:%5:%6")
                     .arg(static_cast<qulonglong>(link.id))
                     .arg(static_cast<int>(link.shape))
                     .arg(static_cast<int>(link.mode))
                     .arg(static_cast<qulonglong>(link.owner))
                     .arg(sources.join(QLatin1Char(',')),
                          targets.join(QLatin1Char(',')));
    }
    parts.sort();
    return parts.join(QLatin1Char('|'));
}

void ShellWindow::remove_selected_port()
{
    remove_port(selection_.port());
}

void ShellWindow::copy_selected_port()
{
    const vss::endpoint_id id = selection_.port();
    if (id == 0)
        return;
    const auto port = backend_.port(id);
    if (!port || port->path.isEmpty())
        return;
    QApplication::clipboard()->setText(port->path);
    status_->showMessage(tr("Copied %1.").arg(port->path), 2500);
}

void ShellWindow::focus_inspector()
{
    if (inspector_dock_ == nullptr)
        return;
    inspector_dock_->show();
    inspector_dock_->raise();
    inspector_panel_->setFocus(Qt::ShortcutFocusReason);
}

void ShellWindow::apply_compact()
{
    const bool compact = width() < 1100 || height() < 700;
    if (compact == compact_)
        return;
    compact_ = compact;

    toolbar_->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly
                                         : Qt::ToolButtonTextBesideIcon);
    if (compact)
    {
        if (!tabifiedDockWidgets(inspector_dock_).contains(send_dock_))
        {
            tabifyDockWidget(inspector_dock_, send_dock_);
            inspector_dock_->raise();
        }
    }
    else if (tabifiedDockWidgets(inspector_dock_).contains(send_dock_))
    {
        splitDockWidget(inspector_dock_, send_dock_, Qt::Vertical);
    }
    if (ports_panel_ != nullptr)
        ports_panel_->set_compact(compact_);
    if (monitor_panel_ != nullptr)
        monitor_panel_->set_compact(compact_);
    apply_dock_sizes();
}

void ShellWindow::show_launcher()
{
    if (!level_.advanced())
    {
        simple_page_->show_jobs();
        update_content();
        return;
    }
    launcher_forced_ = true;
    update_content();
}

void ShellWindow::show_workspace()
{
    launcher_forced_ = false;
    update_content();
}

void ShellWindow::open_palette()
{
    CommandPalette palette(registry_, level_, this);
    if (!search_->text().trimmed().isEmpty())
        palette.set_query(search_->text());
    palette.exec();
    search_->clear();
    status_->refresh();
}

void ShellWindow::open_connect_ports()
{
    ConnectPortsDialog dialog(backend_, selection_.port(), level_.advanced(),
                              this);
    connect(&dialog, &ConnectPortsDialog::advanced_requested, this,
            [this, &dialog]
            {
                level_.set_advanced(true);
                dialog.set_advanced(true);
            });
    if (dialog.exec() != QDialog::Accepted)
        return;
    if (dialog.created_link())
    {
        if (const auto made = backend_.link(*dialog.created_link()))
            undo_.push(connect_step(backend_, *made, tr("Connect ports")));
    }
    status_->refresh();
    if (!dialog.result_summary().isEmpty())
        ToastManager::instance().show(this, dialog.result_summary());
}

void ShellWindow::open_share_wizard()
{
    if (!backend_.caps().share)
    {
        status_->showMessage(tr("Sharing is not available in this build."),
                             4000);
        return;
    }
    ShareWizard wizard(backend_, level_.advanced(), this);
    connect(&wizard, &ShareWizard::advanced_requested, this,
            [this, &wizard]
            {
                level_.set_advanced(true);
                wizard.set_advanced(true);
            });
    connect(&wizard, &ShareWizard::open_monitor_requested, this,
            [this] { show_monitor(); });
    // A failed share is rolled back by the engine, but its transient
    // endpoint signals must not mark the setup dirty.
    loading_ = true;
    wizard.exec();
    loading_ = false;
    if (wizard.listener_count() > 0)
        set_dirty(true);
    status_->refresh();
}

void ShellWindow::open_network_endpoint()
{
    if (!backend_.caps().bridge)
    {
        status_->showMessage(tr("Networking is not available in this build."),
                             4000);
        return;
    }
    NetworkEndpointDialog dialog(backend_, level_.advanced(), selection_.port(),
                                 this);
    connect(&dialog, &NetworkEndpointDialog::advanced_requested, this,
            [this, &dialog]
            {
                level_.set_advanced(true);
                dialog.set_advanced(true);
            });
    // A failed bridge is rolled back by the engine; do not dirty the setup
    // from the transient endpoint signals.
    loading_ = true;
    const int accepted = dialog.exec();
    loading_ = false;
    if (accepted != QDialog::Accepted)
        return;
    set_dirty(true);
    status_->refresh();
    ToastManager::instance().show(
        this, tr("Bridge is running on port %1.").arg(dialog.result_port()),
        tr("Open monitor"), [this] { show_monitor(); });
}

void ShellWindow::open_templates()
{
    if (!confirm_discard())
        return;

    TemplatesDialog dialog(backend_, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto document = dialog.result_document();
    if (!document)
        return;

    loading_ = true;
    const auto applied = backend_.apply(*document, true);
    loading_ = false;
    if (!applied)
    {
        ErrorDialog error(
            tr("Could not open %1").arg(dialog.result_title()),
            tr("The setup needs a device or network endpoint that "
               "is not available on this PC. Nothing was changed. "
               "Check the device, then try again."),
            error_text(applied.error()), this);
        error.exec();
        status_->refresh();
        return;
    }

    if (dialog.result_is_template())
    {
        document_path_.clear();
        title_override_ = dialog.result_title();
        set_dirty(true);
    }
    else
    {
        document_path_ = dialog.result_path();
        title_override_.clear();
        set_dirty(false);
        const auto ports = backend_.ports();
        const auto links = backend_.links();
        add_recent(document_path_,
                   tr("%1 ports, %2 links")
                       .arg(static_cast<qulonglong>(ports.size()))
                       .arg(static_cast<qulonglong>(links.size())));
    }
    show_workspace();
    update_title();
    status_->refresh();
}

void ShellWindow::show_monitor()
{
    if (monitor_dock_ == nullptr)
        return;
    monitor_dock_->show();
    monitor_dock_->raise();
    if (monitor_action_ != nullptr)
    {
        const QSignalBlocker blocker(monitor_action_);
        monitor_action_->setChecked(true);
    }
}

void ShellWindow::show_send()
{
    if (send_dock_ == nullptr)
        return;
    send_dock_->show();
    send_dock_->raise();
}

port_menu_state ShellWindow::port_menu_state_for(vss::endpoint_id id) const
{
    VSS_PROFILE_ZONE("gui.shell.port_menu_state");
    port_menu_state state;
    state.advanced = level_.advanced();
    state.repeating = send_panel_ != nullptr && send_panel_->repeating() &&
                      send_panel_->repeat_port() == id;
    if (behaviours_ != nullptr && behaviours_->is_fixture(id))
    {
        // The fixture behaviours are an Advanced control, like the fixture
        // itself.
        state.fixture = state.advanced;
        state.generating = behaviours_->generating(id);
        state.echoing = behaviours_->echoing(id);
        state.echo_available = behaviours_->echo_available();
    }
    return state;
}

void ShellWindow::run_port_action(vss::endpoint_id id, port_menu_action action)
{
    selection_.select_port(id);
    switch (action)
    {
    case port_menu_action::send_bytes:
        show_send();
        send_panel_->send_from(id);
        break;
    case port_menu_action::toggle_repeat:
        show_send();
        send_panel_->toggle_repeat(id);
        break;
    case port_menu_action::open_monitor:
        monitor_panel_->set_selected_port(id);
        show_monitor();
        break;
    case port_menu_action::toggle_generator:
        behaviours_->toggle_generator(id);
        status_->showMessage(behaviours_->generating(id)
                                 ? tr("Generator started.")
                                 : tr("Generator stopped."),
                             3000);
        break;
    case port_menu_action::toggle_echo:
        behaviours_->toggle_echo(id);
        break;
    }
}

void ShellWindow::run_job(const QString& job)
{
    if (job == QLatin1String("add"))
        show_add_port();
    else if (job == QLatin1String("pair"))
        show_add_port(true);
    else if (job == QLatin1String("attach"))
        show_attach_device();
    else if (job == QLatin1String("share"))
        open_share_wizard();
    else if (job == QLatin1String("watch"))
        show_watch();
    else if (job == QLatin1String("connect"))
    {
        open_connect_ports();
        return;
    }
    else if (job == QLatin1String("network"))
    {
        open_network_endpoint();
        return;
    }
    else if (job == QLatin1String("templates"))
    {
        open_templates();
        return;
    }
    else if (job == QLatin1String("mock"))
    {
        registry_.action(QStringLiteral("ports.mock"))->trigger();
        return;
    }
    else
        placeholder(job);
}

void ShellWindow::show_add_port(bool pair_selected)
{
    AddPortDialog dialog(backend_, level_.advanced(),
                         pair_selected ? AddPortDialog::mode::linked_pair
                                       : AddPortDialog::mode::virtual_port,
                         vss::gui::host_platform(), this);
    connect(&dialog, &AddPortDialog::advanced_requested, this,
            [this, &dialog]
            {
                level_.set_advanced(true);
                dialog.set_advanced(true);
            });
    connect(&dialog, &AddPortDialog::setup_check_requested, this,
            [this] { open_setup_check(); });
    connect(&dialog, &AddPortDialog::driver_setup_requested, this,
            [this] { open_driver_setup(); });
    if (dialog.exec() != QDialog::Accepted)
        return;

    if (dialog.wants_attach())
    {
        show_attach_device();
        return;
    }
    if (dialog.created_ports().isEmpty())
        return;
    if (!dialog.pair_result())
    {
        show_port_ready(dialog.created_ports().first(),
                        dialog.created_kind_token());
        return;
    }

    selection_.select_port(dialog.pair_result()->first);
    update_actions();
    status_->showMessage(
        tr("Linked pair created. Program A opens %1; Program B opens %2.")
            .arg(dialog.pair_result()->first_path,
                 dialog.pair_result()->second_path),
        8000);
}

void ShellWindow::show_pair()
{
    PairDialog dialog(backend_, level_.advanced(), vss::gui::host_platform(),
                      this);
    connect(&dialog, &PairDialog::advanced_requested, this,
            [this, &dialog]
            {
                level_.set_advanced(true);
                dialog.set_advanced(true);
            });
    connect(&dialog, &PairDialog::setup_check_requested, this,
            [this] { open_setup_check(); });
    connect(&dialog, &PairDialog::driver_setup_requested, this,
            [this] { open_driver_setup(); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto outcome = dialog.outcome();
    if (!outcome)
        return;
    selection_.select_port(outcome->first);
    update_actions();
    status_->showMessage(
        tr("Linked pair created. Program A opens %1; Program B opens %2.")
            .arg(outcome->first_path, outcome->second_path),
        8000);
}

void ShellWindow::show_attach_device()
{
    AttachDialog dialog(backend_, level_.advanced(), vss::gui::host_platform(),
                        this);
    if (dialog.exec() != QDialog::Accepted || dialog.logical_port() == 0)
        return;
    show_port_ready(dialog.logical_port(), QStringLiteral("auto"));
}

void ShellWindow::show_watch()
{
    WatchDialog dialog(backend_, selection_.port(), vss::gui::host_platform(),
                       this);
    const int result = dialog.exec();
    // The empty list offers "Create a port": open that flow now the Watch
    // window has closed.
    if (dialog.create_port_chosen())
    {
        show_add_port();
        return;
    }
    if (result != QDialog::Accepted || dialog.link_id() == 0)
        return;
    const auto source = backend_.port(dialog.source_id());
    const auto watcher = backend_.port(dialog.watcher_id());
    if (dialog.open_monitor_requested())
        focus_monitor(dialog.source_id());
    else
    {
        selection_.select_port(dialog.source_id());
        update_content();
        if (monitor_dock_ != nullptr)
            monitor_dock_->hide();
        if (monitor_action_ != nullptr)
        {
            const QSignalBlocker blocker(monitor_action_);
            monitor_action_->setChecked(false);
        }
    }
    status_->showMessage(tr("Watching %1. The watcher is %2.")
                             .arg(source ? source->path : QString(),
                                  watcher ? watcher->name : QString()),
                         6000);
}

void ShellWindow::show_port_ready(vss::endpoint_id port,
                                  const QString& virtual_kind)
{
    if (!backend_.port(port))
        return;
    bool create_another = false;
    bool connect_requested = false;
    {
        PortReadyDialog dialog(backend_, port, virtual_kind, level_.advanced(),
                               vss::gui::host_platform(), this);
        connect(&dialog, &PortReadyDialog::create_another_requested, this,
                [&create_another] { create_another = true; });
        connect(&dialog, &PortReadyDialog::connect_requested, this,
                [&connect_requested](vss::endpoint_id)
                { connect_requested = true; });
        connect(&dialog, &PortReadyDialog::setup_check_requested, this,
                [this] { open_setup_check(); });
        dialog.exec();
    }
    if (create_another)
        show_add_port();
    else if (connect_requested)
        open_connect_ports();
}

void ShellWindow::focus_monitor(vss::endpoint_id port)
{
    if (monitor_action_ != nullptr)
    {
        const QSignalBlocker blocker(monitor_action_);
        monitor_action_->setChecked(true);
    }
    if (monitor_dock_ != nullptr)
    {
        monitor_dock_->show();
        monitor_dock_->raise();
        monitor_dock_->setFocus(Qt::OtherFocusReason);
    }
    selection_.select_port(port);
    update_content();
}

void ShellWindow::placeholder(const QString& what)
{
    status_->showMessage(tr("%1 is part of a later work package.").arg(what),
                         5000);
}

void ShellWindow::later_package(const QString& what, const QString& package)
{
    status_->showMessage(tr("%1 arrives with %2.").arg(what, package), 6000);
}

void ShellWindow::refresh_topology()
{
    VSS_PROFILE_ZONE("gui.shell.refresh_topology");
    if (topology_ != nullptr)
        topology_->set_topology(backend_.ports(), backend_.links());
    if (links_ != nullptr)
    {
        VSS_PROFILE_ZONE("gui.shell.links_list");
        links_->set_links(backend_.links(), backend_.ports());
    }
}

void ShellWindow::sync_selection()
{
    if (topology_ != nullptr)
        topology_->set_selection(selection_.port(), selection_.link());
    if (links_ != nullptr)
        links_->set_selected(selection_.link());
}

void ShellWindow::validate_selection()
{
    bool port_exists = false;
    for (const auto& port : backend_.ports())
        port_exists = port_exists || port.id == selection_.port();
    if (!port_exists && selection_.port() != 0)
        selection_.select_port(0);

    bool link_exists = false;
    for (const auto& link : backend_.links())
        link_exists = link_exists || link.id == selection_.link();
    if (!link_exists && selection_.link() != 0)
        selection_.select_link(0);
}

void ShellWindow::copy_port_name(vss::endpoint_id id)
{
    for (const auto& port : backend_.ports())
    {
        if (port.id != id)
            continue;
        QApplication::clipboard()->setText(port.name);
        status_->showMessage(topology_text::copied(port.name), 3000);
        return;
    }
}

void ShellWindow::connect_from_port(vss::endpoint_id id)
{
    selection_.select_link(0);
    selection_.select_port(id);
    if (QAction* action = registry_.action(QStringLiteral("topology.connect")))
        action->trigger();
}

void ShellWindow::remove_selection()
{
    if (selection_.link() != 0)
    {
        remove_link(selection_.link());
        return;
    }
    if (selection_.port() != 0)
        remove_port(selection_.port());
}

void ShellWindow::remove_selected_link()
{
    remove_link(selection_.link());
}

void ShellWindow::remove_link(vss::link_id id)
{
    if (id == 0)
        return;
    const auto link = backend_.link(id);
    if (!link)
        return;

    QHash<vss::endpoint_id, QString> names;
    for (const auto& port : backend_.ports())
        names.insert(port.id, port.name);
    const QString description = topology_text::link_name(*link, names);

    if (!ConfirmDialog::ask(
            tr("Remove link"),
            tr("Remove this link? %1. Nothing is saved until you "
               "save the setup.")
                .arg(description),
            tr("Remove link"), tr("Cancel"), true, this))
    {
        return;
    }

    const auto removed = backend_.disconnect(id);
    if (!removed)
    {
        ErrorDialog dialog(tr("Could not remove the link"),
                           error_text(removed.error()), {}, this);
        dialog.exec();
        return;
    }

    undo_.push(remove_link_step(backend_, *link, tr("Remove link")));
    selection_.select_link(0);
    backend_.refresh();
    status_->showMessage(tr("Removed the link."), 3000);
}

namespace
{

QString port_count_text(int count)
{
    return count == 1 ? ShellWindow::tr("1 port")
                      : ShellWindow::tr("%1 ports").arg(count);
}

QString link_count_text(int count)
{
    return count == 1 ? ShellWindow::tr("1 link")
                      : ShellWindow::tr("%1 links").arg(count);
}

} // namespace

void ShellWindow::remove_port(vss::endpoint_id id)
{
    if (id == 0)
        return;
    const auto port = backend_.port(id);
    if (!port)
        return;

    // A sentence in front of the question when a program has the port open.
    QString warning = removal_warning(*port);
    if (!warning.isEmpty())
        warning += QLatin1Char(' ');

    const auto ports = backend_.ports();
    const auto links = backend_.links();
    const setup_group group = find_setup(ports, links, id);
    // One program of a share: the port is taken off the link first, so the
    // link and the other programs carry on while this one is removed.
    const auto share = find_share(ports, links, id);
    const bool program_of_share =
        share && share->programs.contains(id) && share->programs.size() > 1;
    if (group.has_company())
    {
        // The port is one part of something built together.  Offer the whole
        // setup next to the single port so nobody has to hunt for it.
        const int others = static_cast<int>(group.ports.size()) - 1;
        const QVector<confirm_choice> choices{
            {tr("Cancel"), QStringLiteral("quiet"), true},
            {tr("Remove only %1").arg(port->name), {}, false},
            {tr("Remove the whole setup"), QStringLiteral("danger"), false}};
        const QString body =
            program_of_share
                ? tr("%1 is one of %2 programs sharing a device, in a setup "
                     "with %3: %4. Remove only %1 and the others keep "
                     "sharing, or close the whole setup. Nothing is saved "
                     "until you save the setup.")
                      .arg(port->name)
                      .arg(share->programs.size())
                      .arg(port_count_text(others),
                           setup_port_names(group, ports))
                : tr("%1 is part of a setup with %2: %3. Remove only %1 and "
                     "its links, or close the whole setup. Nothing is saved "
                     "until you save the setup.")
                      .arg(port->name, port_count_text(others),
                           setup_port_names(group, ports));
        ConfirmDialog dialog(tr("Remove %1?").arg(port->name), warning + body,
                             choices, 0, this);
        dialog.exec();
        if (dialog.chosen() == 2)
        {
            // Choosing it here is the confirmation.
            remove_setup(group, false);
            return;
        }
        if (dialog.chosen() != 1)
            return;
    }
    else if (!ConfirmDialog::ask(
                 tr("Remove %1?").arg(port->name),
                 warning + tr("Remove %1 and its links? Nothing is saved "
                              "until you save the setup.")
                               .arg(port->name),
                 tr("Remove port"), tr("Cancel"), true, this))
    {
        return;
    }

    vss::result<void> removed = vss::result<void>{};
    if (program_of_share)
    {
        const auto kept = vss::gui::remove_program_port(backend_, *share, id);
        if (!kept)
            removed = kept.error();
    }
    else
    {
        removed = backend_.remove_port(id);
    }
    if (!removed)
    {
        ErrorDialog dialog(tr("Could not remove %1").arg(port->name),
                           error_text(removed.error()), {}, this);
        dialog.exec();
        backend_.refresh();
        return;
    }

    selection_.clear();
    backend_.refresh();
    status_->showMessage(tr("Removed %1.").arg(port->name), 3000);
}

void ShellWindow::remove_selected_setup()
{
    const auto ports = backend_.ports();
    const auto links = backend_.links();
    const setup_group group =
        selection_.port() != 0
            ? find_setup(ports, links, selection_.port())
            : find_setup_of_link(ports, links, selection_.link());
    remove_setup(group);
}

void ShellWindow::remove_setup(const setup_group& group, bool confirm)
{
    if (group.empty())
        return;
    const auto ports = backend_.ports();
    const int port_count = static_cast<int>(group.ports.size());
    const int link_count = static_cast<int>(group.links.size());
    const QString what = link_count == 0
                             ? port_count_text(port_count)
                             : tr("%1 and %2")
                                   .arg(port_count_text(port_count),
                                        link_count_text(link_count));
    QString warnings;
    for (const vss::endpoint_id id : group.ports)
    {
        for (const port_view& port : ports)
        {
            const QString warning =
                port.id == id ? removal_warning(port) : QString();
            if (!warning.isEmpty() && !warnings.contains(warning))
                warnings += warning + QLatin1Char(' ');
        }
    }
    if (confirm &&
        !ConfirmDialog::ask(
            tr("Remove this setup?"),
            warnings +
                tr("This closes %1: %2. A program that has one of these ports "
                   "open loses it. Nothing is saved until you save the setup.")
                    .arg(what, setup_port_names(group, ports)),
            tr("Remove setup"), tr("Cancel"), true, this))
    {
        return;
    }

    int removed_ports = 0;
    for (const vss::endpoint_id id : group.ports)
    {
        const auto removed = backend_.remove_port(id);
        if (removed || removed.error() == vss::errc::not_found)
        {
            ++removed_ports;
            continue;
        }
        QString name = QString::number(static_cast<qulonglong>(id));
        if (const auto port = backend_.port(id))
            name = port->name;
        ErrorDialog dialog(
            tr("Could not remove %1").arg(name),
            tr("Part of the setup is still open. Close what is using it, "
               "then choose Remove setup again."),
            error_text(removed.error()), this);
        dialog.exec();
        break;
    }

    selection_.clear();
    backend_.refresh();
    if (removed_ports == port_count)
        status_->showMessage(tr("Removed the setup (%1).").arg(what), 4000);
    else
        status_->showMessage(tr("Removed %1 of %2.")
                                 .arg(port_count_text(removed_ports),
                                      port_count_text(port_count)),
                             4000);
}

void ShellWindow::remove_all_setups()
{
    const auto ports = backend_.ports();
    if (ports.empty())
        return;
    const auto links = backend_.links();
    int setups = 0;
    QSet<vss::endpoint_id> seen;
    for (const auto& port : ports)
    {
        if (seen.contains(port.id))
            continue;
        ++setups;
        for (const auto id : find_setup(ports, links, port.id).ports)
            seen.insert(id);
    }
    if (!ConfirmDialog::ask(
            tr("Remove all setups?"),
            tr("This closes %n setup(s) and every port in them. Programs "
               "using those ports lose them. Nothing is saved until you save.",
               nullptr, setups),
            tr("Remove all"), tr("Cancel"), true, this))
        return;
    int failed = 0;
    for (const auto& port : ports)
    {
        const auto removed = backend_.remove_port(port.id);
        if (!removed && removed.error() != vss::errc::not_found)
            ++failed;
    }
    selection_.clear();
    backend_.refresh();
    status_->showMessage(
        failed == 0 ? tr("Removed all setups.")
                    : tr("%n port(s) could not be removed.", nullptr, failed),
        5000);
}

vss::link_id ShellWindow::share_link_of_selection() const
{
    const auto ports = backend_.ports();
    const auto links = backend_.links();
    if (selection_.port() != 0)
    {
        const auto share = find_share(ports, links, selection_.port());
        return share ? share->link : 0;
    }
    if (selection_.link() != 0)
    {
        const auto share = find_share_of_link(ports, links, selection_.link());
        return share ? share->link : 0;
    }
    return 0;
}

void ShellWindow::add_program_port()
{
    const auto ports = backend_.ports();
    const auto links = backend_.links();
    const auto share =
        selection_.port() != 0
            ? find_share(ports, links, selection_.port())
            : find_share_of_link(ports, links, selection_.link());
    if (!share)
        return;
    const auto device = backend_.port(share->device);
    const QString device_name = device ? device->name : tr("the device");

    const auto added = vss::gui::add_program_port(
        backend_, *share, next_program_name(ports), true);
    if (!added)
    {
        ErrorDialog dialog(
            tr("Could not add a program port to %1").arg(device_name),
            tr("The setup is as it was. Check the device, then try again."),
            error_text(added.error()), this);
        dialog.exec();
        return;
    }
    backend_.refresh();
    selection_.select_port(added->port);
    const auto port = backend_.port(added->port);
    status_->showMessage(
        tr("Added %1 to %2.")
            .arg(port ? port_display_name(*port) : tr("a port"), device_name),
        4000);
}

void ShellWindow::show_rename(vss::endpoint_id id)
{
    if (id == 0 || !backend_.caps().rename_port)
        return;
    const auto port = backend_.port(id);
    if (!port)
        return;

    RenameDialog dialog(backend_, id, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    undo_.push(rename_step(backend_, id, port->name, dialog.new_name(),
                           tr("Rename port")));
    selection_.select_port(id);
    status_->showMessage(
        tr("Renamed %1 to %2.").arg(port->name, dialog.new_name()), 3000);
}

void ShellWindow::show_com_number(vss::endpoint_id id)
{
    if (id == 0 || !backend_.caps().com_numbers)
        return;
    const auto port = backend_.port(id);
    if (!port || !port->driver_port)
        return;

    ComNumberDialog dialog(backend_, id, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    // The number is part of the saved setup.
    last_signature_ = document_signature();
    set_dirty(true);
    selection_.select_port(id);
    status_->showMessage(tr("%1 is now %2.").arg(port->name, dialog.com_name()),
                         3000);
}

void ShellWindow::show_release_port(vss::endpoint_id id)
{
    if (id == 0 || !backend_.caps().release_port)
        return;
    const auto port = backend_.port(id);
    if (!port || !port->driver_port)
        return;

    ReleasePortDialog dialog(backend_, id, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.released())
        return;
    selection_.select_port(id);
    status_->showMessage(tr("Released %1.").arg(port->name), 3000);
}

void ShellWindow::show_links_dock(bool shown)
{
    if (links_dock_ == nullptr)
        return;
    if (shown && backend_.ports().empty())
        return;
    if (shown)
    {
        links_dock_->show();
        links_dock_->raise();
    }
    else
    {
        links_dock_->hide();
    }
}

void ShellWindow::open_add_fixture()
{
    AddFixtureDialog dialog(backend_, selection_.port(), this);
    connect(&dialog, &AddFixtureDialog::message, this,
            [this](const QString& text)
            {
                status_->showMessage(text, 6000);
                log_local(text);
            });
    dialog.exec();
    update_owner_bar();
    if (json_editor_ != nullptr)
        json_editor_->reload_if_clean();
}

void ShellWindow::open_wiring()
{
    vss::endpoint_id target = selection_.port();
    if (target == 0)
    {
        for (const auto& port : backend_.ports())
        {
            if (port.kind == vss::endpoint_kind::mock)
            {
                target = port.id;
                break;
            }
        }
    }
    if (target == 0)
    {
        const auto ports = backend_.ports();
        if (!ports.empty())
            target = ports.front().id;
    }
    if (target == 0)
    {
        status_->showMessage(tr("Add a port before opening the line matrix."),
                             6000);
        return;
    }
    WiringDialog dialog(backend_, target, this);
    dialog.exec();
    update_owner_bar();
}

void ShellWindow::open_benchmark()
{
    BenchmarkDialog dialog(backend_, this);
    connect(&dialog, &BenchmarkDialog::message, this,
            [this](const QString& text) { status_->showMessage(text, 6000); });
    dialog.exec();
}

void ShellWindow::open_export_command()
{
    ExportCommandDialog dialog(backend_, document_path_, this);
    connect(&dialog, &ExportCommandDialog::message, this,
            [this](const QString& text)
            {
                status_->showMessage(text, 6000);
                log_local(text);
            });
    dialog.exec();
}

void ShellWindow::show_owner_controls()
{
    update_owner_bar();
    if (owner_bar_ == nullptr || !owner_bar_->has_link())
    {
        status_->showMessage(
            tr("Select the target of a join or the device of a split with "
               "the owner policy to control who writes."),
            6000);
        return;
    }
    show_workspace();
    if (owner_bar_->give_button()->isEnabled())
    {
        owner_bar_->give_button()->setFocus(Qt::OtherFocusReason);
    }
    else if (owner_bar_->release_button()->isEnabled())
    {
        owner_bar_->release_button()->setFocus(Qt::OtherFocusReason);
    }
}

void ShellWindow::update_owner_bar()
{
    if (owner_bar_ == nullptr)
        return;
    if (!level_.advanced())
    {
        owner_bar_->clear();
        return;
    }
    std::optional<link_view> candidate;
    const auto consider = [&candidate](const link_view& link)
    {
        if (!candidate && OwnerBar::supports_owner(link))
            candidate = link;
    };
    if (selection_.link() != 0)
    {
        if (const auto link = backend_.link(selection_.link()))
            consider(*link);
    }
    if (!candidate && selection_.port() != 0)
    {
        for (const auto& link : backend_.links())
        {
            if (OwnerBar::meets_at(link, selection_.port()))
                consider(link);
        }
    }
    if (!candidate)
    {
        for (const auto& link : backend_.links())
            consider(link);
    }
    owner_bar_->set_link(candidate);
}

void ShellWindow::sync_advanced_docks()
{
    if (log_dock_ != nullptr)
    {
        log_dock_->setVisible(level_.advanced() && log_action_ != nullptr &&
                              log_action_->isChecked());
    }
    if (json_dock_ != nullptr)
    {
        json_dock_->setVisible(level_.advanced() && json_action_ != nullptr &&
                               json_action_->isChecked());
    }
}

void ShellWindow::log_local(const QString& text)
{
    if (event_log_ != nullptr)
        event_log_->add_local(text);
}

void ShellWindow::open_setup_check()
{
    SetupCheckDialog dialog(backend_, this);
    connect(&dialog, &SetupCheckDialog::restart_as_administrator_requested,
            this, &ShellWindow::restart_elevated);
    connect(&dialog, &SetupCheckDialog::continue_in_test_mode_requested, this,
            [this]
            {
                ToastManager::instance().show(
                    this, tr("Test mode. New ports only work "
                             "inside Virtual Serial Studio."));
            });
    dialog.exec();
    status_->refresh();
    launcher_->refresh();
}

void ShellWindow::open_driver_setup()
{
    DriverSetupDialog dialog(backend_, backend_.check_driver().problem, this);
    dialog.exec();
    // The banners keep an answer for a few seconds; this one is stale now.
    status_->invalidate_setup();
    launcher_->invalidate_setup();
    simple_page_->jobs()->invalidate_setup();
    status_->refresh();
    launcher_->refresh();
    simple_page_->jobs()->refresh();
    if (dialog.is_ready())
    {
        ToastManager::instance().show(
            this, tr("Virtual Serial Studio is set up. New ports can be "
                     "opened by other programs."));
    }
}

void ShellWindow::restart_elevated()
{
    if (!confirm_discard())
        return;
    QString error;
    if (relaunch_as_administrator(&error))
    {
        restarting_ = true;
        close();
        return;
    }
    if (!error.isEmpty())
    {
        ErrorDialog dialog(tr("Could not restart as administrator"), error, {},
                           this);
        dialog.exec();
    }
}

void ShellWindow::open_settings()
{
    SettingsDialog dialog(backend_, load_ui_settings(), this);
    connect(&dialog, &SettingsDialog::advanced_changed, this,
            [this](bool advanced) { level_.set_advanced(advanced); });
    connect(&dialog, &SettingsDialog::settings_changed, this,
            [this](const ui_settings& changed)
            { topology_->set_link_style(changed.links); });
    dialog.exec();
    update_actions();
    status_->refresh();
    launcher_->refresh();
}

void ShellWindow::open_troubleshoot()
{
    vss::endpoint_id port = selection_.port();
    if (port == 0)
    {
        const auto ports = backend_.ports();
        if (!ports.empty())
            port = ports.front().id;
    }
    TroubleshootDialog dialog(backend_, port, this);
    connect(&dialog, &TroubleshootDialog::bundle_requested, this,
            &ShellWindow::open_bundle);
    dialog.exec();
}

void ShellWindow::open_shortcuts()
{
    ShortcutsDialog dialog(shortcut_entries(registry_), this);
    dialog.exec();
}

void ShellWindow::open_about()
{
    AboutDialog dialog(this);
    connect(&dialog, &AboutDialog::bundle_requested, this,
            &ShellWindow::open_bundle);
    dialog.exec();
}

void ShellWindow::open_help_contents()
{
    HelpContentsDialog dialog(this);
    dialog.exec();
    switch (dialog.requested())
    {
    case help_topic::none:
        break;
    case help_topic::setup_check:
        open_setup_check();
        break;
    case help_topic::troubleshoot:
        open_troubleshoot();
        break;
    case help_topic::shortcuts:
        open_shortcuts();
        break;
    case help_topic::about:
        open_about();
        break;
    }
}

void ShellWindow::open_bundle()
{
    DiagnosticBundleDialog dialog(backend_, support_log_, this);
    dialog.exec();
}

void ShellWindow::copy_environment()
{
    QGuiApplication::clipboard()->setText(environment_report());
    ToastManager::instance().show(this, tr("Environment copied."));
}

void ShellWindow::new_document()
{
    if (!confirm_discard())
        return;
    if (const QString warning =
            new_setup_warning(ports_in_use(backend_.ports()));
        !warning.isEmpty() &&
        !ConfirmDialog::ask(tr("Start a new setup?"), warning, tr("Start new"),
                            tr("Cancel"), true, this))
    {
        return;
    }
    loading_ = true;
    const auto ports = backend_.ports();
    for (const auto& port : ports)
        static_cast<void>(backend_.remove_port(port.id));
    backend_.refresh();
    loading_ = false;
    document_path_.clear();
    last_signature_ = document_signature();
    title_override_.clear();
    set_dirty(false);
    update_title();
    selection_.clear();
    log_local(tr("Started a new setup."));
    show_launcher();
    status_->refresh();
}

void ShellWindow::open_document()
{
    if (!confirm_discard())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open setup"), QString(),
        tr("Virtual Serial Studio setups (*.json);;All files (*)"));
    if (path.isEmpty())
        return;
    open_path(path);
}

void ShellWindow::open_path(const QString& path)
{
    if (!confirm_discard())
        return;
    const auto document = vss::config::load_file(path.toStdString());
    if (!document)
    {
        ErrorDialog dialog(
            tr("Could not open %1").arg(QFileInfo(path).fileName()),
            error_text(document.error()), {}, this);
        dialog.exec();
        return;
    }

    loading_ = true;
    const auto applied = backend_.apply(*document, true);
    loading_ = false;
    if (!applied)
    {
        ErrorDialog dialog(
            tr("Could not open %1").arg(QFileInfo(path).fileName()),
            error_text(applied.error()), {}, this);
        dialog.exec();
        return;
    }
    document_path_ = path;
    last_signature_ = document_signature();
    title_override_.clear();
    set_dirty(false);
    update_title();
    const auto ports = backend_.ports();
    const auto links = backend_.links();
    add_recent(path, tr("%1 ports, %2 links")
                         .arg(static_cast<qulonglong>(ports.size()))
                         .arg(static_cast<qulonglong>(links.size())));
    log_local(tr("Opened %1.").arg(QFileInfo(path).fileName()));
    show_workspace();
    status_->refresh();
}

void ShellWindow::save_document()
{
    if (document_path_.isEmpty())
    {
        save_document_as();
        return;
    }
    const auto saved = vss::config::save_file(document_path_.toStdString(),
                                              backend_.snapshot());
    if (!saved)
    {
        ErrorDialog dialog(tr("Could not save this setup"),
                           error_text(saved.error()), {}, this);
        dialog.exec();
        return;
    }
    set_dirty(false);
    update_title();
    const auto ports = backend_.ports();
    const auto links = backend_.links();
    add_recent(document_path_, tr("%1 ports, %2 links")
                                   .arg(static_cast<qulonglong>(ports.size()))
                                   .arg(static_cast<qulonglong>(links.size())));
    const QString saved_name = QFileInfo(document_path_).fileName();
    status_->showMessage(tr("Saved %1.").arg(saved_name), 3000);
    log_local(tr("Saved %1.").arg(saved_name));
}

void ShellWindow::save_document_as()
{
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save setup"),
        document_path_.isEmpty() ? QStringLiteral("lab.json") : document_path_,
        tr("Virtual Serial Studio setups (*.json)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");
    document_path_ = std::move(path);
    title_override_.clear();
    save_document();
}

bool ShellWindow::confirm_discard()
{
    if (!dirty_)
        return true;
    if (!load_ui_settings().ask_on_discard)
        return true;

    ConfirmDialog dialog(
        tr("Save changes to %1?").arg(document_name(document_path_)),
        tr("Your ports and links have changed since the last save. "
           "You can turn this prompt off in Settings."),
        QVector<confirm_choice>{
            {tr("Don't save"), QStringLiteral("quiet"), false},
            {tr("Cancel"), QString(), true},
            {tr("Save"), QStringLiteral("primary"), false}},
        2, this);
    dialog.exec();
    switch (dialog.chosen())
    {
    case 0:
        return true; // Don't save: carry on and discard.
    case 2:
        save_document();
        return !dirty_;
    default:
        return false;
    }
}

void ShellWindow::add_recent(const QString& path, const QString& summary)
{
    record_recent(path, summary);
    launcher_->refresh();
}

void ShellWindow::set_dirty(bool dirty)
{
    if (dirty_ == dirty)
        return;
    dirty_ = dirty;
    update_title();
}

void ShellWindow::save_layout()
{
    QSettings settings;
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("ui/windowState"), saveState());
}

void ShellWindow::restore_layout()
{
    QSettings settings;
    const QByteArray geometry =
        settings.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
    const QByteArray state =
        settings.value(QStringLiteral("ui/windowState")).toByteArray();
    if (!state.isEmpty())
    {
        restored_state_ = restoreState(state);
    }
}

void ShellWindow::reset_layout()
{
    resizeDocks(
        {ports_dock_},
        {Theme::instance().metric(QStringLiteral("layout.ports.default"))},
        Qt::Horizontal);
    resizeDocks(
        {inspector_dock_},
        {Theme::instance().metric(QStringLiteral("layout.inspector.default"))},
        Qt::Horizontal);
    apply_dock_sizes();
    show_workspace();
    status_->showMessage(tr("Layout reset."), 2500);
}

void ShellWindow::apply_dock_sizes()
{
    const auto metric = [](const char* name)
    { return Theme::instance().metric(QString::fromLatin1(name)); };
    const int ports =
        metric(compact_ ? "layout.ports.min" : "layout.ports.default");
    const int inspector =
        metric(compact_ ? "layout.inspector.min" : "layout.inspector.default");
    const int monitor = compact_ ? 156 : metric("layout.monitor.default");

    // QMainWindow honours the requested split only when the dock cannot be
    // smaller at that moment. Hold the floor for the first layout pass, then
    // relax it to the documented minimum so the user can still resize.
    ports_dock_->setMinimumWidth(ports);
    inspector_dock_->setMinimumWidth(inspector);
    monitor_dock_->setMinimumHeight(monitor);
    resizeDocks({ports_dock_, inspector_dock_}, {ports, inspector},
                Qt::Horizontal);
    resizeDocks({send_dock_}, {metric("layout.send.default")}, Qt::Vertical);
    resizeDocks({monitor_dock_}, {monitor}, Qt::Vertical);
}

void ShellWindow::apply_theme_setting()
{
    const QString theme =
        QSettings()
            .value(QStringLiteral("ui/theme"), QStringLiteral("system"))
            .toString();
    if (theme == QLatin1String("light"))
        Theme::instance().set_mode(theme_mode::light);
    else if (theme == QLatin1String("dark"))
        Theme::instance().set_mode(theme_mode::dark);
    else
        Theme::instance().set_mode(theme_mode::system);
    for (const QString& name :
         {QStringLiteral("system"), QStringLiteral("light"),
          QStringLiteral("dark")})
    {
        QAction* action =
            registry_.action(QStringLiteral("view.theme.") + name);
        if (action != nullptr)
        {
            const QSignalBlocker blocker(action);
            action->setChecked(theme == name);
        }
    }
}

void ShellWindow::apply_density_setting()
{
    const QString density =
        QSettings()
            .value(QStringLiteral("ui/density"), QStringLiteral("comfortable"))
            .toString();
    Theme::instance().set_density(density == QLatin1String("compact")
                                      ? density_mode::compact
                                      : density_mode::comfortable);
    for (const QString& name :
         {QStringLiteral("comfortable"), QStringLiteral("compact")})
    {
        QAction* action =
            registry_.action(QStringLiteral("view.density.") + name);
        if (action != nullptr)
        {
            const QSignalBlocker blocker(action);
            action->setChecked(density == name);
        }
    }
}

void ShellWindow::apply_menu_palette()
{
    if (menuBar() == nullptr)
        return;
    QPalette palette = Theme::instance().palette();
    const QColor ink = Theme::instance().colour(colour_role::ink);
    palette.setColor(QPalette::WindowText, ink);
    palette.setColor(QPalette::ButtonText, ink);
    palette.setColor(QPalette::Text, ink);
    menuBar()->setPalette(palette);
}

void ShellWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    apply_compact();
}

void ShellWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!layout_sized_)
    {
        if (!restored_state_)
        {
            // QMainWindow lays the docks out after the first show; a queued
            // resize is what makes the default 360/340/196 sizes stick.
            QTimer::singleShot(0, this,
                               [this]
                               {
                                   if (!restored_state_)
                                       apply_dock_sizes();
                               });
        }
        layout_sized_ = true;
    }
    apply_compact();
}

bool ShellWindow::confirm_exit()
{
    const auto ports = backend_.ports();
    const QString warning = exit_warning(ports_in_use(ports));
    if (background_ && !ports.empty())
    {
        // With background running on, quitting is the one way to stop the
        // ports, so it is always confirmed while any run.
        const QString body =
            !warning.isEmpty()
                ? warning
                : tr("This stops %1 ports and %2 links. Studio offers to "
                     "start them again the next time it opens.")
                      .arg(static_cast<qulonglong>(ports.size()))
                      .arg(static_cast<qulonglong>(backend_.links().size()));
        return ConfirmDialog::ask(tr("Stop the ports and quit?"), body,
                                  tr("Stop the ports and quit"),
                                  tr("Keep running"), true, this);
    }
    if (warning.isEmpty())
        return true;
    return ConfirmDialog::ask(tr("Close Studio?"), warning, tr("Close Studio"),
                              tr("Keep Studio open"), true, this);
}

void ShellWindow::closeEvent(QCloseEvent* event)
{
    if (restarting_)
    {
        // The Studio that replaces this one (as administrator) starts the
        // same ports again, so this is not a Quit.
        save_layout();
        if (background_)
        {
            save_running_now(false);
            QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        }
        event->accept();
        return;
    }

    if (background_ && !quitting_ && !backend_.ports().empty() &&
        load_ui_settings().keep_running_on_close)
    {
        if (tray_ != nullptr && tray_->visible())
        {
            save_layout();
            save_running_now(false);
            hide();
            event->ignore();
            show_background_notice();
            return;
        }
        // Nowhere to keep a hidden Studio: never hide what cannot be found.
        ConfirmDialog dialog(
            tr("Keep the ports running?"),
            tr("This desktop has no notification area to keep Studio in. "
               "Minimise the window to keep the ports running, or stop them "
               "and quit."),
            QVector<confirm_choice>{
                {tr("Cancel"), QString(), true},
                {tr("Stop the ports and quit"), QStringLiteral("danger"),
                 false},
                {tr("Minimise instead"), QStringLiteral("primary"), false}},
            2, this);
        dialog.exec();
        if (dialog.chosen() == 2)
            showMinimized();
        if (dialog.chosen() != 1)
        {
            event->ignore();
            return;
        }
        quitting_ = true;
    }

    if (!confirm_discard() || !confirm_exit())
    {
        quitting_ = false;
        event->ignore();
        return;
    }
    event->accept();
    finish_quit();
}

void ShellWindow::quit_studio()
{
    quitting_ = true;
    if (!confirm_discard() || !confirm_exit())
    {
        quitting_ = false;
        return;
    }
    finish_quit();
}

void ShellWindow::finish_quit()
{
    quitting_ = true;
    if (isVisible())
        save_layout();
    if (!background_)
        return;
    save_running_now(true);
    if (running_save_ != nullptr)
        running_save_->stop();
    if (tray_ != nullptr)
        tray_->hide();
    // The window may be hidden, and the last window closing does not end the
    // program while background running is on, so end it here.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void ShellWindow::enable_background(const QString& running_path, bool use_tray)
{
    if (background_)
        return;
    background_ = true;
    running_path_ = running_path;
    QApplication::setQuitOnLastWindowClosed(false);

    running_save_ = new QTimer(this);
    running_save_->setSingleShot(true);
    running_save_->setInterval(1000);
    connect(running_save_, &QTimer::timeout, this,
            [this] { save_running_now(false); });
    // Signing out or shutting down ends Studio without a Quit: keep what
    // runs so it comes back.
    connect(qApp, &QGuiApplication::commitDataRequest, this,
            [this](QSessionManager&) { save_running_now(false); });

    if (use_tray)
    {
        tray_ = new StudioTray(this);
        connect(tray_, &StudioTray::open_requested, this,
                &ShellWindow::bring_to_front);
        connect(tray_, &StudioTray::quit_requested, this,
                &ShellWindow::quit_studio);
        connect(tray_, &StudioTray::sign_in_toggled, this,
                [this](bool on)
                {
                    if (!set_sign_in_start(on))
                        tray_->notify(tr("Could not change start at sign-in"),
                                      tr("Studio could not write the sign-in "
                                         "entry. Nothing was changed."),
                                      true);
                    tray_->set_sign_in(sign_in_start_available(),
                                       sign_in_start_enabled());
                });
        // Refresh the sign-in item each time the menu opens: the entry can
        // change in Preferences or outside Studio.
        connect(tray_->menu(), &QMenu::aboutToShow, this,
                [this]
                {
                    tray_->set_sign_in(sign_in_start_available(),
                                       sign_in_start_enabled());
                });
        tray_->set_sign_in(sign_in_start_available(), sign_in_start_enabled());
        tray_->show();
    }

    bool damaged = false;
    const auto setup = load_running_setup(running_path_, &damaged);
    if (damaged)
    {
        const QString text = tr("The ports from last time could not be read, "
                                "so Studio started without them.");
        status_->showMessage(text, 10000);
        log_local(text);
    }
    if (setup && !setup->state.stopped && backend_.ports().empty())
        start_running_setup(true);
    update_resume_banner();
    update_tray();
}

bool ShellWindow::needed_in_background() const
{
    return restore_failed_ || !backend_.ports().empty();
}

void ShellWindow::bring_to_front()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void ShellWindow::schedule_running_save()
{
    // A fixed delay from the first change, not from the last: counters make
    // the ports change all the time while traffic flows.
    if (running_save_ != nullptr && !loading_ && !quitting_ &&
        !running_save_->isActive())
        running_save_->start();
}

void ShellWindow::save_running_now(bool stopped)
{
    if (!background_ || running_path_.isEmpty())
        return;
    if (backend_.ports().empty() && !stopped)
    {
        // Nothing runs: keep a stopped setup that can still be started
        // again, and otherwise forget the last one.
        const auto kept = support::load_running_state(running_path_);
        if (kept.stopped)
            return;
    }
    const support::running_state state{document_path_, dirty_, stopped};
    const auto document = backend_.snapshot();
    const auto text = vss::config::to_json(document);
    const QByteArray key =
        (text ? QByteArray::fromStdString(*text) : QByteArray()) +
        state.document_path.toUtf8() + (state.dirty ? "1" : "0") +
        (state.stopped ? "1" : "0");
    if (key == running_written_)
        return;
    if (support::save_running_setup(running_path_, document, state))
        running_written_ = key;
}

void ShellWindow::start_running_setup(bool automatic)
{
    bool damaged = false;
    const auto setup = load_running_setup(running_path_, &damaged);
    if (!setup)
    {
        update_resume_banner();
        return;
    }
    if (!automatic && !backend_.ports().empty())
        return;

    loading_ = true;
    const auto applied = backend_.apply(setup->document, true);
    loading_ = false;
    if (!applied)
    {
        const QString text = tr("Could not start the ports from last time: %1")
                                 .arg(error_text(applied.error()));
        status_->showMessage(text, 10000);
        log_local(text);
        if (tray_ != nullptr && !isVisible())
            tray_->notify(tr("Could not start the ports from last time"),
                          tr("Open Studio to see why and start them again."),
                          true);
        if (automatic)
            restore_failed_ = true;
        // Offer it again from the banner, as after a Quit.
        support::running_state state = setup->state;
        state.stopped = true;
        support::save_running_state(running_path_, state);
        update_resume_banner();
        return;
    }
    restore_failed_ = false;
    document_path_ = setup->state.document_path;
    title_override_.clear();
    last_signature_ = document_signature();
    dirty_ = setup->state.dirty;
    update_title();
    const QString text =
        automatic ? tr("Started the ports that were running before: %1 "
                       "ports, %2 links.")
                        .arg(setup->ports)
                        .arg(setup->links)
                  : tr("Started the ports again: %1 ports, %2 links.")
                        .arg(setup->ports)
                        .arg(setup->links);
    status_->showMessage(text, 6000);
    log_local(text);
    show_workspace();
    status_->refresh();
    save_running_now(false);
    update_resume_banner();
    update_tray();
}

void ShellWindow::update_resume_banner()
{
    if (resume_banner_ == nullptr)
        return;
    if (!background_ || !backend_.ports().empty())
    {
        resume_banner_->setVisible(false);
        return;
    }
    const auto setup = load_running_setup(running_path_);
    if (!setup || !setup->state.stopped)
    {
        resume_banner_->setVisible(false);
        return;
    }
    resume_banner_->set_text(
        tr("When you last quit, %1 ports and %2 links were running.")
            .arg(setup->ports)
            .arg(setup->links));
    resume_banner_->setVisible(true);
}

void ShellWindow::update_tray()
{
    if (tray_ == nullptr)
        return;
    const bool lost =
        backend_.caps().recreate_ports && !backend_.lost_ports().empty();
    tray_->set_state(static_cast<int>(backend_.ports().size()),
                     static_cast<int>(backend_.links().size()), lost);
    if (lost && !was_lost_ && !isVisible())
        tray_->notify(tr("Some COM ports were lost"),
                      tr("Open Studio to recreate them."), true);
    was_lost_ = lost;
}

void ShellWindow::show_background_notice()
{
    if (tray_ == nullptr)
        return;
    QSettings settings;
    const QString key = QStringLiteral("ui/backgroundNoticeShown");
    if (settings.value(key, false).toBool())
        return;
    settings.setValue(key, true);
    tray_->notify(
        tr("Studio is still running your ports"),
        sign_in_start_available()
            ? tr("Open it again from this icon, or quit it there to stop the "
                 "ports. To have them back after a restart, choose Start when "
                 "I sign in in the same menu.")
            : tr("Open it again from this icon, or quit it there to stop the "
                 "ports."));
}

bool ShellWindow::run_selftest()
{
    QStringList failures;
    const auto fail = [&failures](const QString& text)
    {
        failures << text;
        std::fprintf(stderr, "selftest: FAIL: %s\n", text.toUtf8().constData());
    };

    show();
    QApplication::processEvents();

    // Simple mode is always the setups page; an empty setup in Advanced mode
    // is the empty canvas.
    if (!level_.advanced() && central_->currentWidget() != simple_page_)
        fail(QStringLiteral("Simple mode did not show the setups page"));
    if (level_.advanced() && backend_.ports().empty() &&
        central_->currentWidget() != workspace_)
        fail(QStringLiteral(
            "the empty Advanced studio did not show the canvas"));
    if (registry_.actions().isEmpty())
        fail(QStringLiteral("the action registry is empty"));
    // The application icon is in the program and draws something at the sizes
    // a title bar, a taskbar and a dock ask for.
    {
        const QIcon app_icon = IconProvider::instance().app_icon();
        if (app_icon.isNull())
        {
            fail(QStringLiteral("the application icon is missing"));
        }
        else
        {
            for (const int side : {16, 32, 256})
            {
                // At a display scale above 100 percent the pixmap has more
                // pixels than `side`, so the middle is found from its width.
                const QImage image = app_icon.pixmap(side, side).toImage();
                if (image.isNull() || image.width() < side ||
                    qAlpha(image.pixel(image.width() / 2,
                                       image.height() / 2)) == 0)
                {
                    fail(QStringLiteral(
                             "the application icon draws nothing at %1 px")
                             .arg(side));
                }
            }
        }
    }
    if (!registry_.shortcuts_unique())
    {
        fail(QStringLiteral("duplicate shortcuts: %1")
                 .arg(registry_.duplicate_shortcuts().join(
                     QStringLiteral(", "))));
    }
    if (registry_.action(QStringLiteral("file.new")) == nullptr)
        fail(QStringLiteral("File, New is not registered"));
    if (registry_.action(QStringLiteral("tools.search")) == nullptr)
        fail(QStringLiteral("the command palette action is not registered"));
    if (topology_ == nullptr ||
        topology_->accessibleName() != topology_text::view_name())
    {
        fail(QStringLiteral("the topology view is missing its name"));
    }
    if (links_action_ == nullptr ||
        registry_.action(QStringLiteral("view.links")) == nullptr)
    {
        fail(QStringLiteral("the links list action is not registered"));
    }

    const auto created = backend_.create_virtual_port(
        QStringLiteral("selftest"), vss::virtual_port_kind::loopback, true);
    if (!created)
    {
        fail(QStringLiteral("create_virtual_port: %1")
                 .arg(error_text(created.error())));
    }
    else
    {
        backend_.refresh();
        QApplication::processEvents();
        if (!level_.advanced())
        {
            // Simple mode keeps the setups page; the canvas is Advanced.
            if (central_->currentWidget() != simple_page_)
                fail(QStringLiteral("Simple mode left the setups page"));
        }
        else
        {
            if (central_->currentWidget() != workspace_)
                fail(QStringLiteral(
                    "the workspace did not replace the launcher"));
            if (!ports_dock_->isVisible())
                fail(QStringLiteral("the Ports dock is not visible"));
            if (topology_->node_count() != 1)
            {
                fail(QStringLiteral(
                    "the topology did not draw the created port"));
            }
            if (!topology_->summary().contains(QStringLiteral("1 port")))
                fail(QStringLiteral("the topology summary did not update"));
        }
        const auto removed = backend_.remove_port(*created);
        if (!removed)
            fail(QStringLiteral("remove_port: %1")
                     .arg(error_text(removed.error())));
        backend_.refresh();
        QApplication::processEvents();
        if (!level_.advanced() && central_->currentWidget() != simple_page_)
            fail(QStringLiteral("the setups page did not stay after removal"));
        if (level_.advanced() && central_->currentWidget() != workspace_)
            fail(QStringLiteral("the empty canvas did not stay after removal"));
    }

    if (backend_.caps().shared_bus)
    {
        // A shared bus through the same backend the Connect dialog uses: three
        // members, one link, and the link goes when the members do.
        std::vector<vss::endpoint_id> members;
        for (const char* name : {"selftest-a", "selftest-b", "selftest-c"})
        {
            const auto member = backend_.create_virtual_port(
                QString::fromLatin1(name), vss::virtual_port_kind::loopback,
                true);
            if (!member)
            {
                fail(QStringLiteral("create_virtual_port for the bus: %1")
                         .arg(error_text(member.error())));
                break;
            }
            members.push_back(*member);
        }
        if (members.size() == 3)
        {
            const auto bus = backend_.bus(members, {});
            backend_.refresh();
            QApplication::processEvents();
            const auto links = backend_.links();
            if (!bus)
            {
                fail(QStringLiteral("bus: %1").arg(error_text(bus.error())));
            }
            else if (links.size() != 1 ||
                     links.front().shape != link_shape::bus ||
                     links.front().sources.size() != 3)
            {
                fail(QStringLiteral("the shared bus did not show as one link "
                                    "of three ports"));
            }
        }
        for (const vss::endpoint_id id : members)
            static_cast<void>(backend_.remove_port(id));
        backend_.refresh();
        QApplication::processEvents();
        if (!backend_.links().empty())
            fail(QStringLiteral("removing the members left the bus behind"));
    }

    qputenv("VSS_HIGH_CONTRAST", QByteArrayLiteral("1"));
    Theme::instance().set_mode(theme_mode::dark);
    Theme::instance().set_mode(theme_mode::light);
    if (qApp == nullptr || !qApp->styleSheet().isEmpty())
        fail(
            QStringLiteral("high contrast did not drop the custom stylesheet"));
    qunsetenv("VSS_HIGH_CONTRAST");
    qputenv("VSS_REDUCE_MOTION", QByteArrayLiteral("1"));
    if (!Theme::instance().reduced_motion() ||
        Theme::instance().motion_ms(motion_kind::hover) != 0)
    {
        fail(QStringLiteral("reduced motion did not stop the animations"));
    }
    qunsetenv("VSS_REDUCE_MOTION");
    Theme::instance().set_mode(theme_mode::dark);
    Theme::instance().set_mode(theme_mode::light);

    set_dirty(false);
    failures += run_accessibility_checks(registry_, this);
    if (!failures.isEmpty())
    {
        for (const auto& text : failures)
            std::fprintf(stderr, "selftest: %s\n", text.toUtf8().constData());
        return false;
    }
    return true;
}

} // namespace vss::gui

namespace vss::gui
{

void ShellWindow::sync_canvas_editing()
{
    if (canvas_ == nullptr)
        return;
    const bool advanced = level_.advanced();
    canvas_->set_editing(advanced);
    QAction* toolbox = registry_.action(QStringLiteral("view.toolbox"));
    if (toolbox_ != nullptr)
    {
        const bool shown = toolbox == nullptr || toolbox->isChecked();
        toolbox_->setVisible(advanced && shown);
    }
}

} // namespace vss::gui
