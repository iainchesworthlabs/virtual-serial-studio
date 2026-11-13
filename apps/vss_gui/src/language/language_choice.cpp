//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "language/language_choice.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QTranslator>

#include <memory>
#include <string_view>

namespace vss::gui
{

namespace
{

QString g_description;
std::unique_ptr<QTranslator> g_translator;

/// The code of the catalogue that serves a tag, or an empty string when there
/// is none for it.
QString catalogue_for(const QString& tag, const QStringList& available)
{
    const QStringList parts =
        QString(tag).replace(u'_', u'-').split(u'-', Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    QString language = parts.front().toLower();
    // The old codes that systems still send for Hebrew and Yiddish.
    if (language == QStringLiteral("iw"))
        language = QStringLiteral("he");
    else if (language == QStringLiteral("ji"))
        language = QStringLiteral("yi");

    QString code = language;
    if (language == QStringLiteral("zh"))
    {
        // Simplified, the one there is, unless the tag says Traditional.
        for (const QString& part : parts)
        {
            const QString lower = part.toLower();
            if (lower == QStringLiteral("hant") ||
                lower == QStringLiteral("tw") ||
                lower == QStringLiteral("hk") || lower == QStringLiteral("mo"))
            {
                return {};
            }
        }
        code = QStringLiteral("zh_CN");
    }
    return available.contains(code) ? code : QString();
}

/// Shows every text of the program through pseudo_localise().
class pseudo_translator final : public QTranslator
{
public:
    using QTranslator::QTranslator;

    [[nodiscard]] QString translate(const char* context,
                                    const char* source_text,
                                    const char* disambiguation = nullptr,
                                    int n = -1) const override
    {
        static_cast<void>(context);
        static_cast<void>(disambiguation);
        static_cast<void>(n);
        if (source_text == nullptr)
            return {};
        return pseudo_localise(QString::fromUtf8(source_text));
    }

    [[nodiscard]] bool isEmpty() const override { return false; }
};

/// A catalogue that falls back from a qualified context to the class name.
class catalogue_translator final : public QTranslator
{
public:
    explicit catalogue_translator(QObject* parent) : QTranslator(parent) {}

    [[nodiscard]] bool load_file(const QString& path)
    {
        return inner_.load(path);
    }

    [[nodiscard]] QString translate(const char* context,
                                    const char* source_text,
                                    const char* disambiguation = nullptr,
                                    int n = -1) const override
    {
        QString found =
            inner_.translate(context, source_text, disambiguation, n);
        if (!found.isNull() || context == nullptr)
            return found;
        const std::string_view name(context);
        const std::size_t scope = name.rfind("::");
        if (scope == std::string_view::npos)
            return found;
        // name.data() is the whole context, which is null-terminated, so the
        // part after the scope is too.
        return inner_.translate(context + scope + 2, source_text,
                                disambiguation, n);
    }

    [[nodiscard]] bool isEmpty() const override { return inner_.isEmpty(); }

private:
    QTranslator inner_;
};

/// The application owns it through its Qt parent.
QTranslator* make_pseudo_translator(QObject* parent)
{
    return new pseudo_translator(parent);
}

QString catalogue_path(const QString& code)
{
    return QStringLiteral(":/i18n/vss_gui_%1.qm").arg(code);
}

} // namespace

QStringList supported_languages()
{
    return {
        QStringLiteral("en"), QStringLiteral("fr"),    QStringLiteral("es"),
        QStringLiteral("de"), QStringLiteral("he"),    QStringLiteral("yi"),
        QStringLiteral("ja"), QStringLiteral("zh_CN"), QStringLiteral("ar")};
}

language_choice choose_language(const QStringList& preferred,
                                const QStringList& available)
{
    VSS_PROFILE_ZONE("gui.language.choose");
    for (const QString& tag : preferred)
    {
        const QString code = catalogue_for(tag, available);
        if (!code.isEmpty())
            return {code, tag};
    }
    return {QStringLiteral("en"), {}};
}

bool is_right_to_left(const QString& code)
{
    return code == QStringLiteral("he") || code == QStringLiteral("yi") ||
           code == QStringLiteral("ar");
}

QTranslator* make_catalogue_translator(const QString& path, QObject* parent)
{
    auto* translator = new catalogue_translator(parent);
    if (translator->load_file(path))
        return translator;
    delete translator;
    return nullptr;
}

QString pseudo_localise(const QString& source)
{
    // Letters a to z, then A to Z, each as an accented look-alike.
    static const QString accented = QString::fromUtf16(
        u"åƀçðéƒĝĥîĵ"
        u"ķļḿñöþǫŕšţ"
        u"ûṽŵẋýž"
        u"ÅƁÇÐÉƑĜĤÎĴ"
        u"ĶĻḾÑÖÞǪŔŠŢ"
        u"ÛṼŴẊÝŽ");
    const auto accent = [](QChar ch) -> QChar
    {
        if (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
            return accented.at(ch.unicode() - u'a');
        if (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
            return accented.at(26 + ch.unicode() - u'A');
        return ch;
    };

    QString out;
    out.reserve(source.size() * 2 + 2);
    out += QLatin1Char('[');
    qsizetype letters = 0;
    qsizetype i = 0;
    while (i < source.size())
    {
        const QChar ch = source.at(i);
        if (ch == QLatin1Char('%') && i + 1 < source.size())
        {
            // %1, %L1, %n, %Ln and %% stay as they are.
            qsizetype end = i + 1;
            if (source.at(end) == QLatin1Char('L'))
                ++end;
            if (end < source.size() && source.at(end) == QLatin1Char('n'))
                ++end;
            else
            {
                while (end < source.size() && source.at(end).isDigit())
                    ++end;
                if (end == i + 1 && source.at(end) == QLatin1Char('%'))
                    ++end;
            }
            out += QStringView(source).mid(i, end - i);
            i = end;
            continue;
        }
        if (ch == QLatin1Char('<'))
        {
            const qsizetype close = source.indexOf(QLatin1Char('>'), i);
            if (close > i)
            {
                out += QStringView(source).mid(i, close - i + 1);
                i = close + 1;
                continue;
            }
        }
        const QChar changed = accent(ch);
        letters += changed != ch ? 1 : 0;
        out += changed;
        ++i;
    }
    out += QString(letters * 3 / 10, QLatin1Char('~'));
    out += QLatin1Char(']');
    return out;
}

void record_language_choice(const language_choice& choice,
                            const QStringList& preferred)
{
    g_description =
        QStringLiteral("%1 (%2; the system offers %3)")
            .arg(choice.code,
                 choice.matched.isEmpty()
                     ? QStringLiteral("nothing matched")
                     : QStringLiteral("from ") + choice.matched,
                 preferred.isEmpty() ? QStringLiteral("nothing")
                                     : preferred.join(QStringLiteral(", ")));
}

QString language_choice_description()
{
    return g_description.isEmpty() ? QStringLiteral("en (not chosen yet)")
                                   : g_description;
}

language_choice apply_language()
{
    VSS_PROFILE_ZONE("gui.language.apply");
    // Only a language whose catalogue is in the program can be chosen, so the
    // layout direction never mirrors English text.
    QStringList available{QStringLiteral("en")};
    for (const QString& code : supported_languages())
    {
        if (code != QStringLiteral("en") && QFile::exists(catalogue_path(code)))
            available.append(code);
    }

    QStringList preferred;
    const QString forced = qEnvironmentVariable("VSS_LANGUAGE");
    if (!forced.isEmpty())
        preferred.append(forced);
    else
        preferred = QLocale::system().uiLanguages();

    // The test languages are never chosen from the system, only by name.
    const bool pseudo = forced == QStringLiteral("pseudo") ||
                        forced == QStringLiteral("pseudo-rtl");
    const language_choice choice =
        pseudo ? language_choice{forced, QStringLiteral("VSS_LANGUAGE")}
               : choose_language(preferred, available);
    record_language_choice(choice, preferred);

    // The program keeps the translator for the rest of its life, and a second
    // call replaces the first.
    if (g_translator != nullptr)
        QCoreApplication::removeTranslator(g_translator.get());
    g_translator.reset();
    if (pseudo)
        g_translator.reset(make_pseudo_translator(nullptr));
    else if (choice.code != QStringLiteral("en"))
        g_translator.reset(
            make_catalogue_translator(catalogue_path(choice.code), nullptr));
    if (g_translator != nullptr)
        QCoreApplication::installTranslator(g_translator.get());

    const bool right_to_left = pseudo ? forced == QStringLiteral("pseudo-rtl")
                                      : is_right_to_left(choice.code);
    QGuiApplication::setLayoutDirection(right_to_left ? Qt::RightToLeft
                                                      : Qt::LeftToRight);
    return choice;
}

} // namespace vss::gui
