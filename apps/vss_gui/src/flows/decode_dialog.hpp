//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include "flows/flow_dialog.hpp"
#include "support/plugin_parts.hpp"

#include <vector>

class QComboBox;
class QLabel;

namespace vss::gui
{

class NoteBanner;

/// "Decode <place>": chooses which decoder of which plugin reads the data of
/// one port (`docs/gui-design/docs/screens/11-decoding-and-triggers.md`,
/// "Decode a port or a link"). It only chooses; the window makes the instance.
/// Decoders that cannot be used now are listed disabled, and the banner says
/// why.
class DecodeDialog final : public FlowDialog
{
    Q_OBJECT

public:
    DecodeDialog(const QString& place_name,
                 std::vector<support::decoder_choice> choices,
                 QWidget* parent = nullptr);

    QComboBox* protocol_combo() const noexcept { return protocol_; }
    QPushButton* start_button() const noexcept { return primary_button(); }
    /// The decoder chosen, or null while none can be used.
    const support::decoder_choice* chosen() const;

private:
    void update_state();

    QString place_name_;
    std::vector<support::decoder_choice> choices_;
    QComboBox* protocol_ = nullptr;
    QLabel* from_ = nullptr;
    NoteBanner* note_ = nullptr;
};

} // namespace vss::gui
