//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/debug_access.hpp"

#include "backend/plugin_backend.hpp"
#include "backend/plugin_view.hpp"
#include "widgets/dialogs.hpp"

#include <vss/control/audit_log.hpp>
#include <vss/debug/client.hpp>
#include <vss/debug/redact.hpp>
#include <vss/debug/server.hpp>
#include <vss/debug/studio_provider.hpp>
#include <vss/machine/computer_name.hpp>
#include <vss/studio/studio.hpp>

#include <boost/json.hpp>

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QLineEdit>
#include <QMainWindow>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QWidget>

#include <chrono>
#include <future>
#include <memory>
#include <optional>

namespace vss::gui::support
{
namespace
{

namespace json = boost::json;
namespace dbg = vss::debug;

constexpr auto gui_wait = std::chrono::seconds(3);
constexpr std::size_t picture_most = 900 * 1024;
constexpr int value_text_most = 200;
constexpr int children_most = 400;

/// Runs `body` on the window's thread and waits for it, up to `gui_wait`.  A
/// window that is busy gives `nullopt` after the wait and the late result is
/// dropped; the worker never waits for ever.  `body` must not touch the
/// provider, which may be gone by the time it runs.
template <class Body>
auto on_gui_thread(Body body) -> std::optional<decltype(body())>
{
    using result_type = decltype(body());
    auto promise = std::make_shared<std::promise<result_type>>();
    auto future = promise->get_future();
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [promise, body = std::move(body)]() mutable
        { promise->set_value(body()); }, Qt::QueuedConnection);
    if (future.wait_for(gui_wait) != std::future_status::ready)
        return std::nullopt;
    return future.get();
}

std::string plugin_state_name(plugin_state state)
{
    switch (state)
    {
    case plugin_state::available:
        return "available";
    case plugin_state::starting:
        return "starting";
    case plugin_state::off:
        return "off";
    case plugin_state::needs_decision:
        return "needs_decision";
    case plugin_state::blocked:
        return "blocked";
    case plugin_state::needs_isolation:
        return "needs_isolation";
    case plugin_state::not_used:
        return "not_used";
    case plugin_state::needs_licence:
        return "needs_licence";
    case plugin_state::stopped:
        return "stopped";
    case plugin_state::needs_newer_studio:
        return "needs_newer_studio";
    case plugin_state::cannot_be_used:
        return "cannot_be_used";
    case plugin_state::not_installed:
        return "not_installed";
    }
    return "unknown";
}

std::string text_of(const QString& text)
{
    return text.toStdString();
}

QString shown(QString text)
{
    text.replace(QLatin1Char('<'), QLatin1Char('?'));
    text.replace(QLatin1Char('>'), QLatin1Char('?'));
    return text;
}

/// A word in the name of a control that holds a secret.
bool names_a_secret(const QString& name)
{
    for (const char* word : {"password", "passwd", "token", "secret", "key"})
    {
        if (name.contains(QLatin1String(word), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

QString label_of(const QWidget* widget)
{
    QString name = widget->accessibleName();
    if (name.isEmpty())
        name = widget->objectName();
    return name;
}

QString cut_text(QString text)
{
    if (text.size() > value_text_most)
    {
        text.truncate(value_text_most);
        text += QStringLiteral("...");
    }
    return text;
}

/// The window the person is working in: the modal sheet if there is one, else
/// the active window, else the main window.
QWidget* working_window()
{
    if (QWidget* modal = QApplication::activeModalWidget())
        return modal;
    if (QWidget* active = QApplication::activeWindow())
        return active;
    for (QWidget* top : QApplication::topLevelWidgets())
    {
        if (qobject_cast<QMainWindow*>(top) != nullptr)
            return top;
    }
    return nullptr;
}

json::object window_object(const QWidget* window)
{
    json::object out;
    out["class"] = window->metaObject()->className();
    out["object_name"] = text_of(window->objectName());
    out["title"] = text_of(window->windowTitle());
    out["visible"] = window->isVisible();
    out["modal"] = window->isModal();
    out["active"] = window->isActiveWindow();
    const QRect box = window->geometry();
    out["width"] = box.width();
    out["height"] = box.height();
    return out;
}

/// The values of the controls in a window, by name.  A control that holds a
/// secret (a password field, or a name that says so) gives no value.
json::array control_values(QWidget* window)
{
    json::array out;
    int seen = 0;
    for (QWidget* child : window->findChildren<QWidget*>())
    {
        if (++seen > children_most)
            break;
        if (!child->isVisibleTo(window))
            continue;
        json::object item;
        const QString name = label_of(child);
        bool recorded = false;
        if (const auto* line = qobject_cast<const QLineEdit*>(child))
        {
            // The spin box's own line edit has no name of its own.
            if (qobject_cast<const QAbstractSpinBox*>(line->parentWidget()) !=
                nullptr)
                continue;
            if (line->echoMode() == QLineEdit::Normal && !names_a_secret(name))
                item["value"] = text_of(cut_text(line->text()));
            else
                item["value"] = "(hidden)";
            recorded = true;
        }
        else if (const auto* spin =
                     qobject_cast<const QAbstractSpinBox*>(child))
        {
            item["value"] = text_of(spin->text());
            recorded = true;
        }
        else if (const auto* combo = qobject_cast<const QComboBox*>(child))
        {
            item["value"] = text_of(cut_text(combo->currentText()));
            recorded = true;
        }
        else if (const auto* button =
                     qobject_cast<const QAbstractButton*>(child))
        {
            if (button->isCheckable())
            {
                item["checked"] = button->isChecked();
                recorded = true;
            }
        }
        else if (const auto* slider = qobject_cast<const QSlider*>(child))
        {
            item["value"] = slider->value();
            recorded = true;
        }
        if (!recorded)
            continue;
        item["control"] = child->metaObject()->className();
        item["name"] = text_of(name);
        item["enabled"] = child->isEnabled();
        out.push_back(std::move(item));
    }
    return out;
}

/// What the window knows that the engine does not, as the provider's own.
class window_provider final : public dbg::studio_provider
{
public:
    window_provider(vss::studio& target, QPointer<PluginBackend> plugins)
        : studio_provider(target), plugins_(std::move(plugins))
    {
    }

    dbg::op_result plugins() override
    {
        const QPointer<PluginBackend> host = plugins_;
        const auto made = on_gui_thread(
            [host]() -> std::optional<std::string>
            {
                if (host.isNull())
                    return std::nullopt;
                const plugin_snapshot_ptr snapshot = host->snapshot();
                if (!snapshot)
                    return std::nullopt;
                json::object out;
                out["can_run_plugins"] = snapshot->can_run_plugins;
                out["user_plugins_allowed"] = snapshot->user_plugins_allowed;
                json::array list;
                for (const plugin_view& item : snapshot->plugins)
                {
                    json::object row;
                    row["id"] = text_of(item.id);
                    row["name"] = text_of(item.name);
                    row["version"] = text_of(item.version);
                    row["state"] = plugin_state_name(item.state);
                    row["wished_on"] = item.wished_on;
                    row["reason"] = text_of(item.reason_code);
                    row["failure_count"] = item.failure_count;
                    row["attempt"] = item.attempt;
                    row["back_off_seconds"] = item.back_off_seconds;
                    json::array lines;
                    const auto from = item.log.size() > 10
                                          ? item.log.size() - 10
                                          : std::size_t{0};
                    for (std::size_t index = from; index < item.log.size();
                         ++index)
                        lines.push_back(json::string(
                            text_of(cut_text(item.log[index].text))));
                    row["log"] = std::move(lines);
                    list.push_back(std::move(row));
                }
                out["plugins"] = std::move(list);
                out["readers"] = snapshot->readers.size();
                return json::serialize(out);
            });
        if (!made)
            return dbg::op_result::fail("timeout",
                                        "The window did not answer in time.");
        if (!*made)
            return dbg::op_result::fail("unsupported",
                                        "There is no plugin host.");
        return dbg::op_result::done(**made);
    }

    dbg::op_result gui_state() override
    {
        const auto made = on_gui_thread(
            []() -> std::string
            {
                json::object out;
                out["application"] =
                    text_of(QCoreApplication::applicationName());
                out["version"] =
                    text_of(QCoreApplication::applicationVersion());
                json::array windows;
                for (const QWidget* top : QApplication::topLevelWidgets())
                {
                    if (!top->isVisible())
                        continue;
                    windows.push_back(window_object(top));
                }
                out["windows"] = std::move(windows);
                if (QWidget* active = working_window())
                {
                    json::object sheet = window_object(active);
                    sheet["controls"] = control_values(active);
                    out["working_window"] = std::move(sheet);
                }
                if (const QWidget* focus = QApplication::focusWidget())
                {
                    json::object item;
                    item["class"] = focus->metaObject()->className();
                    item["name"] = text_of(label_of(focus));
                    out["focus"] = std::move(item);
                }
                return json::serialize(out);
            });
        if (!made)
            return dbg::op_result::fail("timeout",
                                        "The window did not answer in time.");
        return dbg::op_result::done(*made);
    }

    dbg::op_result screenshot(const std::string& args_json) override
    {
        boost::system::error_code ec;
        const json::value given = json::parse(args_json, ec);
        std::string which = "active";
        if (!ec && given.is_object())
        {
            if (const auto* found = given.as_object().if_contains("window");
                found != nullptr && found->is_string())
                which = std::string(found->as_string());
        }
        const auto made = on_gui_thread(
            [which]() -> std::optional<QByteArray>
            {
                QWidget* target = nullptr;
                if (which == "main")
                {
                    for (QWidget* top : QApplication::topLevelWidgets())
                    {
                        if (qobject_cast<QMainWindow*>(top) != nullptr)
                            target = top;
                    }
                }
                else if (which == "active")
                {
                    target = working_window();
                }
                else
                {
                    const QString wanted = QString::fromStdString(which);
                    for (QWidget* top : QApplication::topLevelWidgets())
                    {
                        if (top->isVisible() &&
                            top->windowTitle().contains(wanted,
                                                        Qt::CaseInsensitive))
                            target = top;
                    }
                }
                if (target == nullptr)
                    return std::nullopt;
                QPixmap picture = target->grab();
                // A frame holds a megabyte at most: smaller until it fits.
                for (int attempt = 0; attempt < 6; ++attempt)
                {
                    QByteArray bytes;
                    QBuffer buffer(&bytes);
                    buffer.open(QIODevice::WriteOnly);
                    picture.save(&buffer, "PNG");
                    if (static_cast<std::size_t>(bytes.size()) <= picture_most)
                        return bytes;
                    picture = picture.scaled(picture.size() * 3 / 4,
                                             Qt::KeepAspectRatio,
                                             Qt::SmoothTransformation);
                }
                return std::nullopt;
            });
        if (!made)
            return dbg::op_result::fail("timeout",
                                        "The window did not answer in time.");
        if (!*made)
            return dbg::op_result::fail("not_found",
                                        "No such window, or its picture is "
                                        "too large to send.");
        dbg::op_result out = dbg::op_result::done();
        const QByteArray& bytes = **made;
        out.blob.assign(reinterpret_cast<const vss::byte*>(bytes.constData()),
                        reinterpret_cast<const vss::byte*>(bytes.constData()) +
                            bytes.size());
        return out;
    }

private:
    QPointer<PluginBackend> plugins_;
};

std::string scope_text(unsigned set)
{
    std::string out;
    for (const dbg::scope item :
         {dbg::scope::read, dbg::scope::act, dbg::scope::gui, dbg::scope::data})
    {
        if (!dbg::has(set, item))
            continue;
        if (!out.empty())
            out += ", ";
        out += dbg::scope_name(item);
    }
    return out;
}

} // namespace

DebugAccess::DebugAccess() = default;

DebugAccess::~DebugAccess()
{
    if (server_)
        server_->stop();
}

DebugAccess& DebugAccess::instance()
{
    static DebugAccess* const self = new DebugAccess();
    return *self;
}

void DebugAccess::attach(vss::studio& engine, PluginBackend* plugins)
{
    engine_ = &engine;
    plugins_ = plugins;
}

void DebugAccess::use_directories(std::filesystem::path audit,
                                  std::filesystem::path instances)
{
    audit_directory_ = std::move(audit);
    instance_directory_ = std::move(instances);
}

bool DebugAccess::enable()
{
    if (engine_ == nullptr || server_)
        return server_ != nullptr;

    dbg::server_options options;
    options.source = std::make_shared<window_provider>(
        *engine_, QPointer<PluginBackend>(plugins_));
    if (!audit_directory_.empty())
    {
        options.audit_directory = audit_directory_;
        options.instance_directory = instance_directory_;
    }
    else
    {
        if (const auto audit = dbg::default_audit_directory())
            options.audit_directory = *audit;
        if (const auto instances = dbg::default_instance_directory())
            options.instance_directory = *instances;
    }
    options.about_json = [&]
    {
        json::object about;
        about["application"] = text_of(QCoreApplication::applicationName());
        about["version"] = text_of(QCoreApplication::applicationVersion());
        return json::serialize(about);
    }();
    options.redact = dbg::redactor::for_user(
        text_of(QDir::homePath()),
        text_of(qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER"))),
        vss::machine::computer_name());
    options.on_change = [this]
    {
        QMetaObject::invokeMethod(
            this, [this] { Q_EMIT changed(); }, Qt::QueuedConnection);
    };
    options.approve = [this](const dbg::client_request& request,
                             std::function<void(dbg::approval)> answer)
    {
        const QString name = QString::fromStdString(request.name);
        const QString program = QString::fromStdString(request.program);
        const quint64 process = request.process_id;
        const unsigned wanted = request.wanted;
        QMetaObject::invokeMethod(
            this,
            [this, name, program, process, wanted, answer]
            {
                ask_person(name, program, process, wanted,
                           [answer](unsigned granted, bool remember)
                           {
                               dbg::approval reply;
                               reply.granted = granted;
                               reply.remember = remember;
                               answer(reply);
                           });
            },
            Qt::QueuedConnection);
    };

    auto started = dbg::server::start(std::move(options));
    if (!started)
    {
        error_ = QString::fromStdString(started.error().message());
        Q_EMIT changed();
        return false;
    }
    server_ = std::move(started).value();
    error_.clear();
    Q_EMIT changed();
    return true;
}

void DebugAccess::disable()
{
    if (!server_)
        return;
    // The server is stopped before it is dropped, so a client that is in the
    // middle of a call is closed and not left waiting.
    server_->stop();
    server_.reset();
    Q_EMIT changed();
}

bool DebugAccess::traffic_on() const
{
    return server_ && server_->traffic_on();
}

void DebugAccess::set_traffic(bool on)
{
    if (!server_)
        return;
    server_->set_traffic(on);
    Q_EMIT changed();
}

QString DebugAccess::summary() const
{
    if (!server_)
        return QCoreApplication::translate("DebugAccess",
                                           "Debug access is off.");
    const int count = static_cast<int>(server_->clients().size());
    QString text = QCoreApplication::translate(
        "DebugAccess",
        "Debug access is on. Programs on this computer that you allow can "
        "read what Studio is doing.");
    if (count > 0)
        text += QLatin1Char(' ') + QCoreApplication::translate(
                                       "DebugAccess", "Programs connected: %1.")
                                       .arg(count);
    if (server_->traffic_on())
        text += QLatin1Char(' ') +
                QCoreApplication::translate(
                    "DebugAccess", "The bytes of traffic can be read.");
    return text;
}

QString DebugAccess::chip_text() const
{
    if (!server_)
        return {};
    const int count = static_cast<int>(server_->clients().size());
    if (count == 0)
        return QCoreApplication::translate("DebugAccess", "Debug access on");
    return QCoreApplication::translate("DebugAccess",
                                       "Debug access on (%1 connected)")
        .arg(count);
}

QVector<DebugAccess::client_row> DebugAccess::clients() const
{
    QVector<client_row> out;
    if (!server_)
        return out;
    for (const auto& item : server_->clients())
    {
        client_row row;
        row.name = QString::fromStdString(item.name);
        row.program = QString::fromStdString(item.program);
        row.process = item.process_id;
        row.scopes = QString::fromStdString(scope_text(item.granted));
        out.push_back(row);
    }
    return out;
}

void DebugAccess::disconnect_all()
{
    if (server_)
        server_->disconnect_all();
}

void DebugAccess::ask_person(const QString& name, const QString& program,
                             quint64 process, unsigned wanted,
                             std::function<void(unsigned, bool)> answer)
{
    QString what;
    const auto line = [&what](const QString& text)
    { what += QStringLiteral("\n- ") + text; };
    if (dbg::has(wanted, dbg::scope::read))
        line(QCoreApplication::translate(
            "DebugAccess",
            "read the state of Studio's ports, its engine and Windows"));
    if (dbg::has(wanted, dbg::scope::act))
        line(QCoreApplication::translate(
            "DebugAccess", "try operations on ports, which can change them"));
    if (dbg::has(wanted, dbg::scope::gui))
        line(QCoreApplication::translate(
            "DebugAccess",
            "read what the window shows and take a picture of it, which can "
            "show names and paths"));
    if (dbg::has(wanted, dbg::scope::data))
        line(QCoreApplication::translate(
            "DebugAccess",
            "read the bytes your ports carry, when the traffic switch is on"));

    const QString body =
        QCoreApplication::translate(
            "DebugAccess", "%1 (process %2) wants to:%3\n\nProgram: %4")
            .arg(shown(name))
            .arg(process)
            .arg(what)
            .arg(program.isEmpty()
                     ? QCoreApplication::translate("DebugAccess", "not known")
                     : shown(program));
    const QVector<confirm_choice> choices{
        {QCoreApplication::translate("DebugAccess", "Don't allow"), QString(),
         true},
        {QCoreApplication::translate("DebugAccess", "Allow once"), QString(),
         false},
        {QCoreApplication::translate("DebugAccess", "Allow until Studio quits"),
         QStringLiteral("primary"), false}};
    auto* dialog = new ConfirmDialog(
        QCoreApplication::translate("DebugAccess",
                                    "Allow this program to use debug access?"),
        body, choices, 0, QApplication::activeWindow());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("debugApprovalDialog"));
    connect(dialog, &QDialog::finished, this,
            [dialog, answer, wanted](int)
            {
                switch (dialog->chosen())
                {
                case 1:
                    answer(wanted, false);
                    break;
                case 2:
                    answer(wanted, true);
                    break;
                default:
                    answer(0, false);
                    break;
                }
            });
    // The server stops waiting after a minute; so does the question.
    QTimer::singleShot(std::chrono::seconds(60), dialog, &QDialog::reject);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

} // namespace vss::gui::support
