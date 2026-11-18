//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "topology/add_sheet.hpp"

#include "theme/icon_provider.hpp"
#include "theme/theme.hpp"
#include "backend/com_numbers.hpp"
#include "topology/toolbox.hpp"
#include "widgets/common.hpp"
#include "widgets/controls.hpp"
#include "widgets/notes.hpp"

#include <vss/core/profile.hpp>

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QGuiApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace vss::gui::topology
{
namespace
{

/// The arrow of a collapsed section points the way the text reads on.
Qt::ArrowType collapsed_arrow()
{
    return QGuiApplication::layoutDirection() == Qt::RightToLeft
               ? Qt::LeftArrow
               : Qt::RightArrow;
}

constexpr int kKindNew = 0;
constexpr int kKindFixture = 1;
constexpr int kKindLoopback = 2;
constexpr int kKindExisting = 3;

struct framing
{
    unsigned data_bits = 8;
    vss::parity_mode parity{};
    vss::stop_bits_mode stop_bits{};
    const char* label = nullptr;
};

constexpr framing kFramings[] = {
    {8, vss::parity_mode::none, vss::stop_bits_mode::one, "8N1"},
    {7, vss::parity_mode::even, vss::stop_bits_mode::one, "7E1"},
    {7, vss::parity_mode::odd, vss::stop_bits_mode::one, "7O1"},
    {8, vss::parity_mode::even, vss::stop_bits_mode::one, "8E1"},
    {8, vss::parity_mode::none, vss::stop_bits_mode::two, "8N2"},
};

bool single_kind(canvas_item item)
{
    return item == canvas_item::port || item == canvas_item::fixture ||
           item == canvas_item::loopback;
}

} // namespace

QString slot_label(canvas_item item, int index, int slots)
{
    const int number = index + 1;
    switch (item)
    {
    case canvas_item::pair:
        return index == 0 ? AddSheet::tr("End A") : AddSheet::tr("End B");
    case canvas_item::split:
        return index == 0 ? AddSheet::tr("Source")
                          : AddSheet::tr("Target %1").arg(index);
    case canvas_item::join:
        return index == 0 ? AddSheet::tr("Target")
                          : AddSheet::tr("Source %1").arg(index);
    case canvas_item::switch_link:
        return index == 0 ? AddSheet::tr("Device")
                          : AddSheet::tr("Program %1").arg(index);
    case canvas_item::bus:
        return AddSheet::tr("Member %1").arg(number);
    case canvas_item::redirect:
        return index == 0 ? AddSheet::tr("From") : AddSheet::tr("To");
    case canvas_item::monitor:
        return index == 0 ? AddSheet::tr("Watched") : AddSheet::tr("Copy");
    case canvas_item::bench:
        return index % 2 == 0 ? AddSheet::tr("Fixture %1").arg(index / 2 + 1)
                              : AddSheet::tr("Port %1").arg(index / 2 + 1);
    case canvas_item::port:
    case canvas_item::fixture:
    case canvas_item::loopback:
    case canvas_item::device:
    case canvas_item::network:
    case canvas_item::share:
        break;
    }
    return slots == 1 ? AddSheet::tr("Name")
                      : AddSheet::tr("Name %1").arg(number);
}

AddSheet::AddSheet(canvas_item item, const std::vector<port_view>& ports,
                   QWidget* parent)
    : QDialog(parent), item_(item), ports_(ports)
{
    // The title, name and surface style make this a panel-style dialog; the
    // title doubles as the accessible name.
    const QString title = tr("Add %1").arg(item_label(item).toLower());
    setWindowTitle(title);
    setAccessibleName(title);
    setObjectName(QStringLiteral("canvasAddSheet"));
    setProperty("surface", QStringLiteral("panel"));
    polish(this);
    setMinimumWidth(340);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // Top to bottom: heading and description, the count (if it can vary), one
    // row per port, optional line settings, an error banner and the buttons.
    auto* heading = new QHBoxLayout();
    heading->setSpacing(8);
    auto* icon = new QLabel(this);
    icon->setPixmap(IconProvider::instance()
                        .icon(item_icon(item), colour_role::ink, 20)
                        .pixmap(20, 20));
    heading->addWidget(icon);
    auto* title_label = make_label(title, text_style::subtitle, this);
    heading->addWidget(title_label, 1);
    layout->addLayout(heading);
    auto* description =
        make_label(item_description(item), text_style::caption, this);
    description->setWordWrap(true);
    description->setProperty("muted", true);
    layout->addWidget(description);

    // The count box is only shown when the item can have a varying number of
    // ports. Changing it rebuilds the rows.
    if (minimum_count(item) != maximum_count(item))
    {
        count_ = new QSpinBox(this);
        count_->setObjectName(QStringLiteral("addSheetCount"));
        count_->setAccessibleName(tr("How many"));
        count_->setRange(minimum_count(item), maximum_count(item));
        count_->setValue(default_count(item));
        layout->addWidget(new FormRow(tr("How many"), count_, {}, this));
        connect(count_, &QSpinBox::valueChanged, this,
                [this](int value) { rebuild_rows(slot_count(item_, value)); });
    }

    // The rows are made and replaced by rebuild_rows(), so they get a host of
    // their own to be built into.
    rows_host_ = new QWidget(this);
    grid_ = new QGridLayout(rows_host_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(8);
    grid_->setVerticalSpacing(8);
    layout->addWidget(rows_host_);

    // The COM number row stays hidden until set_com_numbers() offers it.
    com_ = new QComboBox(this);
    com_->setObjectName(QStringLiteral("addSheetCom"));
    com_->setEditable(true);
    com_->setInsertPolicy(QComboBox::NoInsert);
    com_->setAccessibleName(tr("COM number"));
    com_->setValidator(new QRegularExpressionValidator(
        QRegularExpression(
            QStringLiteral("(?:[Cc][Oo][Mm])?[0-9]{0,3}|Automatic")),
        com_));
    com_row_ = new FormRow(tr("COM number"), com_, {}, this);
    com_row_->setVisible(false);
    layout->addWidget(com_row_);
    connect(com_, &QComboBox::currentTextChanged, this,
            [this] { update_state(); });

    // Baud rate and framing are rarely needed, so they sit behind a toggle
    // that is closed at first.
    more_ = new QToolButton(this);
    more_->setObjectName(QStringLiteral("addSheetMore"));
    more_->setText(tr("More settings"));
    more_->setCheckable(true);
    more_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more_->setArrowType(collapsed_arrow());
    layout->addWidget(more_);

    settings_ = new QWidget(this);
    auto* settings_layout = new QHBoxLayout(settings_);
    settings_layout->setContentsMargins(0, 0, 0, 0);
    baud_ = new QComboBox(settings_);
    baud_->setAccessibleName(tr("Baud rate"));
    baud_->setEditable(true);
    for (const int value : {9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600,
                                1000000, 2000000, 3000000, 4000000, 12000000})
        baud_->addItem(QString::number(value), value);
    baud_->setCurrentText(QStringLiteral("115200"));
    // The baud rate can be typed as well as chosen, within a sane range.
    baud_->setValidator(new QIntValidator(50, 12'000'000, baud_));
    framing_ = new QComboBox(settings_);
    framing_->setAccessibleName(tr("Framing"));
    for (const auto& value : kFramings)
        framing_->addItem(QString::fromLatin1(value.label));
    settings_layout->addWidget(new FormRow(tr("Baud rate"), baud_, {}, this));
    settings_layout->addWidget(new FormRow(tr("Framing"), framing_, {}, this));
    settings_->setVisible(false);
    layout->addWidget(settings_);
    settings_note_ = new QLabel(this);
    settings_note_->setObjectName(QStringLiteral("addSheetSettingsNote"));
    settings_note_->setWordWrap(true);
    settings_note_->setFont(Theme::instance().font(text_style::caption));
    settings_note_->setProperty("muted", true);
    settings_note_->hide();
    layout->addWidget(settings_note_);
    connect(more_, &QToolButton::toggled, this,
            [this](bool on)
            {
                settings_->setVisible(on);
                more_->setArrowType(on ? Qt::DownArrow : collapsed_arrow());
                adjustSize();
            });

    // The error banner stays hidden until show_error() has something to say.
    error_ = new NoteBanner(NoteBanner::kind::danger, {}, this);
    error_->setObjectName(QStringLiteral("addSheetError"));
    error_->hide();
    layout->addWidget(error_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"), this);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancel);
    // Add is the default button, so Enter adds; update_state() enables it.
    add_ = new QPushButton(tr("Add"), this);
    add_->setObjectName(QStringLiteral("addSheetAdd"));
    set_role(add_, "primary");
    add_->setDefault(true);
    connect(add_, &QPushButton::clicked, this, [this] { submit(); });
    buttons->addWidget(add_);
    layout->addLayout(buttons);

    // Make the first set of rows now that the widgets they update exist.
    rebuild_rows(slot_count(item, default_count(item)));
}

void AddSheet::rebuild_rows(int slots)
{
    VSS_PROFILE_ZONE("gui.topology.add_sheet.rebuild_rows");
    // Keep what the user typed in the rows that stay.
    QVector<bundle_slot> kept;
    for (const row& line : rows_)
    {
        bundle_slot slot;
        const int kind = line.kind->currentData().toInt();
        slot.kind = kind == kKindExisting   ? slot_kind::existing
                    : kind == kKindFixture  ? slot_kind::new_fixture
                    : kind == kKindLoopback ? slot_kind::new_loopback
                                            : slot_kind::new_port;
        slot.name = slot.kind == slot_kind::existing ? line.kind->currentText()
                                                     : line.name->text();
        kept.append(slot);
        delete line.label;
        delete line.kind;
        delete line.name;
    }
    rows_.clear();

    // Defaults for rows that did not exist before, plus the names already in
    // use so new ones do not clash with them.
    bundle_request fresh = default_request(
        item_, ports_,
        count_ != nullptr ? count_->value() : default_count(item_));
    QStringList taken;
    for (const bundle_slot& slot : kept)
        taken.append(slot.name);

    // Build one row per slot: label, kind chooser and name box. The kind
    // chooser lists the three new kinds, then every existing port.
    for (int index = 0; index < slots; ++index)
    {
        row line;
        line.label = new QLabel(slot_label(item_, index, slots), rows_host_);
        line.kind = new QComboBox(rows_host_);
        line.kind->setAccessibleName(
            tr("%1 kind").arg(slot_label(item_, index, slots)));
        line.kind->addItem(tr("New port"), kKindNew);
        line.kind->addItem(tr("New fixture"), kKindFixture);
        line.kind->addItem(tr("Loopback"), kKindLoopback);
        for (const port_view& port : ports_)
            line.kind->addItem(port.name, kKindExisting);
        line.name = new QLineEdit(rows_host_);
        line.name->setAccessibleName(
            tr("%1 name").arg(slot_label(item_, index, slots)));

        // A row that was already there keeps what it had; the others take
        // the default.
        bundle_slot slot =
            index < kept.size() ? kept[index] : fresh.slots.value(index);
        if (index >= kept.size())
        {
            // A new row gets a name nothing else has.
            slot.name = next_free_name(ports_,
                                       slot.kind == slot_kind::new_fixture
                                           ? QStringLiteral("fixture")
                                       : slot.kind == slot_kind::new_loopback
                                           ? QStringLiteral("loopback")
                                           : QStringLiteral("port"),
                                       taken);
            taken.append(slot.name);
        }
        // Select the matching kind: indexes 0 to 2 are the new kinds, in the
        // order added above, and an existing port is found by its name.
        switch (slot.kind)
        {
        case slot_kind::new_port:
            line.kind->setCurrentIndex(0);
            break;
        case slot_kind::new_fixture:
            line.kind->setCurrentIndex(1);
            break;
        case slot_kind::new_loopback:
            line.kind->setCurrentIndex(2);
            break;
        case slot_kind::existing:
            line.kind->setCurrentIndex(
                std::max(0, line.kind->findText(slot.name)));
            break;
        }
        if (slot.kind != slot_kind::existing)
            line.name->setText(slot.name);
        // Items that only ever make one kind of port have nothing to choose.
        line.kind->setVisible(!single_kind(item_));

        grid_->addWidget(line.label, index, 0);
        grid_->addWidget(line.kind, index, 1);
        grid_->addWidget(line.name, index, 2);
        rows_.append(line);

        // Changing a row's kind may rename it and show or hide its name box;
        // any change revalidates the whole sheet.
        const int position = index;
        connect(line.kind, &QComboBox::currentIndexChanged, this,
                [this, position]
                {
                    rename_default(position);
                    sync_name_visibility(position);
                    update_state();
                });
        connect(line.name, &QLineEdit::textChanged, this,
                [this] { update_state(); });
        sync_name_visibility(index);
    }
    // Let the name column take the width, then size the dialog to fit and put
    // focus on the first name.
    grid_->setColumnStretch(2, 1);
    update_state();
    adjustSize();
    if (!rows_.isEmpty())
        rows_.first().name->setFocus(Qt::OtherFocusReason);
}

void AddSheet::set_com_numbers(std::map<unsigned, com_use> uses)
{
    if (item_ != canvas_item::port)
        return;
    com_uses_ = std::move(uses);
    const QSignalBlocker block(com_);
    com_->clear();
    com_->addItem(tr("Automatic"));
    for (const unsigned candidate :
         free_com_numbers(com_uses_, 1, 48))
        com_->addItem(QStringLiteral("COM%1").arg(candidate));
    com_->setCurrentIndex(0);
    com_row_->set_hint(
        tr("A program that opens a fixed name such as COM3 needs the same "
           "number every time. Automatic picks a free one."));
    com_row_->setVisible(true);
    update_state();
    adjustSize();
}

unsigned AddSheet::com_number() const
{
    if (com_row_ == nullptr || com_row_->isHidden())
        return 0;
    QString text = com_->currentText().trimmed();
    if (text.startsWith(QLatin1String("com"), Qt::CaseInsensitive))
        text.remove(0, 3);
    bool ok = false;
    const unsigned value = text.toUInt(&ok);
    return ok ? value : 0U;
}

void AddSheet::rename_default(int index)
{
    if (index < 0 || index >= rows_.size())
        return;
    // A name the sheet made up follows the kind ("port-2" becomes
    // "fixture-1"); a name the user typed stays.
    static const QRegularExpression made_up(
        QStringLiteral("^(port|fixture|loopback)-\\d+$"));
    QLineEdit* name = rows_[index].name;
    if (!made_up.match(name->text()).hasMatch())
        return;
    const int kind = rows_[index].kind->currentData().toInt();
    const QString base = kind == kKindFixture    ? QStringLiteral("fixture")
                         : kind == kKindLoopback ? QStringLiteral("loopback")
                                                 : QStringLiteral("port");
    if (name->text().startsWith(base + QLatin1Char('-')))
        return;
    QStringList taken;
    for (int other = 0; other < rows_.size(); ++other)
    {
        if (other != index)
            taken.append(rows_[other].name->text());
    }
    name->setText(next_free_name(ports_, base, taken));
}

void AddSheet::sync_name_visibility(int index)
{
    if (index < 0 || index >= rows_.size())
        return;
    const bool existing =
        rows_[index].kind->currentData().toInt() == kKindExisting;
    rows_[index].name->setVisible(!existing);
}

void AddSheet::update_state()
{
    VSS_PROFILE_ZONE("gui.topology.add_sheet.update_state");
    const bundle_request value = request();
    QStringList names;
    bool ok = true;
    for (const bundle_slot& slot : value.slots)
    {
        if (slot.name.trimmed().isEmpty() || names.contains(slot.name))
            ok = false;
        names.append(slot.name);
        if (slot.kind != slot_kind::existing)
        {
            const bool taken = std::any_of(ports_.begin(), ports_.end(),
                                           [&slot](const port_view& port)
                                           { return port.name == slot.name; });
            if (taken)
                ok = false;
        }
    }
    if (com_row_ != nullptr && !com_row_->isHidden())
    {
        com_row_->clear_error();
        const unsigned number = com_number();
        const bool automatic =
            com_->currentText().trimmed().compare(tr("Automatic"),
                                                  Qt::CaseInsensitive) == 0 ||
            com_->currentText().trimmed().isEmpty();
        QString reason;
        if (!automatic && (number < 1 || number > 255))
            reason = tr("Choose a number from 1 to 255, or Automatic.");
        else if (const auto found = com_uses_.find(number);
                 !automatic && found != com_uses_.end())
            reason = tr("COM%1 is used by %2. Choose a free number.")
                         .arg(number)
                         .arg(com_use_text(found->second));
        if (!reason.isEmpty())
        {
            com_row_->set_error(reason);
            ok = false;
        }
    }
    if (add_ != nullptr)
        add_->setEnabled(ok);
    update_source_settings();
}

void AddSheet::update_source_settings()
{
    if (item_ != canvas_item::monitor || settings_note_ == nullptr ||
        rows_.isEmpty())
        return;
    const row& watched = rows_.first();
    const bool existing = watched.kind->currentData().toInt() == kKindExisting;
    const QString name = existing ? watched.kind->currentText() : QString();
    if (name == source_name_)
        return;
    source_name_ = name;
    const auto port =
        std::find_if(ports_.begin(), ports_.end(),
                     [&name](const port_view& view) { return view.name == name; });
    const bool follow = existing && port != ports_.end();
    baud_->setEnabled(!follow);
    framing_->setEnabled(!follow);
    settings_note_->setVisible(follow);
    if (!follow)
        return;
    // Show what the port really has, in the open, so it is not a hidden
    // default that differs from the port.
    const vss::serial_settings& have = port->serial;
    baud_->setCurrentText(QString::number(have.baud_rate));
    int index = -1;
    for (std::size_t each = 0; each < std::size(kFramings); ++each)
    {
        if (kFramings[each].data_bits == have.data_bits &&
            kFramings[each].parity == have.parity &&
            kFramings[each].stop_bits == have.stop_bits)
            index = static_cast<int>(each);
    }
    if (index < 0)
    {
        // A framing the list does not have is shown as it is.
        framing_->addItem(QString::fromStdString(vss::to_label(have)));
        index = framing_->count() - 1;
    }
    framing_->setCurrentIndex(index);
    settings_note_->setText(
        tr("Taken from %1. A monitor runs at the speed of the port it "
           "watches.")
            .arg(name));
    if (!more_->isChecked())
        more_->setChecked(true);
}

bool AddSheet::settings_editable() const
{
    return baud_->isEnabled() && framing_->isEnabled();
}

QString AddSheet::settings_note() const
{
    return settings_note_->isHidden() ? QString() : settings_note_->text();
}

bundle_request AddSheet::request() const
{
    bundle_request value;
    value.item = item_;
    for (const row& line : rows_)
    {
        bundle_slot slot;
        const int kind = line.kind->currentData().toInt();
        if (single_kind(item_))
            slot.kind = default_slot_kind(item_, 0);
        else if (kind == kKindExisting)
            slot.kind = slot_kind::existing;
        else if (kind == kKindFixture)
            slot.kind = slot_kind::new_fixture;
        else if (kind == kKindLoopback)
            slot.kind = slot_kind::new_loopback;
        else
            slot.kind = slot_kind::new_port;
        slot.name = slot.kind == slot_kind::existing
                        ? line.kind->currentText()
                        : line.name->text().trimmed();
        if (slot.kind == slot_kind::new_port && single_kind(item_))
            slot.com_number = com_number();
        value.slots.append(slot);
    }
    bool ok = false;
    const int baud = baud_->currentText().toInt(&ok);
    if (ok && baud > 0)
        value.settings.baud_rate = static_cast<unsigned>(baud);
    const int index = std::clamp(framing_->currentIndex(), 0,
                                 static_cast<int>(std::size(kFramings)) - 1);
    value.settings.data_bits = kFramings[index].data_bits;
    value.settings.parity = kFramings[index].parity;
    value.settings.stop_bits = kFramings[index].stop_bits;
    return value;
}

int AddSheet::count() const
{
    return count_ == nullptr ? default_count(item_) : count_->value();
}

void AddSheet::set_count(int value)
{
    if (count_ != nullptr)
        count_->setValue(value);
}

void AddSheet::set_slot_kind(int index, slot_kind kind, const QString& existing)
{
    if (index < 0 || index >= rows_.size())
        return;
    QComboBox* combo = rows_[index].kind;
    switch (kind)
    {
    case slot_kind::new_port:
        combo->setCurrentIndex(0);
        break;
    case slot_kind::new_fixture:
        combo->setCurrentIndex(1);
        break;
    case slot_kind::new_loopback:
        combo->setCurrentIndex(2);
        break;
    case slot_kind::existing:
        combo->setCurrentIndex(std::max(0, combo->findText(existing)));
        break;
    }
}

void AddSheet::set_slot_name(int index, const QString& name)
{
    if (index >= 0 && index < rows_.size())
        rows_[index].name->setText(name);
}

int AddSheet::slot_rows() const
{
    return static_cast<int>(rows_.size());
}

QString AddSheet::slot_row_label(int index) const
{
    return index >= 0 && index < rows_.size() ? rows_[index].label->text()
                                              : QString();
}

void AddSheet::show_error(const QString& text)
{
    error_->set_text(text);
    error_->clear_action();
    error_->setVisible(!text.isEmpty());
    adjustSize();
}

QString AddSheet::error_text() const
{
    return error_->isHidden() ? QString() : error_->text();
}

void AddSheet::submit()
{
    VSS_PROFILE_ZONE("gui.topology.add_sheet.submit");
    if (!add_->isEnabled())
        return;
    if (!attempt_)
    {
        accept();
        return;
    }
    const QString failure = attempt_(request());
    if (failure.isEmpty())
    {
        accept();
        return;
    }
    // Stay open with what was typed, say why, and put the cursor back in the
    // first name so the next try is one edit away.
    show_error(failure);
    if (setup_offered_ && setup_offered_())
    {
        error_->set_action(tr("Set up Virtual Serial Studio"),
                           [this]
                           {
                               reject();
                               Q_EMIT driver_setup_requested();
                           });
    }
    for (const row& line : rows_)
    {
        if (line.name->isVisibleTo(this))
        {
            line.name->setFocus(Qt::OtherFocusReason);
            line.name->selectAll();
            break;
        }
    }
}

} // namespace vss::gui::topology
