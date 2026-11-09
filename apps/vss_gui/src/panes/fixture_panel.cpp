//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "panes/fixture_panel.hpp"

#include "panes/format.hpp"
#include "theme/theme.hpp"
#include "widgets/common.hpp"

#include <vss/core/profile.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace vss::gui
{
namespace
{

QLabel* caption(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setFont(Theme::instance().font(text_style::caption));
    label->setProperty("muted", true);
    polish(label);
    return label;
}

} // namespace

FixturePanel::FixturePanel(FixtureBehaviours& behaviours, QWidget* parent)
    : QFrame(parent), behaviours_(behaviours)
{
    build();
    connect(&behaviours_, &FixtureBehaviours::changed, this,
            [this](vss::endpoint_id id)
            {
                if (id == port_)
                    sync();
            });
}

void FixturePanel::build()
{
    VSS_PROFILE_ZONE("gui.panes.fixture.build");
    setProperty("card", true);
    polish(this);
    setAccessibleName(tr("Fixture behaviour"));

    // The card is a single column: echo toggle, a form grid, then the
    // generator button and a status line.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(8);
    layout->addWidget(caption(tr("What this fixture does"), this));

    echo_ = new QCheckBox(tr("Echo received bytes"), this);
    layout->addWidget(echo_);

    // Label and field pairs, one per row; the field column takes the slack.
    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
    int row = 0;

    grid->addWidget(caption(tr("Generator"), this), row, 0);
    kind_ = new QComboBox(this);
    kind_->setAccessibleName(tr("Generator kind"));
    kind_->addItem(tr("Text line"), static_cast<int>(generator_kind::text_line));
    kind_->addItem(tr("Fixed pattern"),
                   static_cast<int>(generator_kind::fixed_pattern));
    kind_->addItem(tr("Counter"), static_cast<int>(generator_kind::counter));
    kind_->addItem(tr("Random bytes"),
                   static_cast<int>(generator_kind::random_bytes));
    grid->addWidget(kind_, row++, 1);

    // The text and byte-count rows are shown or hidden to suit the chosen
    // generator kind (see update_field_visibility).
    text_label_ = caption(tr("Line"), this);
    grid->addWidget(text_label_, row, 0);
    text_ = new QLineEdit(this);
    text_->setAccessibleName(tr("Generator bytes"));
    text_->setFont(Theme::instance().font(text_style::data));
    grid->addWidget(text_, row++, 1);

    chunk_label_ = caption(tr("Bytes"), this);
    grid->addWidget(chunk_label_, row, 0);
    chunk_ = new QSpinBox(this);
    chunk_->setRange(1, 256);
    chunk_->setSuffix(tr(" bytes each time"));
    chunk_->setAccessibleName(tr("Bytes each time"));
    grid->addWidget(chunk_, row++, 1);

    grid->addWidget(caption(tr("Rate"), this), row, 0);
    rate_ = new QSpinBox(this);
    rate_->setRange(1, 50);
    rate_->setSuffix(tr(" per second"));
    rate_->setAccessibleName(tr("Sends per second"));
    grid->addWidget(rate_, row++, 1);
    layout->addLayout(grid);

    // The button text and status line are refreshed by sync(), so only
    // their styling is set here.
    generator_ = new QPushButton(tr("Start generator"), this);
    generator_->setProperty("role", QStringLiteral("primary"));
    polish(generator_);
    layout->addWidget(generator_);

    status_ = new QLabel(this);
    status_->setFont(Theme::instance().font(text_style::caption));
    status_->setWordWrap(true);
    status_->setProperty("muted", true);
    polish(status_);
    layout->addWidget(status_);

    // Edits are pushed to the behaviours object; syncing_ stops the echo of
    // our own sync() back into it. A failed set_echo reverts the checkbox.
    connect(echo_, &QCheckBox::toggled, this,
            [this](bool on)
            {
                if (syncing_ || port_ == 0)
                    return;
                if (!behaviours_.set_echo(port_, on))
                    sync();
            });
    connect(kind_, &QComboBox::currentIndexChanged, this,
            [this]
            {
                if (syncing_)
                    return;
                const auto kind =
                    static_cast<generator_kind>(kind_->currentData().toInt());
                update_field_visibility(kind);
                // One field serves both kinds, so each starts from its own
                // example and a line is never read as hex.
                if (kind == generator_kind::fixed_pattern)
                    text_->setText(QStringLiteral("55 AA"));
                else if (kind == generator_kind::text_line)
                    text_->setText(generator_config{}.text);
                apply_edits();
            });
    connect(text_, &QLineEdit::textEdited, this, [this] { apply_edits(); });
    connect(rate_, &QSpinBox::valueChanged, this,
            [this]
            {
                if (!syncing_)
                    apply_edits();
            });
    connect(chunk_, &QSpinBox::valueChanged, this,
            [this]
            {
                if (!syncing_)
                    apply_edits();
            });
    connect(generator_, &QPushButton::clicked, this,
            [this]
            {
                if (port_ != 0)
                    behaviours_.toggle_generator(port_);
            });
}

void FixturePanel::set_port(vss::endpoint_id id)
{
    port_ = id;
    sync();
}

QString FixturePanel::status_text() const
{
    return status_->text();
}

void FixturePanel::update_field_visibility(generator_kind kind)
{
    const bool has_text = kind == generator_kind::text_line ||
                          kind == generator_kind::fixed_pattern;
    const bool has_chunk = kind == generator_kind::counter ||
                           kind == generator_kind::random_bytes;
    text_label_->setVisible(has_text);
    text_->setVisible(has_text);
    chunk_label_->setVisible(has_chunk);
    chunk_->setVisible(has_chunk);
    if (kind == generator_kind::fixed_pattern)
    {
        text_label_->setText(tr("Pattern"));
        text_->setPlaceholderText(tr("Hex, such as 55 AA"));
    }
    else
    {
        text_label_->setText(tr("Line"));
        text_->setPlaceholderText(tr("Text, sent with CRLF"));
    }
}

void FixturePanel::apply_edits()
{
    VSS_PROFILE_ZONE("gui.panes.fixture.apply_edits");
    if (port_ == 0)
        return;
    generator_config config;
    config.kind = static_cast<generator_kind>(kind_->currentData().toInt());
    config.text = text_->text();
    config.per_second = rate_->value();
    config.chunk = chunk_->value();
    behaviours_.set_config(port_, config);
}

void FixturePanel::sync()
{
    VSS_PROFILE_ZONE("gui.panes.fixture.sync");
    const bool fixture = port_ != 0 && behaviours_.is_fixture(port_);
    setVisible(fixture);
    if (!fixture)
        return;

    syncing_ = true;
    const generator_config config = behaviours_.config(port_);
    const int kind_index =
        kind_->findData(static_cast<int>(config.kind));
    if (kind_index >= 0 && kind_->currentIndex() != kind_index)
        kind_->setCurrentIndex(kind_index);
    update_field_visibility(config.kind);
    if (text_->text() != config.text && !text_->hasFocus())
        text_->setText(config.text);
    rate_->setValue(config.per_second);
    chunk_->setValue(config.chunk);

    const bool generating = behaviours_.generating(port_);
    generator_->setText(generating ? tr("Stop generator")
                                   : tr("Start generator"));
    generator_->setProperty("role", generating ? QStringLiteral("secondary")
                                               : QStringLiteral("primary"));
    polish(generator_);
    const auto bytes = behaviours_.generated_bytes(port_);
    status_->setText(
        generating ? tr("Generating, %1 sent from this fixture.")
                         .arg(format_bytes(bytes))
                   : bytes > 0 ? tr("Stopped after %1.").arg(format_bytes(bytes))
                               : tr("Not running."));

    const bool echo_ok = behaviours_.echo_available();
    echo_->setEnabled(echo_ok);
    echo_->setToolTip(echo_ok ? QString() : tr("Not available in this build"));
    echo_->setChecked(behaviours_.echoing(port_));
    syncing_ = false;
}

} // namespace vss::gui
