//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every language has a catalogue, every catalogue has every source string, no
// translation is unfinished or empty, and the placeholders match the source.
//

#include "test_framework.hpp"

#include "language/language_choice.hpp"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>

namespace
{

struct message
{
    QString context;
    QString source;
    QString comment;
    bool numerus = false;
    bool unfinished = false;
    bool vanished = false;
    QStringList forms;
};

struct catalogue
{
    QString language;
    QVector<message> messages;
};

QString translations_path(const QString& code)
{
    return QStringLiteral(VSS_TRANSLATIONS_DIR "/vss_gui_%1.ts").arg(code);
}

catalogue read_catalogue_file(const QString& path)
{
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    QXmlStreamReader xml(&file);
    catalogue result;
    QString context;
    message current;
    while (!xml.atEnd())
    {
        xml.readNext();
        if (xml.isStartElement())
        {
            const QStringView name = xml.name();
            if (name == QLatin1String("TS"))
                result.language = xml.attributes()
                                      .value(QLatin1String("language"))
                                      .toString();
            else if (name == QLatin1String("name"))
                context = xml.readElementText();
            else if (name == QLatin1String("message"))
            {
                current = {};
                current.context = context;
                current.numerus =
                    xml.attributes().value(QLatin1String("numerus")) ==
                    QLatin1String("yes");
            }
            else if (name == QLatin1String("source"))
                current.source = xml.readElementText();
            else if (name == QLatin1String("comment"))
                current.comment = xml.readElementText();
            else if (name == QLatin1String("translation"))
            {
                const QStringView type =
                    xml.attributes().value(QLatin1String("type"));
                current.unfinished = type == QLatin1String("unfinished");
                current.vanished = type == QLatin1String("vanished") ||
                                   type == QLatin1String("obsolete");
                if (!current.numerus)
                    current.forms << xml.readElementText();
            }
            else if (name == QLatin1String("numerusform"))
                current.forms << xml.readElementText();
        }
        else if (xml.isEndElement() && xml.name() == QLatin1String("message"))
            result.messages.push_back(current);
    }
    REQUIRE(!xml.hasError());
    return result;
}

catalogue read_catalogue(const QString& code)
{
    return read_catalogue_file(translations_path(code));
}

/// The part of a context name after the last `::`. The catalogues keep the
/// short class name, and lupdate writes the qualified one.
QString short_context(const QString& context)
{
    return context.mid(context.lastIndexOf(QLatin1String("::")) < 0
                           ? 0
                           : context.lastIndexOf(QLatin1String("::")) + 2);
}

QString message_key(const message& entry)
{
    return short_context(entry.context) + QLatin1Char('') + entry.source +
           QLatin1Char('') + entry.comment;
}

QStringList placeholders(const QString& text)
{
    static const QRegularExpression pattern(QStringLiteral("%(?:L?\\d+|n|Ln)"));
    QStringList found;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext())
        found << matches.next().captured();
    found.sort();
    return found;
}

QStringList translated_languages()
{
    QStringList languages = vss::gui::supported_languages();
    languages.removeAll(QStringLiteral("en"));
    return languages;
}

} // namespace

TEST_CASE("translations: every language but English has a catalogue")
{
    for (const QString& code : translated_languages())
        REQUIRE(QFile::exists(translations_path(code)));
}

TEST_CASE("translations: no message is unfinished or empty")
{
    for (const QString& code : translated_languages())
    {
        const catalogue loaded = read_catalogue(code);
        REQUIRE(!loaded.messages.isEmpty());
        for (const message& entry : loaded.messages)
        {
            if (entry.unfinished || entry.forms.isEmpty())
                throw vss_test::failure(
                    (code + ": unfinished: " + entry.source).toStdString());
            for (const QString& form : entry.forms)
            {
                if (form.trimmed().isEmpty())
                    throw vss_test::failure(
                        (code + ": empty: " + entry.source).toStdString());
            }
        }
    }
}

TEST_CASE("translations: the placeholders match the source")
{
    for (const QString& code : translated_languages())
    {
        for (const message& entry : read_catalogue(code).messages)
        {
            const QStringList wanted = placeholders(entry.source);
            for (const QString& form : entry.forms)
            {
                const QStringList found = placeholders(form);
                bool same = found == wanted;
                if (entry.numerus)
                {
                    // A plural form may leave the number out (one, two) but
                    // may not add another placeholder.
                    same = std::all_of(found.begin(), found.end(),
                                       [&](const QString& item)
                                       { return wanted.contains(item); });
                }
                if (!same)
                    throw vss_test::failure(
                        (code + ": placeholders differ: " + entry.source)
                            .toStdString());
            }
        }
    }
}

TEST_CASE("translations: every catalogue holds the same source strings")
{
    QSet<QString> reference;
    bool first = true;
    for (const QString& code : translated_languages())
    {
        QSet<QString> keys;
        for (const message& entry : read_catalogue(code).messages)
            keys.insert(entry.context + QLatin1Char('\x1f') + entry.source +
                        QLatin1Char('\x1f') + entry.comment);
        if (first)
        {
            reference = keys;
            first = false;
        }
        else if (keys != reference)
        {
            throw vss_test::failure(
                (code + ": its strings differ from the first catalogue's")
                    .toStdString());
        }
    }
}

TEST_CASE("translations: a catalogue names the language it is for")
{
    for (const QString& code : translated_languages())
    {
        const QString language = read_catalogue(code).language;
        REQUIRE(language == code ||
                language.startsWith(code + QLatin1Char('_')));
    }
}

TEST_CASE("translations: right-to-left languages have catalogues")
{
    for (const char* code : {"he", "yi", "ar"})
        REQUIRE(QFile::exists(translations_path(QString::fromLatin1(code))));
}

TEST_CASE("translations: no catalogue keeps a message the sources dropped")
{
    for (const QString& code : translated_languages())
    {
        for (const message& entry : read_catalogue(code).messages)
        {
            if (entry.vanished)
                throw vss_test::failure(
                    (code + ": vanished or obsolete message, remove it: " +
                     entry.source)
                        .toStdString());
        }
    }
}

// lupdate reads the sources the way the build does and writes every string
// that reaches a catalogue. A string the catalogues lack shows in English in
// that language, so the test compares the two and names what is missing.
TEST_CASE("translations: every catalogue has every string in the sources")
{
    const QString fresh_path =
        QStringLiteral(VSS_I18N_SCRATCH_DIR "/sources.ts");
    QFile::remove(fresh_path);

    QProcess lupdate;
    lupdate.start(QStringLiteral(VSS_LUPDATE_EXE),
                  {QStringLiteral("@" VSS_I18N_SOURCE_LIST),
                   QStringLiteral("-no-obsolete"), QStringLiteral("-ts"),
                   fresh_path});
    REQUIRE(lupdate.waitForFinished(180000));
    REQUIRE_EQ(lupdate.exitCode(), 0);

    QSet<QString> wanted;
    for (const message& entry : read_catalogue_file(fresh_path).messages)
        wanted.insert(message_key(entry));
    REQUIRE(wanted.size() > 1000);

    for (const QString& code : translated_languages())
    {
        QSet<QString> held;
        for (const message& entry : read_catalogue(code).messages)
            held.insert(message_key(entry));
        QStringList missing;
        for (const QString& key : wanted)
        {
            if (!held.contains(key))
                missing << key.section(QLatin1Char(''), 0, 1)
                               .replace(QLatin1Char(''), QLatin1String(": "));
        }
        if (!missing.isEmpty())
        {
            missing.sort();
            throw vss_test::failure(
                (code + ": " + QString::number(missing.size()) +
                 " strings of the sources are not in the catalogue, such as " +
                 missing.front() +
                 ". Run the update_translations target and translate them "
                 "(docs/i18n.md)")
                    .toStdString());
        }
    }
}
