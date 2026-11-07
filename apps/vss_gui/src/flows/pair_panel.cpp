//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "flows/pair_panel.hpp"

#include "backend/driver_error.hpp"
#include "theme/theme.hpp"
#include "widgets/chips.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSet>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace vss::gui
{
namespace
{

QFrame* make_box(QWidget* parent, QLabel*& value, const QString& caption,
                 const QString& accessible_name)
{
    auto* box = new QFrame(parent);
    box->setObjectName(QStringLiteral("pairBox"));
    box->setMinimumWidth(150);
    box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(2);
    auto* label = new QLabel(caption, box);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);

    value = new QLabel(box);
    value->setFont(Theme::instance().font(text_style::subtitle));
    value->setAlignment(Qt::AlignCenter);
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    value->setAccessibleName(accessible_name);
    layout->addWidget(value);
    return box;
}

} // namespace

PairPanel::PairPanel(StudioBackend& backend, bool advanced,
                     platform_kind platform, QWidget* parent)
    : QWidget(parent), backend_(backend), platform_(platform),
      advanced_(advanced)
{
    build(advanced);
    refresh();
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this]
            {
                first_diagram_->setFont(
                    Theme::instance().font(text_style::subtitle));
                second_diagram_->setFont(
                    Theme::instance().font(text_style::subtitle));
            });
}

void PairPanel::build(bool advanced)
{
    VSS_PROFILE_ZONE("gui.flows.pair_panel.build");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    auto* diagram = new QFrame(this);
    diagram->setObjectName(QStringLiteral("pairDiagram"));
    auto* diagram_layout = new QHBoxLayout(diagram);
    diagram_layout->setContentsMargins(24, 20, 24, 20);
    diagram_layout->setSpacing(12);

    diagram_layout->addWidget(make_box(
        diagram, first_diagram_, tr("Program A opens"), tr("Program A opens")));

    auto* connector = new QWidget(diagram);
    connector->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto* connector_layout = new QVBoxLayout(connector);
    connector_layout->setContentsMargins(0, 0, 0, 0);
    connector_layout->setSpacing(6);
    auto* connector_text =
        new QLabel(tr("linked both ways, like a null-modem cable"), connector);
    connector_text->setFont(Theme::instance().font(text_style::caption));
    connector_text->setProperty("muted", true);
    connector_text->setWordWrap(true);
    connector_text->setAlignment(Qt::AlignCenter);
    connector_layout->addWidget(connector_text);
    auto* connector_line = new QFrame(connector);
    connector_line->setObjectName(QStringLiteral("pairConnector"));
    connector_line->setFixedHeight(2);
    connector_layout->addWidget(connector_line);
    connector_layout->addStretch(1);
    diagram_layout->addWidget(connector, 1);

    diagram_layout->addWidget(make_box(diagram, second_diagram_,
                                       tr("Program B opens"),
                                       tr("Program B opens")));
    layout->addWidget(diagram);

    pair_name_ = new QLineEdit(QStringLiteral("pair-1"), this);
    pair_name_->setObjectName(QStringLiteral("pairName"));
    pair_name_->setAccessibleName(tr("Pair name"));
    pair_name_row_ =
        new FormRow(tr("Pair name"), pair_name_,
                    tr("The name Studio uses for the two ports."), this);
    layout->addWidget(pair_name_row_);

    auto* paths = new QHBoxLayout();
    paths->setSpacing(12);
    first_port_ = new QLineEdit(this);
    first_port_->setObjectName(QStringLiteral("firstPort"));
    first_port_->setReadOnly(true);
    first_port_->setFont(Theme::instance().font(text_style::data));
    first_port_->setAccessibleName(tr("First port"));
    first_port_row_ = new FormRow(tr("First port"), first_port_, {}, this);
    paths->addWidget(first_port_row_, 1);

    second_port_ = new QLineEdit(this);
    second_port_->setObjectName(QStringLiteral("secondPort"));
    second_port_->setReadOnly(true);
    second_port_->setFont(Theme::instance().font(text_style::data));
    second_port_->setAccessibleName(tr("Second port"));
    second_port_row_ = new FormRow(tr("Second port"), second_port_, {}, this);
    paths->addWidget(second_port_row_, 1);
    layout->addLayout(paths);

    hint_ = new QLabel(
        tr("Names are suggested from free COM numbers. This build chooses "
           "the free ports shown above. On Linux and macOS the ports are "
           "paths such as /dev/pts/3 and are chosen for you."),
        this);
    hint_->setObjectName(QStringLiteral("pairHint"));
    hint_->setFont(Theme::instance().font(text_style::caption));
    hint_->setProperty("muted", true);
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    result_ = new ResultSentence(this);
    result_->setObjectName(QStringLiteral("pairResult"));
    layout->addWidget(result_);

    if (windows_platform(platform_))
    {
        // The design's screenshot shows the elevation warning.  This adapter
        // path claims two pairs the driver already exposes and never runs
        // setupc.exe, so the warning would be false.  The degradation is
        // recorded in docs/gui-design/design-deviations.md under WP5.
        admin_note_ = new NoteBanner(
            NoteBanner::kind::info,
            tr("Windows uses free com0com pairs for this pair, so it does not "
               "ask for administrator permission."),
            this);
        admin_note_->setObjectName(QStringLiteral("pairAdminNote"));
        layout->addWidget(admin_note_);
    }

    driver_note_ = new NoteBanner(NoteBanner::kind::warn, {}, this);
    driver_note_->setObjectName(QStringLiteral("pairDriverNote"));
    driver_note_->set_action(tr("Open setup check"),
                             [this] { Q_EMIT setup_check_requested(); });
    layout->addWidget(driver_note_);

    more_simple_ = new QFrame(this);
    more_simple_->setObjectName(QStringLiteral("pairMoreSimple"));
    auto* more_layout = new QHBoxLayout(more_simple_);
    more_layout->setContentsMargins(12, 10, 12, 10);
    more_layout->setSpacing(10);
    auto* more_title = new QLabel(tr("More options"), more_simple_);
    more_title->setFont(
        flow_semibold(Theme::instance().font(text_style::body)));
    more_layout->addWidget(more_title);
    more_layout->addWidget(new Badge(tr("ADVANCED"), more_simple_));
    auto* more_text = new QLabel(
        tr("Signal wiring and port kind. Turn on Advanced mode to change "
           "them."),
        more_simple_);
    more_text->setFont(Theme::instance().font(text_style::body));
    more_text->setProperty("muted", true);
    more_text->setWordWrap(true);
    more_layout->addWidget(more_text, 1);
    auto* more_button =
        new QPushButton(tr("Turn on Advanced mode"), more_simple_);
    more_button->setProperty("role", QStringLiteral("quiet"));
    more_button->setProperty("size", QStringLiteral("small"));
    more_button->setAccessibleName(tr("Turn on Advanced mode"));
    more_button->setCursor(Qt::PointingHandCursor);
    polish(more_button);
    connect(more_button, &QPushButton::clicked, this,
            &PairPanel::advanced_requested);
    more_layout->addWidget(more_button);
    layout->addWidget(more_simple_);

    more_advanced_ = new Disclosure(tr("More options"), this);
    more_advanced_->setObjectName(QStringLiteral("pairMoreAdvanced"));
    auto* advanced_content = new QWidget(more_advanced_);
    auto* advanced_layout = new QVBoxLayout(advanced_content);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    advanced_layout->setSpacing(12);

    wiring_ = new QComboBox(advanced_content);
    wiring_->setObjectName(QStringLiteral("pairWiring"));
    wiring_->addItem(tr("Standard null-modem"), true);
    wiring_->addItem(tr("Loopback"), false);
    wiring_->addItem(tr("Custom"), false);
    if (auto* model = qobject_cast<QStandardItemModel*>(wiring_->model()))
    {
        for (int index = 1; index <= 2; ++index)
        {
            if (QStandardItem* item = model->item(index))
            {
                item->setEnabled(false);
                item->setToolTip(tr("Not available in this build"));
            }
        }
    }
    wiring_->setAccessibleName(tr("Line wiring"));
    auto* wiring_row =
        new FormRow(tr("Line wiring"), wiring_,
                    tr("Custom wiring needs engine support that this build "
                       "does not have."),
                    advanced_content);
    advanced_layout->addWidget(wiring_row);

    kind_ = new QComboBox(advanced_content);
    kind_->setObjectName(QStringLiteral("pairKind"));
    kind_->addItem(tr("Best available (recommended)"));
    kind_->setAccessibleName(tr("Port kind"));
    kind_->setEnabled(false);
    kind_->setToolTip(tr("Not available in this build"));
    advanced_layout->addWidget(
        new FormRow(tr("Port kind"), kind_, {}, advanced_content));
    more_advanced_->set_content(advanced_content);
    layout->addWidget(more_advanced_);

    set_advanced(advanced);

    connect(wiring_, &QComboBox::currentIndexChanged, this,
            [this] { refresh_result(); });
    connect(&Theme::instance(), &Theme::theme_changed, this,
            [this, diagram, connector_line]
            {
                apply_dashed_frame(more_simple_);
                diagram->setStyleSheet(
                    QStringLiteral("QFrame#pairDiagram { background: %1; "
                                   "border: 1px solid %2; border-radius: 8px; "
                                   "}")
                        .arg(Theme::instance()
                                 .colour(colour_role::sunken)
                                 .name(QColor::HexRgb),
                             Theme::instance()
                                 .colour(colour_role::line)
                                 .name(QColor::HexRgb)));
                connector_line->setStyleSheet(
                    QStringLiteral("background: %1;")
                        .arg(Theme::instance()
                                 .colour(colour_role::ink2)
                                 .name(QColor::HexRgb)));
                const auto boxes =
                    diagram->findChildren<QFrame*>(QStringLiteral("pairBox"));
                for (QFrame* box : boxes)
                {
                    box->setStyleSheet(
                        QStringLiteral("QFrame#pairBox { background: %1; "
                                       "border: 1px solid %2; border-radius: "
                                       "6px; }")
                            .arg(Theme::instance()
                                     .colour(colour_role::panel)
                                     .name(QColor::HexRgb),
                                 Theme::instance()
                                     .colour(colour_role::line2)
                                     .name(QColor::HexRgb)));
                }
            });

    diagram->setStyleSheet(
        QStringLiteral(
            "QFrame#pairDiagram { background: %1; border: 1px solid %2; "
            "border-radius: 8px; }")
            .arg(Theme::instance()
                     .colour(colour_role::sunken)
                     .name(QColor::HexRgb),
                 Theme::instance()
                     .colour(colour_role::line)
                     .name(QColor::HexRgb)));
    connector_line->setStyleSheet(QStringLiteral("background: %1;")
                                      .arg(Theme::instance()
                                               .colour(colour_role::ink2)
                                               .name(QColor::HexRgb)));
    for (QFrame* box :
         diagram->findChildren<QFrame*>(QStringLiteral("pairBox")))
    {
        box->setStyleSheet(
            QStringLiteral("QFrame#pairBox { background: %1; border: 1px "
                           "solid %2; border-radius: 6px; }")
                .arg(Theme::instance()
                         .colour(colour_role::panel)
                         .name(QColor::HexRgb),
                     Theme::instance()
                         .colour(colour_role::line2)
                         .name(QColor::HexRgb)));
    }
    apply_dashed_frame(more_simple_);
}

void PairPanel::set_advanced(bool advanced)
{
    advanced_ = advanced;
    more_simple_->setVisible(!advanced);
    more_advanced_->setVisible(advanced);
    if (advanced && !more_advanced_->is_expanded())
        more_advanced_->set_expanded(true);
}

QVector<PairPanel::free_pair> PairPanel::free_pairs() const
{
    VSS_PROFILE_ZONE("gui.flows.pair_panel.free_pairs");
    QMap<int, std::pair<QString, QString>> ends;
    const auto devices = backend_.devices();
    for (const auto& device : devices)
    {
        if (!device.com0com || device.com0com_pair < 0)
            continue;
        auto& pair = ends[device.com0com_pair];
        if (device.com0com_side == QLatin1String("A"))
            pair.first = device.path;
        else if (device.com0com_side == QLatin1String("B"))
            pair.second = device.path;
    }

    QSet<QString> used;
    for (const auto& port : backend_.ports())
        used.insert(port.path);

    QVector<free_pair> out;
    for (auto it = ends.constBegin(); it != ends.constEnd(); ++it)
    {
        const QString& a = it.value().first;
        const QString& b = it.value().second;
        if (a.isEmpty() || b.isEmpty())
            continue;
        if (used.contains(a) || used.contains(b))
            continue;
        // The auto path attaches A and advertises B.  That is the free port
        // the second program can open.
        out.push_back(free_pair{b, a});
    }
    return out;
}

void PairPanel::refresh()
{
    VSS_PROFILE_ZONE("gui.flows.pair_panel.refresh");
    const setup_facts facts = backend_.facts();
    const bool windows = windows_platform(platform_);
    studio_driver_ = windows && facts.driver_found;
    driver_available_ = !windows || facts.driver_found || facts.com0com_found;
    const QVector<free_pair> pairs = free_pairs();
    free_pair_count_ = static_cast<int>(pairs.size());

    can_create_ = backend_.caps().links && driver_available_;
    // The Studio driver makes its ports when they are asked for, so only
    // com0com needs two free pairs to exist beforehand.
    if (windows && !studio_driver_)
        can_create_ = can_create_ && free_pair_count_ >= 2;

    if (admin_note_ != nullptr)
    {
        admin_note_->set_text(
            studio_driver_
                ? tr("The Studio driver makes the ports for this pair, so "
                     "Windows does not ask for administrator permission.")
                : tr("Windows uses free com0com pairs for this pair, so it "
                     "does not ask for administrator permission."));
    }
    if (hint_ != nullptr)
    {
        hint_->setText(
            studio_driver_
                ? tr("The Studio driver makes both ports when you create the "
                     "pair. Windows chooses their COM numbers.")
                : tr("Names are suggested from free COM numbers. This build "
                     "chooses the free ports shown above. On Linux and macOS "
                     "the ports are paths such as /dev/pts/3 and are chosen "
                     "for you."));
    }

    if (windows && !studio_driver_)
    {
        if (pairs.size() >= 1)
            first_port_->setText(pairs.at(0).first);
        else
        {
            first_port_->clear();
            first_port_->setPlaceholderText(
                tr("No free COM port for the first program"));
        }
        if (pairs.size() >= 2)
            second_port_->setText(pairs.at(1).first);
        else
        {
            second_port_->clear();
            second_port_->setPlaceholderText(
                tr("No free COM port for the second program"));
        }
    }
    else
    {
        first_port_->clear();
        first_port_->setPlaceholderText(tr("Chosen when you create the pair"));
        second_port_->clear();
        second_port_->setPlaceholderText(tr("Chosen when you create the pair"));
    }

    refresh_warning();
    refresh_result();
    Q_EMIT state_changed();
}

void PairPanel::refresh_warning()
{
    const bool windows = windows_platform(platform_);
    if (!backend_.caps().links)
    {
        driver_note_->set_text(
            tr("Linked pairs are not available in this build."));
        driver_note_->setVisible(true);
        driver_note_->clear_action();
    }
    else if (windows && !driver_available_)
    {
        driver_note_->set_text(tr("Linked pairs need a driver. Open the setup "
                                  "check."));
        driver_note_->setVisible(true);
    }
    else if (windows && !studio_driver_ && free_pair_count_ < 2)
    {
        driver_note_->set_text(
            tr("There are not enough free driver pairs. Open the setup "
               "check."));
        driver_note_->setVisible(true);
    }
    else
    {
        driver_note_->setVisible(false);
    }
}

void PairPanel::refresh_result()
{
    const QString first = first_port_->text().trimmed();
    const QString second = second_port_->text().trimmed();
    const bool wiring = wiring_ == nullptr || wiring_->currentData().toBool();
    if (!first.isEmpty() && !second.isEmpty())
    {
        if (wiring)
        {
            result_->set_result_text(
                tr("Anything <b>%1</b> sends arrives at <b>%2</b>, and the "
                   "other way round. DTR is wired to DSR and RTS to CTS, as "
                   "on a standard null-modem cable.")
                    .arg(first.toHtmlEscaped(), second.toHtmlEscaped()),
                tr("Anything %1 sends arrives at %2, and the other way round. "
                   "DTR is wired to DSR and RTS to CTS, as on a standard "
                   "null-modem cable.")
                    .arg(first, second));
        }
        else
        {
            result_->set_result_text(
                tr("Anything <b>%1</b> sends arrives at <b>%2</b>, and the "
                   "other way round. Modem lines are not crossed.")
                    .arg(first.toHtmlEscaped(), second.toHtmlEscaped()),
                tr("Anything %1 sends arrives at %2, and the other way round. "
                   "Modem lines are not crossed.")
                    .arg(first, second));
        }
        return;
    }

    result_->set_result_text(
        tr("Anything the first port sends arrives at the second, and the "
           "other way round. DTR is wired to DSR and RTS to CTS, as on a "
           "standard null-modem cable."));
}

bool PairPanel::validate()
{
    VSS_PROFILE_ZONE("gui.flows.pair_panel.validate");
    pair_name_row_->clear_error();
    first_port_row_->clear_error();
    second_port_row_->clear_error();
    if (pair_name_->text().trimmed().isEmpty())
    {
        pair_name_row_->set_error(tr("Enter a name for the pair."));
        pair_name_->setFocus(Qt::OtherFocusReason);
        return false;
    }

    const QString first = first_port_->text().trimmed();
    const QString second = second_port_->text().trimmed();
    if (first.isEmpty() || second.isEmpty())
        return true;

    QSet<QString> used;
    for (const auto& port : backend_.ports())
        used.insert(port.path);

    const auto alternative = [this, &used](const QString& current)
    {
        for (const free_pair& pair : free_pairs())
        {
            if (pair.first != current && !used.contains(pair.first))
                return pair.first;
        }
        return tr("another free COM port");
    };

    if (first == second)
    {
        const QString next = alternative(first);
        first_port_row_->set_error(
            tr("%1 is already used. Try %2.").arg(first, next));
        first_port_->setFocus(Qt::OtherFocusReason);
        return false;
    }

    // The fields are normally read-only and filtered to free pairs.  The
    // check still protects against a pair another process claimed while the
    // dialog was open.
    if (used.contains(first))
    {
        const QString next = alternative(first);
        first_port_row_->set_error(
            tr("%1 is already used. Try %2.").arg(first, next));
        first_port_->setFocus(Qt::OtherFocusReason);
        return false;
    }
    if (used.contains(second))
    {
        const QString next = alternative(second);
        second_port_row_->set_error(
            tr("%1 is already used. Try %2.").arg(second, next));
        second_port_->setFocus(Qt::OtherFocusReason);
        return false;
    }
    return true;
}

std::optional<pair_outcome> PairPanel::create_pair(flow_error* error)
{
    VSS_PROFILE_ZONE("gui.flows.pair_panel.create");
    if (error != nullptr)
        *error = {};
    if (!validate())
        return std::nullopt;

    if (!can_create_)
    {
        if (error != nullptr)
        {
            error->title = tr("Could not create the pair");
            error->body =
                windows_platform(platform_)
                    ? tr("Windows needs a serial driver before Studio "
                         "can create a pair another program can "
                         "open. Nothing was changed. Open the setup "
                         "check.")
                    : tr("This build cannot create a linked pair. "
                         "Nothing was changed.");
        }
        return std::nullopt;
    }

    const QString base = pair_name_->text().trimmed();
    const auto first =
        backend_.create_auto_port(base + QStringLiteral("-a"), true);
    if (!first)
    {
        if (error != nullptr)
        {
            const vss::error_code failure =
                driver_aware_error(backend_, first.error());
            error->title = tr("Could not create the pair");
            error->body = error_body(failure,
                                     tr("Studio could not create the first "
                                        "port. Nothing was changed."),
                                     tr(" Check the setup and try again."));
            error->details = error_details(failure);
            apply_driver_problem(*error, failure);
        }
        return std::nullopt;
    }

    const auto second =
        backend_.create_auto_port(base + QStringLiteral("-b"), true);
    if (!second)
    {
        static_cast<void>(backend_.remove_port(*first));
        if (error != nullptr)
        {
            const vss::error_code failure =
                driver_aware_error(backend_, second.error());
            error->title = tr("Could not create the pair");
            error->body = error_body(failure,
                                     tr("Studio could not create the second "
                                        "port. Nothing was changed."),
                                     tr(" Check the setup and try again."));
            error->details = error_details(failure);
            apply_driver_problem(*error, failure);
        }
        return std::nullopt;
    }

    const auto first_view = backend_.port(*first);
    const auto second_view = backend_.port(*second);
    if (!first_view || !second_view || first_view->studio_only ||
        second_view->studio_only)
    {
        static_cast<void>(backend_.remove_port(*first));
        static_cast<void>(backend_.remove_port(*second));
        if (error != nullptr)
        {
            error->title = tr("Could not create the pair");
            error->body = tr("This PC can no longer give the pair two ports "
                             "other programs can open. Nothing was changed. "
                             "Open the setup check.");
        }
        return std::nullopt;
    }

    vss::link_options options;
    options.emulate_modem_lines =
        wiring_ == nullptr || wiring_->currentData().toBool();
    const auto linked = backend_.connect_ports(*first, *second, options);
    if (!linked)
    {
        static_cast<void>(backend_.remove_port(*first));
        static_cast<void>(backend_.remove_port(*second));
        if (error != nullptr)
        {
            error->title = tr("Could not create the pair");
            error->body = error_body(linked.error(),
                                     tr("Studio could not link the two ports. "
                                        "Nothing was changed."),
                                     tr(" Check the setup and try again."));
            error->details = error_text(linked.error());
        }
        return std::nullopt;
    }

    pair_outcome outcome;
    outcome.first = *first;
    outcome.second = *second;
    outcome.link = *linked;
    outcome.first_path = first_view ? port_display_name(*first_view)
                                    : base + QStringLiteral("-a");
    outcome.second_path = second_view ? port_display_name(*second_view)
                                      : base + QStringLiteral("-b");
    backend_.refresh();
    return outcome;
}

void PairPanel::apply_dashed_frame(QFrame* frame)
{
    if (frame == nullptr)
        return;
    frame->setStyleSheet(
        QStringLiteral("QFrame#pairMoreSimple { background: %1; border: 1px "
                       "dashed %2; border-radius: 6px; }")
            .arg(Theme::instance()
                     .colour(colour_role::panel)
                     .name(QColor::HexRgb),
                 Theme::instance()
                     .colour(colour_role::control)
                     .name(QColor::HexRgb)));
}

} // namespace vss::gui
