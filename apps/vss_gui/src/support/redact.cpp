//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "support/redact.hpp"

#include <vss/core/profile.hpp>

#include <QDir>
#include <QRegularExpression>

namespace vss::gui::support
{

QString redact_secrets(QString text)
{
    VSS_PROFILE_ZONE("gui.support.redact");
    // scheme://user:password@host -> scheme://[redacted]@host
    static const QRegularExpression credentials(
        QStringLiteral(R"((://)[^/\s:@]+:[^/\s@]+@)"));
    text.replace(credentials, QStringLiteral("\\1[redacted]@"));

    // Authorization: Bearer <token>.
    static const QRegularExpression bearer(
        QStringLiteral(R"((?i)\b(bearer)\s+[A-Za-z0-9._~+/\-=]+)"));
    text.replace(bearer, QStringLiteral("\\1 [redacted]"));

    // key=value, key: value, "key": "value" for the usual secret names.  The
    // replacement keeps a quoted value so a redacted JSON document stays
    // valid JSON.
    static const QRegularExpression secret_pair(QStringLiteral(
        R"((?i)\b(password|passwd|passphrase|token|secret|api[_-]?key|authorization)\b(["']?)\s*([:=])\s*("[^"]*"|'[^']*'|[^\s,;}\]]+))"));
    text.replace(secret_pair, QStringLiteral("\\1\\2\\3\"[redacted]\""));

    // The home directory identifies the person using the machine.
    const QString home = QDir::homePath();
    if (home.size() > 1 && home != QLatin1String("/"))
    {
        // Both ways the folder is written: Qt's, and the system's own (on
        // Windows a log or a path from another program has backslashes, and
        // the letter case of a path is not fixed there).
        const QString native = QDir::toNativeSeparators(home);
        if (native != home)
            text.replace(native, QStringLiteral("~"), Qt::CaseInsensitive);
        text.replace(home, QStringLiteral("~"),
                     native != home ? Qt::CaseInsensitive : Qt::CaseSensitive);
    }

    return text;
}

} // namespace vss::gui::support
