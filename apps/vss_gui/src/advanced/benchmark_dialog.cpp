//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/benchmark_dialog.hpp"

#include "advanced/chrome.hpp"
#include "theme/icon_provider.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/bytes.hpp>
#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui
{
namespace
{

QString port_label(const port_view& port)
{
    if (port.path.isEmpty())
        return port.name;
    return QCoreApplication::translate("Benchmark", "%1 (%2)")
        .arg(port.name, port.path);
}

} // namespace

BenchmarkDialog::BenchmarkDialog(StudioBackend& backend, QWidget* parent)
    : QDialog(parent), backend_(backend)
{
    build();
    refresh();
}

BenchmarkDialog::~BenchmarkDialog()
{
    cancel_.store(true);
    if (worker_.joinable())
        worker_.join();
}

void BenchmarkDialog::done(int result)
{
    // The thread is joined by the destructor or by the result handler; this
    // only asks it to stop, so closing the dialog never waits for a write.
    if (running_)
        cancel_.store(true);
    QDialog::done(result);
}

void BenchmarkDialog::build()
{
    VSS_PROFILE_ZONE("gui.advanced.benchmark.build");
    VSS_PROFILE_MESSAGE("gui.dialog.benchmark.opened");
    const QString title = tr("Benchmark a connection");
    setWindowTitle(title);
    setAccessibleName(title);
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    resize(Theme::instance().metric(
               QStringLiteral("layout.dialogWidths.standard")),
           720);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 0);
    layout->setSpacing(14);
    layout->addWidget(make_header(
        title,
        tr("Sends data from one port to another and counts what arrives."),
        this, true, {}));

    // Input grid: the two ports on the first row, then the size of the run.
    // The port lists are filled in later by refresh().
    auto* fields = new QGridLayout();
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setHorizontalSpacing(12);
    fields->setVerticalSpacing(10);

    from_ = new QComboBox(this);
    from_->setAccessibleName(tr("Send from"));
    fields->addWidget(new FormRow(tr("Send from"), from_, {}, this), 0, 0);

    to_ = new QComboBox(this);
    to_->setAccessibleName(tr("Receive on"));
    fields->addWidget(new FormRow(tr("Receive on"), to_, {}, this), 0, 1);

    // The item data holds the text the CLI takes for --bytes and --chunk, so
    // update_command() can paste it straight into the command line.
    amount_ = new QComboBox(this);
    amount_->setAccessibleName(tr("Amount of data"));
    amount_->addItem(QStringLiteral("1 MB"), QStringLiteral("1MB"));
    amount_->addItem(QStringLiteral("10 MB"), QStringLiteral("10MB"));
    amount_->addItem(QStringLiteral("100 MB"), QStringLiteral("100MB"));
    amount_->setCurrentIndex(1);
    fields->addWidget(new FormRow(tr("Amount of data"), amount_, {}, this), 1,
                      0);

    chunk_ = new QComboBox(this);
    chunk_->setAccessibleName(tr("Chunk size"));
    chunk_->addItem(QStringLiteral("1 KB"), QStringLiteral("1KB"));
    chunk_->addItem(QStringLiteral("4 KB"), QStringLiteral("4KB"));
    chunk_->addItem(QStringLiteral("64 KB"), QStringLiteral("64KB"));
    chunk_->setCurrentIndex(1);
    fields->addWidget(new FormRow(tr("Chunk size"), chunk_, {}, this), 1, 1);
    layout->addLayout(fields);

    // A fixed hint, plus a warning that update_state() shows only while the
    // chosen ports are linked.
    layout->addWidget(new NoteBanner(
        NoteBanner::kind::info,
        tr("Do not link these two ports in Studio. The data has to travel "
           "through the cable or driver pair, so the count means something."),
        this));

    linked_note_ = new NoteBanner(
        NoteBanner::kind::warn,
        tr("These two ports are linked in Studio. Disconnect them first."),
        this);
    linked_note_->hide();
    layout->addWidget(linked_note_);

    // Result card: a status chip and finish time, a progress bar, then four
    // tiles.  Everything starts in its "not run" state.
    auto* result_card = new QFrame(this);
    result_card->setProperty("group", true);
    polish(result_card);
    auto* result_layout = new QVBoxLayout(result_card);
    result_layout->setContentsMargins(16, 16, 16, 16);
    result_layout->setSpacing(12);

    auto* result_row = new QHBoxLayout();
    result_row->setContentsMargins(0, 0, 0, 0);
    result_row->setSpacing(10);
    result_ = new StatusChip(tr("Not run yet"), StatusChip::kind::waiting,
                             result_card);
    result_->setAccessibleName(tr("Benchmark result: not run yet"));
    result_row->addWidget(result_, 0, Qt::AlignVCenter);
    result_row->addStretch(1);
    finished_ = new QLabel(tr("Not started"), result_card);
    finished_->setFont(Theme::instance().font(text_style::caption));
    finished_->setProperty("muted", true);
    result_row->addWidget(finished_);
    result_layout->addLayout(result_row);

    progress_ = new QProgressBar(result_card);
    progress_->setAccessibleName(tr("Benchmark progress"));
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(false);
    result_layout->addWidget(progress_);

    auto* metrics = new QGridLayout();
    metrics->setContentsMargins(0, 0, 0, 0);
    metrics->setHorizontalSpacing(10);
    // QT_TR_NOOP marks the names for translation; tr() looks them up below.
    // The value labels are kept in metrics_ so a run can update them.
    const char* names[4] = {QT_TR_NOOP("Sent"), QT_TR_NOOP("Received"),
                            QT_TR_NOOP("Lost"), QT_TR_NOOP("Throughput")};
    for (int index = 0; index < 4; ++index)
    {
        auto* cell = new QFrame(result_card);
        cell->setProperty("tile", QStringLiteral("raised"));
        polish(cell);
        auto* cell_layout = new QVBoxLayout(cell);
        cell_layout->setContentsMargins(14, 10, 14, 10);
        cell_layout->setSpacing(2);
        auto* key = new QLabel(tr(names[index]), cell);
        key->setFont(Theme::instance().font(text_style::caption));
        key->setProperty("muted", true);
        cell_layout->addWidget(key);
        auto* value = new QLabel(tr("Not run"), cell);
        value->setFont(Theme::instance().font(text_style::title));
        value->setAccessibleName(tr("%1: not run yet").arg(tr(names[index])));
        cell_layout->addWidget(value);
        metrics_[index] = value;
        metrics->addWidget(cell, 0, index);
    }
    result_layout->addLayout(metrics);

    auto* result_caption = new QLabel(
        tr("A run fails when the received count is short, and the result says "
           "by how much."),
        result_card);
    result_caption->setFont(Theme::instance().font(text_style::caption));
    result_caption->setProperty("muted", true);
    result_caption->setWordWrap(true);
    result_layout->addWidget(result_caption);
    layout->addWidget(result_card);

    // The equivalent `vss_cli bench` command, kept current by
    // update_command().
    auto* command_title =
        new QLabel(tr("Same thing from the command line"), this);
    command_title->setFont(Theme::instance().font(text_style::caption));
    command_title->setProperty("muted", true);
    layout->addWidget(command_title);
    command_ = new CodeBlock(this);
    command_->set_text(QString(), tr("Command"));
    layout->addWidget(command_);
    layout->addStretch(1);

    // Footer: Copy result stays disabled until a run has produced a summary.
    QHBoxLayout* footer_layout = nullptr;
    auto* footer = make_footer(this, footer_layout);
    copy_result_ = make_button(tr("Copy result"), "quiet", footer);
    copy_result_->setAccessibleName(tr("Copy result"));
    copy_result_->setEnabled(false);
    connect(copy_result_, &QPushButton::clicked, this,
            [this]
            {
                if (!has_result_)
                    return;
                QGuiApplication::clipboard()->setText(summary_);
                Q_EMIT message(tr("Copied the result."));
            });
    footer_layout->addWidget(copy_result_);
    auto* close = make_button(tr("Close"), nullptr, footer);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    footer_layout->addWidget(close);
    run_ = make_button(tr("Run"), "primary", footer);
    run_->setIcon(IconProvider::instance().icon(QStringLiteral("run"),
                                                colour_role::onAccent, 16));
    run_->setAccessibleName(tr("Run"));
    connect(run_, &QPushButton::clicked, this, &BenchmarkDialog::run);
    footer_layout->addWidget(run_);
    layout->addWidget(footer);

    // Port choices change what can run; size choices change only the command.
    // A change in the ports or links of the backend rebuilds the port lists.
    connect(from_, &QComboBox::currentIndexChanged, this,
            [this] { update_state(); });
    connect(to_, &QComboBox::currentIndexChanged, this,
            [this] { update_state(); });
    connect(amount_, &QComboBox::currentIndexChanged, this,
            [this] { update_command(); });
    connect(chunk_, &QComboBox::currentIndexChanged, this,
            [this] { update_command(); });
    connect(&backend_, &StudioBackend::ports_changed, this,
            &BenchmarkDialog::refresh);
    connect(&backend_, &StudioBackend::links_changed, this,
            &BenchmarkDialog::refresh);

    // Focus the first field now, and again once the event loop runs so the
    // focus still holds after the dialog is shown.
    from_->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(0, from_,
                       [this] { from_->setFocus(Qt::OtherFocusReason); });
}

void BenchmarkDialog::refresh()
{
    VSS_PROFILE_ZONE("gui.advanced.benchmark.refresh");
    const vss::endpoint_id previous_from =
        from_->count() == 0
            ? 0
            : static_cast<vss::endpoint_id>(from_->currentData().toULongLong());
    const vss::endpoint_id previous_to =
        to_->count() == 0
            ? 0
            : static_cast<vss::endpoint_id>(to_->currentData().toULongLong());

    const QSignalBlocker from_blocker(from_);
    const QSignalBlocker to_blocker(to_);
    from_->clear();
    to_->clear();
    for (const auto& port : backend_.ports())
    {
        from_->addItem(port_label(port), static_cast<qulonglong>(port.id));
        to_->addItem(port_label(port), static_cast<qulonglong>(port.id));
    }
    const auto select = [](QComboBox* combo, vss::endpoint_id id, int fallback)
    {
        int index =
            id == 0 ? fallback : combo->findData(static_cast<qulonglong>(id));
        if (index < 0)
            index = fallback;
        if (index >= 0 && index < combo->count())
            combo->setCurrentIndex(index);
    };
    select(from_, previous_from, 0);
    select(to_, previous_to, to_->count() > 1 ? 1 : 0);
    if (from_->currentIndex() == to_->currentIndex() && from_->count() > 1)
        to_->setCurrentIndex((from_->currentIndex() + 1) % from_->count());
    update_command();
    update_state();
}

bool BenchmarkDialog::ports_linked() const
{
    const vss::endpoint_id from =
        static_cast<vss::endpoint_id>(from_->currentData().toULongLong());
    const vss::endpoint_id to =
        static_cast<vss::endpoint_id>(to_->currentData().toULongLong());
    if (from == 0 || to == 0)
        return false;
    for (const auto& link : backend_.links())
    {
        const bool forward =
            link.sources.contains(from) && link.targets.contains(to);
        const bool backward =
            link.sources.contains(to) && link.targets.contains(from);
        const bool on_bus = link.shape == link_shape::bus &&
                            link.sources.contains(from) &&
                            link.sources.contains(to);
        if (forward || backward || on_bus)
            return true;
    }
    return false;
}

QString BenchmarkDialog::block_reason() const
{
    if (backend_.ports().size() < 2)
        return tr("Add two ports to benchmark a connection.");
    const vss::endpoint_id from =
        static_cast<vss::endpoint_id>(from_->currentData().toULongLong());
    const vss::endpoint_id to =
        static_cast<vss::endpoint_id>(to_->currentData().toULongLong());
    if (from == 0 || to == 0 || from == to)
        return tr("Choose two different ports.");
    if (ports_linked())
        return tr("These two ports are linked in Studio. Disconnect them "
                  "first.");
    if (!backend_.caps().benchmark)
        return not_available_text();
    return {};
}

bool BenchmarkDialog::can_run() const
{
    return block_reason().isEmpty();
}

QString BenchmarkDialog::metric_text(int index) const
{
    if (index < 0 || index >= 4)
        return {};
    return metrics_[index]->text();
}

QString BenchmarkDialog::finished_text() const
{
    return finished_->text();
}

QString BenchmarkDialog::result_summary() const
{
    return result_->text();
}

QString BenchmarkDialog::command() const
{
    return command_->text();
}

QString BenchmarkDialog::block_note_text() const
{
    return linked_note_->text();
}

void BenchmarkDialog::update_command()
{
    VSS_PROFILE_ZONE("gui.advanced.benchmark.update_command");
    const auto path_of = [this](QComboBox* combo)
    {
        // An angle-bracket placeholder is copy-deck style: the command is not
        // runnable until there are two ports, so it must not name a fake one.
        if (combo->count() == 0)
            return QStringLiteral("<port>");
        const auto id =
            static_cast<vss::endpoint_id>(combo->currentData().toULongLong());
        const auto port = backend_.port(id);
        if (!port)
            return QStringLiteral("<port>");
        const QString path = port->path.isEmpty() ? port->name : port->path;
        // The CLI takes a URI; add the serial scheme only when the port does
        // not already carry one.
        return path.contains(QStringLiteral("://"))
                   ? path
                   : QStringLiteral("serial://") + path;
    };
    const QString from = path_of(from_);
    const QString to = path_of(to_);
    const QString bytes = amount_->currentData().toString();
    const QString chunk = chunk_->currentData().toString();
    command_->set_text(QStringLiteral("vss_cli bench --from=%1 --to=%2 "
                                      "--bytes=%3 --chunk=%4")
                           .arg(from, to, bytes, chunk),
                       tr("Command"));
}

void BenchmarkDialog::update_state()
{
    const bool linked = ports_linked();
    linked_note_->setVisible(linked);
    for (QComboBox* combo : {from_, to_, amount_, chunk_})
        combo->setEnabled(!running_);
    if (running_)
    {
        // While a run is going the same button stops it.
        run_->setText(tr("Cancel"));
        run_->setEnabled(true);
        run_->setToolTip({});
        run_->setAccessibleDescription({});
    }
    else
    {
        const QString reason = block_reason();
        run_->setText(tr("Run"));
        run_->setEnabled(can_run());
        run_->setToolTip(reason);
        run_->setAccessibleDescription(reason);
    }
    copy_result_->setEnabled(has_result_ && !running_);
}

void BenchmarkDialog::set_metrics(const QString& sent, const QString& received,
                                  const QString& lost,
                                  const QString& throughput)
{
    static const char* const names[4] = {
        QT_TR_NOOP("Sent"), QT_TR_NOOP("Received"), QT_TR_NOOP("Lost"),
        QT_TR_NOOP("Throughput")};
    const QString values[4] = {sent, received, lost, throughput};
    for (int index = 0; index < 4; ++index)
    {
        metrics_[index]->setText(values[index]);
        metrics_[index]->setAccessibleName(
            tr("%1: %2").arg(tr(names[index]), values[index]));
    }
}

void BenchmarkDialog::run()
{
    if (running_)
    {
        cancel_.store(true);
        return;
    }
    if (!can_run())
    {
        Q_EMIT message(block_reason());
        return;
    }
    start();
}

void BenchmarkDialog::start()
{
    VSS_PROFILE_ZONE("gui.advanced.benchmark.start");
    const auto bytes =
        vss::parse_byte_size(amount_->currentData().toString().toStdString());
    const auto chunk =
        vss::parse_byte_size(chunk_->currentData().toString().toStdString());
    if (!bytes || !chunk || *chunk == 0)
        return;

    vss::benchmark_options options;
    options.bytes = *bytes;
    options.chunk = static_cast<std::size_t>(*chunk);
    const auto from =
        static_cast<vss::endpoint_id>(from_->currentData().toULongLong());
    const auto to =
        static_cast<vss::endpoint_id>(to_->currentData().toULongLong());

    if (worker_.joinable())
        worker_.join();
    cancel_.store(false);
    running_ = true;
    has_result_ = false;
    summary_.clear();
    progress_->setValue(0);
    result_->set_text(tr("Running"));
    result_->set_kind(StatusChip::kind::waiting);
    result_->setAccessibleName(tr("Benchmark result: %1").arg(tr("Running")));
    finished_->setText(tr("%1 of %2 received")
                           .arg(vss::format_byte_size(0).c_str(),
                                vss::format_byte_size(options.bytes).c_str()));
    set_metrics(tr("Not run"), tr("Not run"), tr("Not run"), tr("Not run"));
    update_state();

    // The worker touches no widget: it calls the backend and posts plain
    // values back.  The destructor joins it before the dialog goes.
    worker_ = std::thread(
        [this, from, to, options]
        {
            const auto outcome = backend_.run_benchmark(
                from, to, options,
                [this](const vss::benchmark_progress& progress)
                {
                    QMetaObject::invokeMethod(
                        this, [this, progress] { on_progress(progress); },
                        Qt::QueuedConnection);
                },
                &cancel_);
            QMetaObject::invokeMethod(
                this, [this, outcome] { on_finished(outcome); },
                Qt::QueuedConnection);
        });
}

void BenchmarkDialog::on_progress(const vss::benchmark_progress& progress)
{
    if (!running_ || progress.total == 0)
        return;
    const std::uint64_t done = progress.sent + progress.received;
    const std::uint64_t whole = progress.total * 2;
    progress_->setValue(
        static_cast<int>(std::min<std::uint64_t>(100, done * 100 / whole)));
    finished_->setText(
        tr("%1 of %2 received")
            .arg(vss::format_byte_size(progress.received).c_str(),
                 vss::format_byte_size(progress.total).c_str()));
}

void BenchmarkDialog::on_finished(
    const vss::result<vss::benchmark_result>& outcome)
{
    VSS_PROFILE_ZONE("gui.advanced.benchmark.finished");
    if (worker_.joinable())
        worker_.join();
    running_ = false;

    const auto show = [this](const QString& text, StatusChip::kind kind)
    {
        result_->set_text(text);
        result_->set_kind(kind);
        result_->setAccessibleName(tr("Benchmark result: %1").arg(text));
    };

    if (!outcome)
    {
        show(tr("Failed"), StatusChip::kind::failed);
        finished_->setText(
            tr("Could not finish: %1").arg(error_text(outcome.error())));
        set_metrics(tr("Not run"), tr("Not run"), tr("Not run"), tr("Not run"));
        update_state();
        Q_EMIT message(finished_->text());
        return;
    }

    const vss::benchmark_result& ran = *outcome;
    const QString sent =
        QString::fromStdString(vss::format_byte_size(ran.sent));
    const QString received =
        QString::fromStdString(vss::format_byte_size(ran.received));
    const QString lost =
        QString::fromStdString(vss::format_byte_size(ran.lost()));
    const QString rate = QString::fromStdString(
        vss::format_throughput(ran.received, ran.elapsed));
    set_metrics(sent, received, lost, rate);

    if (ran.cancelled)
    {
        show(tr("Cancelled"), StatusChip::kind::closed);
        finished_->setText(tr("Cancelled"));
        update_state();
        Q_EMIT message(tr("Cancelled"));
        return;
    }

    progress_->setValue(100);
    show(ran.complete ? tr("Passed") : tr("Failed"),
         ran.complete ? StatusChip::kind::healthy : StatusChip::kind::failed);
    finished_->setText(
        tr("Finished at %1")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
    summary_ = tr("%1: sent %2, received %3, lost %4, throughput %5.")
                   .arg(result_->text(), sent, received, lost, rate);
    has_result_ = true;
    update_state();
    Q_EMIT message(summary_);
}

} // namespace vss::gui
