//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "test_framework.hpp"

#include "language/language_choice.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QProcess>
#include <QGuiApplication>
#include <QStringList>
#include <QTranslator>

namespace
{

QStringList everything()
{
    return vss::gui::supported_languages();
}

} // namespace

TEST_CASE("language: any English is English")
{
    for (const char* tag : {"en", "en-GB", "en_AU", "en-Latn-US", "EN-us"})
    {
        const auto choice =
            vss::gui::choose_language({QString::fromLatin1(tag)}, everything());
        REQUIRE_EQ(choice.code, QStringLiteral("en"));
        REQUIRE_EQ(choice.matched, QString::fromLatin1(tag));
    }
}

TEST_CASE("language: the first preference with a catalogue wins")
{
    // A phone that lists English (UK), German and French gets English.
    REQUIRE_EQ(vss::gui::choose_language({QStringLiteral("en-GB"),
                                          QStringLiteral("de-DE"),
                                          QStringLiteral("fr")},
                                         everything())
                   .code,
               QStringLiteral("en"));
    // A preference without a catalogue is passed over.
    const auto choice = vss::gui::choose_language(
        {QStringLiteral("it-IT"), QStringLiteral("de-DE")},
        {QStringLiteral("en"), QStringLiteral("de")});
    REQUIRE_EQ(choice.code, QStringLiteral("de"));
    REQUIRE_EQ(choice.matched, QStringLiteral("de-DE"));
}

TEST_CASE("language: the old codes for Hebrew and Yiddish")
{
    REQUIRE_EQ(
        vss::gui::choose_language({QStringLiteral("iw-IL")}, everything()).code,
        QStringLiteral("he"));
    REQUIRE_EQ(
        vss::gui::choose_language({QStringLiteral("ji")}, everything()).code,
        QStringLiteral("yi"));
    REQUIRE_EQ(
        vss::gui::choose_language({QStringLiteral("ar-EG")}, everything()).code,
        QStringLiteral("ar"));
}

TEST_CASE("language: Chinese is Simplified unless the tag says Traditional")
{
    for (const char* tag : {"zh", "zh-CN", "zh-Hans-CN", "zh_SG"})
    {
        REQUIRE_EQ(
            vss::gui::choose_language({QString::fromLatin1(tag)}, everything())
                .code,
            QStringLiteral("zh_CN"));
    }
    for (const char* tag : {"zh-TW", "zh-Hant", "zh-HK", "zh-Hant-MO"})
    {
        const auto choice = vss::gui::choose_language(
            {QString::fromLatin1(tag), QStringLiteral("fr")}, everything());
        REQUIRE_EQ(choice.code, QStringLiteral("fr"));
    }
}

TEST_CASE("language: nothing that matches is English")
{
    const auto choice = vss::gui::choose_language(
        {QStringLiteral("sv-SE"), QStringLiteral("")}, everything());
    REQUIRE_EQ(choice.code, QStringLiteral("en"));
    REQUIRE(choice.matched.isEmpty());
    REQUIRE_EQ(vss::gui::choose_language({}, everything()).code,
               QStringLiteral("en"));
}

TEST_CASE("language: Hebrew, Yiddish and Arabic are right to left")
{
    for (const char* code : {"he", "yi", "ar"})
        REQUIRE(vss::gui::is_right_to_left(QString::fromLatin1(code)));
    for (const char* code : {"en", "fr", "es", "de", "ja", "zh_CN"})
        REQUIRE(!vss::gui::is_right_to_left(QString::fromLatin1(code)));
}

TEST_CASE("language: every language of the standard is supported")
{
    const QStringList expected{"en", "fr", "es",    "de", "he",
                               "yi", "ja", "zh_CN", "ar"};
    REQUIRE_EQ(vss::gui::supported_languages(), expected);
}

TEST_CASE("language: About can say what was chosen and what was offered")
{
    vss::gui::record_language_choice(
        {QStringLiteral("de"), QStringLiteral("de-AT")},
        {QStringLiteral("de-AT"), QStringLiteral("en")});
    const QString text = vss::gui::language_choice_description();
    REQUIRE(text.startsWith(QStringLiteral("de ")));
    REQUIRE(text.contains(QStringLiteral("from de-AT")));
    REQUIRE(text.contains(QStringLiteral("de-AT, en")));

    vss::gui::record_language_choice({QStringLiteral("en"), {}}, {});
    REQUIRE(vss::gui::language_choice_description().contains(
        QStringLiteral("nothing matched")));
}

TEST_CASE("language: without a catalogue the program stays English")
{
    // No catalogue is built into this test, so a request for German falls back
    // to English and the layout stays left to right.
    qputenv("VSS_LANGUAGE", QByteArrayLiteral("de"));
    const auto choice = vss::gui::apply_language();
    qunsetenv("VSS_LANGUAGE");
    REQUIRE_EQ(choice.code, QStringLiteral("en"));
    REQUIRE(QGuiApplication::layoutDirection() == Qt::LeftToRight);
}

TEST_CASE("language: the pseudo-language keeps placeholders and markup")
{
    const QString text = vss::gui::pseudo_localise(
        QStringLiteral("Open %1 and %L2 now, <b>%n item(s)</b> &Save 100%%"));
    REQUIRE(text.startsWith(QLatin1Char('[')));
    REQUIRE(text.endsWith(QLatin1Char(']')));
    for (const char* kept :
         {"%1", "%L2", "<b>", "%n", "</b>", "&", "100%%", "(", ")"})
        REQUIRE(text.contains(QLatin1String(kept)));
    // No plain letter of the words is left, so text that is not in tr() stands
    // out, and the text is longer, so a fixed width that clips shows.
    REQUIRE(!text.contains(QStringLiteral("Open")));
    REQUIRE(!text.contains(QStringLiteral("Save")));
    REQUIRE(text.size() > QStringLiteral("Open %1 now").size());
    REQUIRE_EQ(vss::gui::pseudo_localise(QString()), QStringLiteral("[]"));
}

TEST_CASE("language: pseudo and pseudo-rtl show every text accented")
{
    qputenv("VSS_LANGUAGE", QByteArrayLiteral("pseudo-rtl"));
    const auto choice = vss::gui::apply_language();
    qunsetenv("VSS_LANGUAGE");
    REQUIRE_EQ(choice.code, QStringLiteral("pseudo-rtl"));
    REQUIRE(QGuiApplication::layoutDirection() == Qt::RightToLeft);
    const QString shown = QCoreApplication::translate("Any", "Add a port");
    REQUIRE(shown != QStringLiteral("Add a port"));
    REQUIRE(shown == vss::gui::pseudo_localise(QStringLiteral("Add a port")));

    // Put the program back as the other tests expect it.
    const auto translators =
        QCoreApplication::instance()->findChildren<QTranslator*>();
    for (QTranslator* translator : translators)
        delete translator;
    QGuiApplication::setLayoutDirection(Qt::LeftToRight);
    REQUIRE_EQ(QCoreApplication::translate("Any", "Add a port"),
               QStringLiteral("Add a port"));
}

TEST_CASE("language: a catalogue answers for a qualified context")
{
    const QString qm = QStringLiteral(VSS_I18N_SCRATCH_DIR "/vss_gui_de.qm");
    QFile::remove(qm);
    QProcess lrelease;
    lrelease.start(QStringLiteral(VSS_LRELEASE_EXE),
                   {QStringLiteral(VSS_TRANSLATIONS_DIR "/vss_gui_de.ts"),
                    QStringLiteral("-qm"), qm});
    REQUIRE(lrelease.waitForFinished(120000));
    REQUIRE_EQ(lrelease.exitCode(), 0);

    QObject owner;
    QTranslator* translator = vss::gui::make_catalogue_translator(qm, &owner);
    REQUIRE(translator != nullptr);
    // Qt passes the class with its namespaces, the catalogue holds the name.
    REQUIRE_EQ(translator->translate("vss::gui::topology::Toolbox", "Pair"),
               QStringLiteral("Paar"));
    REQUIRE_EQ(translator->translate("Toolbox", "Pair"),
               QStringLiteral("Paar"));
    REQUIRE(translator->translate("vss::gui::topology::Toolbox",
                                  "Not a source text")
                .isNull());
    REQUIRE(vss::gui::make_catalogue_translator(
                QStringLiteral(VSS_I18N_SCRATCH_DIR "/missing.qm"), &owner) ==
            nullptr);
}
