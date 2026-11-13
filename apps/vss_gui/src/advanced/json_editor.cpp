//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/json_editor.hpp"

#include "advanced/chrome.hpp"
#include "advanced/command_panel.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QClipboard>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextFormat>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace vss::gui
{
namespace
{

class LineNumberArea final : public QWidget
{
public:
    explicit LineNumberArea(JsonCodeEdit* editor)
        : QWidget(editor), editor_(editor)
    {
        setAccessibleName(tr("Line numbers"));
    }

    QSize sizeHint() const override
    {
        return QSize(editor_->gutter_width(), 0);
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        editor_->paint_gutter(event);
    }

private:
    JsonCodeEdit* editor_;
};

} // namespace

JsonCodeEdit::JsonCodeEdit(QWidget* parent) : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("topologyJsonEditor"));
    setAccessibleName(tr("Topology JSON editor"));
    setFont(Theme::instance().font(text_style::data));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(
        QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * 4.0);
    setPlaceholderText(tr("The setup document appears here."));

    gutter_ = new LineNumberArea(this);
    connect(this, &QPlainTextEdit::blockCountChanged, this,
            [this](int) { setViewportMargins(gutter_width(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this,
            &JsonCodeEdit::update_gutter);
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this]
            {
                highlight_error_line();
                if (gutter_ != nullptr)
                    gutter_->update();
            });
    setViewportMargins(gutter_width(), 0, 0, 0);
}

int JsonCodeEdit::gutter_width() const
{
    return std::max(
        44, fontMetrics().horizontalAdvance(QStringLiteral("9999")) + 16);
}

void JsonCodeEdit::set_error_line(int line)
{
    error_line_ = std::max(0, line);
    highlight_error_line();
    if (gutter_ != nullptr)
        gutter_->update();
}

void JsonCodeEdit::go_to_line(int line)
{
    if (line <= 0)
        return;
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    QTextCursor cursor(block);
    cursor.movePosition(QTextCursor::StartOfBlock);
    setTextCursor(cursor);
    centerCursor();
    setFocus(Qt::OtherFocusReason);
}

void JsonCodeEdit::highlight_error_line()
{
    QList<QTextEdit::ExtraSelection> selections;
    if (error_line_ > 0)
    {
        const QTextBlock block = document()->findBlockByNumber(error_line_ - 1);
        if (block.isValid())
        {
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(
                Theme::instance().colour(colour_role::badSoft));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = QTextCursor(block);
            selections.append(selection);
        }
    }
    setExtraSelections(selections);
}

void JsonCodeEdit::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect area = contentsRect();
    if (gutter_ != nullptr)
        gutter_->setGeometry(
            QRect(area.left(), area.top(), gutter_width(), area.height()));
}

void JsonCodeEdit::update_gutter(const QRect& rect, int dy)
{
    if (gutter_ == nullptr)
        return;
    if (dy != 0)
        gutter_->scroll(0, dy);
    else
        gutter_->update(0, rect.y(), gutter_->width(), rect.height());
    if (rect.contains(viewport()->rect()))
        setViewportMargins(gutter_width(), 0, 0, 0);
}

void JsonCodeEdit::paint_gutter(QPaintEvent* event)
{
    QPainter painter(gutter_);
    painter.fillRect(event->rect(),
                     Theme::instance().colour(colour_role::sunken));

    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    int top =
        qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    painter.setFont(font());

    while (block.isValid() && top <= event->rect().bottom())
    {
        if (block.isVisible() && bottom >= event->rect().top())
        {
            painter.setPen(Theme::instance().colour(colour_role::ink3));
            painter.drawText(0, top, gutter_->width() - 10,
                             fontMetrics().height(), Qt::AlignRight,
                             QString::number(number + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++number;
    }
}

TopologyJsonEditor::TopologyJsonEditor(StudioBackend& backend, QWidget* parent)
    : QWidget(parent), backend_(backend)
{
    build();
    load_from_backend();
}

void TopologyJsonEditor::build()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.build");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Toolbar across the top: document status, a hint, then the three actions.
    auto* toolbar = new QWidget(this);
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(12, 6, 12, 6);
    toolbar_layout->setSpacing(10);

    status_ = new StatusChip(tr("Valid, version 1"), StatusChip::kind::healthy,
                             toolbar);
    status_->setAccessibleName(tr("Document status"));
    toolbar_layout->addWidget(status_, 0, Qt::AlignVCenter);

    caption_ = new QLabel(
        tr("Unsaved edits are checked as you type. Nothing changes until you "
           "apply."),
        toolbar);
    caption_->setFont(Theme::instance().font(text_style::caption));
    caption_->setProperty("muted", true);
    caption_->setWordWrap(true);
    toolbar_layout->addWidget(caption_, 1);
    toolbar_layout->addStretch(0);

    // Revert, Format and Apply use the small button size; Apply is primary.
    revert_ = make_button(tr("Revert"), nullptr, toolbar);
    revert_->setProperty("size", QStringLiteral("small"));
    polish(revert_);
    revert_->setAccessibleName(tr("Revert"));
    connect(revert_, &QPushButton::clicked, this, &TopologyJsonEditor::revert);
    toolbar_layout->addWidget(revert_);

    format_ = make_button(tr("Format"), nullptr, toolbar);
    format_->setProperty("size", QStringLiteral("small"));
    polish(format_);
    format_->setAccessibleName(tr("Format"));
    connect(format_, &QPushButton::clicked, this, &TopologyJsonEditor::format);
    toolbar_layout->addWidget(format_);

    apply_ = make_button(tr("Apply changes"), "primary", toolbar);
    apply_->setProperty("size", QStringLiteral("small"));
    polish(apply_);
    apply_->setAccessibleName(tr("Apply changes"));
    connect(apply_, &QPushButton::clicked, this,
            [this] { static_cast<void>(apply()); });
    toolbar_layout->addWidget(apply_);
    layout->addWidget(toolbar);

    // Body: the editor takes the spare width, with a side panel to its right.
    auto* body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);

    // Every edit schedules a re-validation (see schedule_validation()).
    editor_ = new JsonCodeEdit(this);
    connect(editor_, &QPlainTextEdit::textChanged, this,
            &TopologyJsonEditor::schedule_validation);
    body->addWidget(editor_, 1);

    // Side panel: the command the topology runs with, the validation checks
    // and an error box.  The fixed minimum width stops it being squeezed.
    auto* side = new QFrame(this);
    side->setObjectName(QStringLiteral("jsonSidePanel"));
    side->setFrameShape(QFrame::NoFrame);
    side->setAttribute(Qt::WA_StyledBackground);
    side->setMinimumWidth(380);
    auto* side_layout = new QVBoxLayout(side);
    side_layout->setContentsMargins(16, 16, 16, 16);
    side_layout->setSpacing(16);

    command_ = new CommandPanel(tr("Run it without the window"), side);
    connect(command_, &CommandPanel::export_requested, this,
            [this] { static_cast<void>(export_file()); });
    connect(command_, &CommandPanel::message, this,
            &TopologyJsonEditor::message);
    side_layout->addWidget(command_);

    // The check rows are not built here; the validation pass fills
    // checks_holder_ each time it runs.
    auto* checks_box = new QWidget(side);
    auto* checks_layout = new QVBoxLayout(checks_box);
    checks_layout->setContentsMargins(0, 0, 0, 0);
    checks_layout->setSpacing(6);
    auto* checks_title = new QLabel(tr("Checks"), checks_box);
    checks_title->setFont(Theme::instance().font(text_style::caption));
    checks_title->setProperty("muted", true);
    checks_layout->addWidget(checks_title);
    checks_holder_ = new QWidget(checks_box);
    auto* holder_layout = new QVBoxLayout(checks_holder_);
    holder_layout->setContentsMargins(0, 0, 0, 0);
    holder_layout->setSpacing(6);
    checks_layout->addWidget(checks_holder_);
    side_layout->addWidget(checks_box);

    // The error panel stays hidden until validation or Apply reports a
    // problem.
    error_panel_ = build_error_panel();
    side_layout->addWidget(error_panel_);
    side_layout->addStretch(1);
    body->addWidget(side, 0);
    layout->addLayout(body, 1);

    // The side panel's background is a palette colour, so set it now and
    // again whenever the theme changes.
    repaint_on_theme_change(this);
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [side]
            {
                QPalette palette = side->palette();
                palette.setColor(QPalette::Window,
                                 Theme::instance().colour(colour_role::panel));
                side->setPalette(palette);
            });
    {
        QPalette palette = side->palette();
        palette.setColor(QPalette::Window,
                         Theme::instance().colour(colour_role::panel));
        side->setPalette(palette);
    }
}

QWidget* TopologyJsonEditor::build_error_panel()
{
    auto* panel = new QFrame(this);
    panel->setProperty("note", QStringLiteral("danger"));
    polish(panel);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    error_summary_ = new QLabel(panel);
    QFont summary_font = Theme::instance().font(text_style::body);
    summary_font.setWeight(QFont::DemiBold);
    error_summary_->setFont(summary_font);
    error_summary_->setWordWrap(true);
    layout->addWidget(error_summary_);

    error_detail_ = new QLabel(panel);
    error_detail_->setFont(Theme::instance().font(text_style::caption));
    error_detail_->setWordWrap(true);
    layout->addWidget(error_detail_);

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    go_to_line_ = make_button(QString(), nullptr, panel);
    go_to_line_->setProperty("size", QStringLiteral("small"));
    polish(go_to_line_);
    connect(go_to_line_, &QPushButton::clicked, this,
            [this]
            {
                if (!validation_.problem)
                    return;
                editor_->go_to_line(validation_.problem->line);
            });
    row->addWidget(go_to_line_);
    fix_ = make_button(QString(), "quiet", panel);
    fix_->setProperty("size", QStringLiteral("small"));
    polish(fix_);
    connect(fix_, &QPushButton::clicked, this,
            [this]
            {
                if (!validation_.problem || !validation_.problem->has_fix())
                    return;
                QString current = editor_->toPlainText();
                const QString from =
                    QStringLiteral("\"%1\"").arg(validation_.problem->fix_from);
                const QString to =
                    QStringLiteral("\"%1\"").arg(validation_.problem->fix_to);
                // Search from the reported line so an identical port name
                // earlier in the document is not replaced by mistake.
                qsizetype line_start = 0;
                for (int line = 1; line < validation_.problem->line; ++line)
                {
                    const qsizetype next =
                        current.indexOf(QLatin1Char('\n'), line_start);
                    if (next < 0)
                        break;
                    line_start = next + 1;
                }
                const qsizetype index = current.indexOf(from, line_start);
                if (index < 0)
                    return;
                current.replace(index, from.size(), to);
                editor_->setPlainText(current);
                revalidate();
            });
    row->addWidget(fix_);
    row->addStretch(1);
    layout->addLayout(row);
    panel->hide();
    return panel;
}

void TopologyJsonEditor::load_from_backend()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.load");
    const auto text = vss::config::to_json(backend_.snapshot(), true);
    if (!text)
    {
        Q_EMIT message(
            tr("Could not read the setup: %1").arg(error_text(text.error())));
        return;
    }
    baseline_ = QString::fromStdString(*text).trimmed();
    {
        const QSignalBlocker blocker(editor_);
        editor_->setPlainText(baseline_);
    }
    apply_error_summary_.clear();
    apply_error_detail_.clear();
    revalidate();
}

void TopologyJsonEditor::reload_if_clean()
{
    if (!applying_ && !is_dirty())
        load_from_backend();
}

void TopologyJsonEditor::set_document_path(const QString& path)
{
    path_ = path;
    update_command();
}

QString TopologyJsonEditor::text() const
{
    return editor_->toPlainText();
}

void TopologyJsonEditor::set_text(const QString& text)
{
    editor_->setPlainText(text);
    revalidate();
}

void TopologyJsonEditor::revalidate()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.revalidate");
    validation_ = validate_json_document(text(), backend_.caps().shared_bus,
                                         backend_.caps().half_duplex_bus);
    apply_error_summary_.clear();
    apply_error_detail_.clear();
    update_ui();
}

bool TopologyJsonEditor::is_dirty() const
{
    return text() != baseline_;
}

bool TopologyJsonEditor::can_apply() const
{
    return !applying_ && is_dirty() && validation_.valid();
}

void TopologyJsonEditor::revert()
{
    load_from_backend();
    Q_EMIT message(tr("Reverted to the saved setup."));
}

void TopologyJsonEditor::format()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.format");
    if (!validation_.json_valid)
        return;
    QString formatted = validation_.formatted;
    if (formatted.isEmpty())
    {
        const QJsonDocument document = QJsonDocument::fromJson(text().toUtf8());
        formatted = QString::fromUtf8(document.toJson(QJsonDocument::Indented))
                        .trimmed();
    }
    if (formatted.isEmpty())
        return;
    {
        const QSignalBlocker blocker(editor_);
        editor_->setPlainText(formatted);
    }
    revalidate();
}

bool TopologyJsonEditor::apply()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.apply");
    if (!can_apply())
        return false;
    applying_ = true;
    editor_->setReadOnly(true);
    apply_->setText(tr("Applying"));
    apply_->setAccessibleName(tr("Applying"));
    apply_->setEnabled(false);
    Q_EMIT message(
        tr("Setting up. Windows installs each new port, which can take up to "
           "20 seconds."));

    auto failure = std::make_shared<vss::error_code>();
    work_.start(
        [this, failure, document = validation_.document]
        {
            const auto result = backend_.apply(document, true);
            if (!result)
                *failure = result.error();
        },
        [this, failure]
        {
            applying_ = false;
            editor_->setReadOnly(false);
            apply_->setText(tr("Apply changes"));
            apply_->setAccessibleName(tr("Apply changes"));
            backend_.refresh();
            if (*failure)
            {
                show_apply_error(error_text(*failure));
                update_ui();
                return;
            }
            baseline_ = text();
            apply_error_summary_.clear();
            apply_error_detail_.clear();
            update_ui();
            Q_EMIT applied();
            Q_EMIT message(tr("Applied the changes."));
        });
    return true;
}

QVector<json_check> TopologyJsonEditor::checks() const
{
    return validation_.checks;
}

QString TopologyJsonEditor::error_summary() const
{
    if (!apply_error_summary_.isEmpty())
        return apply_error_summary_;
    return validation_.problem ? validation_.problem->summary : QString();
}

void TopologyJsonEditor::show_apply_error(const QString& text)
{
    apply_error_summary_ = tr("The setup could not be applied");
    apply_error_detail_ = text;
    update_error_panel();
    Q_EMIT message(apply_error_summary_);
}

void TopologyJsonEditor::schedule_validation()
{
    QTimer::singleShot(150, this, [this] { revalidate(); });
}

void TopologyJsonEditor::update_ui()
{
    const bool valid = validation_.valid();
    status_->set_text(valid ? tr("Valid, version 1")
                            : validation_.status_text());
    status_->set_kind(valid ? StatusChip::kind::healthy
                            : StatusChip::kind::failed);
    status_->setAccessibleName(
        tr("Document status: %1")
            .arg(valid ? tr("Valid, version 1") : validation_.status_text()));
    status_->setToolTip(valid ? tr("Valid, version 1")
                              : validation_.status_text());

    apply_->setEnabled(can_apply());
    revert_->setEnabled(is_dirty());
    format_->setEnabled(validation_.json_valid);
    command_->set_export_enabled(validation_.json_valid);
    update_checks();
    update_error_panel();
    update_command();
}

void TopologyJsonEditor::update_checks()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.update_checks");
    auto* layout = qobject_cast<QVBoxLayout*>(checks_holder_->layout());
    if (layout != nullptr)
    {
        while (QLayoutItem* item = layout->takeAt(0))
        {
            delete item->widget();
            delete item;
        }
    }
    else
    {
        layout = new QVBoxLayout(checks_holder_);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);
    }

    for (const json_check& check : validation_.checks)
    {
        auto* row = new QWidget(checks_holder_);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);
        auto* dot = new StateDot(row);
        if (check.state == check_state::passed)
        {
            dot->set_kind(StateDot::kind::filled);
            dot->set_colour(colour_role::ok);
        }
        else if (check.state == check_state::failed)
        {
            dot->set_kind(StateDot::kind::cross);
            dot->set_colour(colour_role::bad);
        }
        else
        {
            dot->set_kind(StateDot::kind::hollow);
            dot->set_colour(colour_role::ink3);
        }
        row_layout->addWidget(dot, 0, Qt::AlignTop);
        auto* label = new QLabel(check.text, row);
        label->setFont(Theme::instance().font(text_style::caption));
        label->setWordWrap(true);
        row_layout->addWidget(label, 1);
        row->setAccessibleName(check.text);
        layout->addWidget(row);
    }
}

void TopologyJsonEditor::update_error_panel()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.update_error_panel");
    const bool has_apply_error = !apply_error_summary_.isEmpty();
    const bool has_problem = validation_.problem.has_value();
    if (!has_apply_error && !has_problem)
    {
        error_panel_->hide();
        editor_->set_error_line(0);
        return;
    }

    const QString summary =
        has_apply_error ? apply_error_summary_ : validation_.problem->summary;
    const QString detail =
        has_apply_error ? apply_error_detail_ : validation_.problem->detail;
    error_summary_->setText(summary);
    error_detail_->setText(detail);
    error_detail_->setVisible(!detail.isEmpty());
    error_panel_->setAccessibleName(tr("Error: %1").arg(summary));

    const bool has_line = !has_apply_error && validation_.problem->has_line();
    go_to_line_->setVisible(has_line);
    if (has_line)
    {
        go_to_line_->setText(
            tr("Go to line %1").arg(validation_.problem->line));
        go_to_line_->setAccessibleName(go_to_line_->text());
        editor_->set_error_line(validation_.problem->line);
    }
    else
    {
        editor_->set_error_line(0);
    }

    const bool has_fix = !has_apply_error && validation_.problem->has_fix();
    fix_->setVisible(has_fix);
    if (has_fix)
    {
        fix_->setText(tr("Use \"%1\"").arg(validation_.problem->fix_to));
        fix_->setAccessibleName(fix_->text());
    }
    error_panel_->show();
}

QString TopologyJsonEditor::command_file_name() const
{
    if (!path_.isEmpty())
        return QFileInfo(path_).fileName();
    QString name = validation_.name;
    if (name.isEmpty())
        name = QStringLiteral("lab");
    if (!name.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        name += QStringLiteral(".json");
    return name;
}

void TopologyJsonEditor::update_command()
{
    command_->set_command(
        QStringLiteral("vss_cli run %1 --seconds=60").arg(command_file_name()));
}

void TopologyJsonEditor::copy_as_command()
{
    QGuiApplication::clipboard()->setText(command_->command());
    Q_EMIT message(tr("Copied the command."));
}

bool TopologyJsonEditor::export_file()
{
    VSS_PROFILE_ZONE("gui.advanced.json_editor.export_file");
    if (!validation_.json_valid)
    {
        Q_EMIT message(tr("Fix the JSON before exporting it."));
        return false;
    }
    const QString suggested = path_.isEmpty() ? command_file_name() : path_;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export the setup"), suggested,
        tr("Virtual Serial Studio setups (*.json)"));
    if (path.isEmpty())
        return false;
    if (!path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        Q_EMIT message(
            tr("Could not export the setup: %1").arg(file.errorString()));
        return false;
    }
    file.write(text().toUtf8());
    if (!file.commit())
    {
        Q_EMIT message(
            tr("Could not export the setup: %1").arg(file.errorString()));
        return false;
    }
    path_ = path;
    update_command();
    Q_EMIT message(tr("Saved %1.").arg(QFileInfo(path).fileName()));
    return true;
}

} // namespace vss::gui
