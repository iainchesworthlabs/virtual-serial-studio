//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_LANGUAGE_LANGUAGE_CHOICE_HPP
#define VSS_GUI_LANGUAGE_LANGUAGE_CHOICE_HPP

#include <QString>
#include <QStringList>

class QObject;
class QTranslator;

namespace vss::gui
{

/// The languages the application is written for, by the code that names the
/// catalogue file (vss_gui_<code>.qm). English is the source language and the
/// fallback.
[[nodiscard]] QStringList supported_languages();

/// What was chosen and why.
struct language_choice
{
    /// One of the available languages.
    QString code;
    /// The tag of the preferred language it came from, as the system gave it,
    /// or empty when nothing matched and English is the fallback.
    QString matched;
};

/// Picks the language from the languages the person prefers, most wanted
/// first, as the system lists them (`en-GB`, `de-DE`, `zh-Hans-CN`, `en_AU`,
/// and the old `iw` and `ji` for Hebrew and Yiddish). The first preference that
/// has a catalogue wins, and it is matched by its language alone: any English
/// is English, any German is German. Chinese is Simplified (`zh_CN`) unless the
/// tag says Traditional (a Hant script, or Taiwan, Hong Kong or Macao), which
/// the application does not have and passes over. With nothing that matches,
/// English.
///
/// The choice is made here and not left to QTranslator and the system's list,
/// because what a system lists differs: some give `en-GB` and some give
/// `en-Latn-GB, en-GB, en-Latn, en`.
[[nodiscard]] language_choice choose_language(const QStringList& preferred,
                                              const QStringList& available);

/// True for the languages written right to left: Hebrew, Yiddish and Arabic.
[[nodiscard]] bool is_right_to_left(const QString& code);

/// Loads a compiled catalogue (.qm) as a translator that also answers for a
/// qualified context. Qt asks for the class name with its namespaces
/// (`vss::gui::topology::Toolbox`) while `lupdate` writes, and the catalogues
/// hold, the class name alone (`Toolbox`); this looks for the exact context
/// first and then for the part after the last `::`. Returns null when the file
/// cannot be loaded. The translator is owned by `parent`.
[[nodiscard]] QTranslator* make_catalogue_translator(const QString& path,
                                                     QObject* parent);

/// A source text as the pseudo-language shows it: every letter accented, 30
/// percent longer, in brackets so a clipped end is easy to see. Placeholders
/// (`%1`, `%n`, `%L1`), markup in `<...>` and the `&` of a mnemonic stay as
/// they are, so the text is still correct. It exists to find text that is not
/// in `tr()` (it stays plain English), text that clips when it grows, and
/// text that is glued together.
[[nodiscard]] QString pseudo_localise(const QString& source);

/// Remembers the choice made at start-up and what the system offered, for the
/// About window.
void record_language_choice(const language_choice& choice,
                            const QStringList& preferred);

/// The recorded choice as one line, or a line saying none was made.
[[nodiscard]] QString language_choice_description();

/// Chooses the language from the system's preferences (or from the
/// VSS_LANGUAGE environment variable, which names one language, or `pseudo`
/// or `pseudo-rtl` for the test languages of `pseudo_localise`), installs its
/// catalogue and sets the layout direction. Call it once, after the
/// QApplication exists and before any window is built.
language_choice apply_language();

} // namespace vss::gui

#endif // VSS_GUI_LANGUAGE_LANGUAGE_CHOICE_HPP
