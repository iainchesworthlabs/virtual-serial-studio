//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_LANGUAGE_BIDI_HPP
#define VSS_GUI_LANGUAGE_BIDI_HPP

#include <QString>

class QWidget;

namespace vss::gui
{

/// U+2068 FIRST STRONG ISOLATE and U+2069 POP DIRECTIONAL ISOLATE.
inline constexpr char16_t first_strong_isolate = 0x2068;
inline constexpr char16_t pop_directional_isolate = 0x2069;

/// Puts a name that is not part of the sentence (a port name, a path such as
/// `loopback:logger` or `COM3`, a file name, an address) in a bidirectional
/// isolate, so that in a right-to-left sentence it keeps its own order and its
/// punctuation does not move to the other end. In a left-to-right layout the
/// text comes back unchanged, so English and every left-to-right language read
/// and copy exactly as before. Empty text and text already isolated come back
/// unchanged.
///
/// Use it where a name is placed into a translated sentence, not on the
/// sentence itself and not on text that is shown on its own.
[[nodiscard]] QString isolate_name(const QString& name);

/// The same for a name that is not shown in a layout the program controls, with
/// the direction given: true wraps the name.
[[nodiscard]] QString isolate_name(const QString& name, bool right_to_left);

/// Removes the isolates `isolate_name` added (and any others), for text that
/// goes to the clipboard, a file or a command.
[[nodiscard]] QString strip_isolates(const QString& text);

/// Makes a widget, and the widgets in it, lay out left to right whatever the
/// language is: for traffic, hex and ASCII bytes, times, addresses and paths,
/// which are read left to right in every language and whose columns and
/// offsets would otherwise mirror. Call it on the view, not on a whole window.
void keep_left_to_right(QWidget* widget);

} // namespace vss::gui

#endif
