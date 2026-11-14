//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include "advanced/json_validation.hpp"

#include "backend/view_types.hpp"

#include <vss/core/profile.hpp>

#include <QCoreApplication>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cstddef>
#include <optional>

namespace vss::gui
{
namespace
{

QString ports_check_text(int count, bool unique)
{
    if (unique)
    {
        return count == 1
                   ? QCoreApplication::translate("TopologyJson",
                                                 "1 port, all names unique")
                   : QCoreApplication::translate("TopologyJson",
                                                 "%1 ports, all names unique")
                         .arg(count);
    }
    return count == 1
               ? QCoreApplication::translate("TopologyJson",
                                             "1 port, a name is used twice")
               : QCoreApplication::translate("TopologyJson",
                                             "%1 ports, a name is used twice")
                     .arg(count);
}

QString links_check_text(int count, bool no_loops)
{
    if (no_loops)
    {
        return count == 1 ? QCoreApplication::translate("TopologyJson",
                                                        "1 link, no loops")
                          : QCoreApplication::translate("TopologyJson",
                                                        "%1 links, no loops")
                                .arg(count);
    }
    return count == 1 ? QCoreApplication::translate("TopologyJson",
                                                    "1 link, a loop was found")
                      : QCoreApplication::translate(
                            "TopologyJson", "%1 links, a loop was found")
                            .arg(count);
}

int edit_distance(const QString& left, const QString& right)
{
    const int rows = static_cast<int>(left.size()) + 1;
    const int columns = static_cast<int>(right.size()) + 1;
    QVector<int> previous(columns);
    QVector<int> current(columns);
    for (int column = 0; column < columns; ++column)
        previous[column] = column;
    for (int row = 1; row < rows; ++row)
    {
        current[0] = row;
        for (int column = 1; column < columns; ++column)
        {
            const int cost = left.at(row - 1) == right.at(column - 1) ? 0 : 1;
            current[column] =
                std::min({previous[column] + 1, current[column - 1] + 1,
                          previous[column - 1] + cost});
        }
        previous = current;
    }
    return previous.last();
}

QString closest_mode(const QString& candidate)
{
    QString best;
    int best_distance = 3;
    for (const char* mode : vss::config::link_modes)
    {
        const QString name = QString::fromLatin1(mode);
        const int distance = edit_distance(candidate.toLower(), name);
        if (distance < best_distance)
        {
            best_distance = distance;
            best = name;
        }
    }
    return best;
}

/// Where every value of a syntactically valid document starts, by path.
///
/// A path joins object keys and array positions with `/`, for example
/// `ports/1/serial/baud_rate`.  The index lets a schema error point at the
/// exact line of the field that caused it, without changing the schema.  The
/// text has already passed `QJsonDocument`, so the scanner only has to follow
/// the structure; it still stops on anything unexpected rather than loop.
class path_index
{
public:
    explicit path_index(const QString& text) : text_(text)
    {
        value(QString(), 0);
    }

    /// The 1-based line of `path`, or of its nearest enclosing value, or 0.
    int line(const QString& path) const
    {
        QString current = path;
        while (!current.isEmpty())
        {
            const auto found = lines_.constFind(current);
            if (found != lines_.constEnd())
                return found.value();
            const qsizetype cut = current.lastIndexOf(QLatin1Char('/'));
            if (cut < 0)
                return 0;
            current.truncate(cut);
        }
        return 0;
    }

private:
    static constexpr int kMaxDepth = 256;

    bool at_end() const { return pos_ >= text_.size(); }

    void skip_space()
    {
        while (!at_end())
        {
            const QChar c = text_.at(pos_);
            if (c == QLatin1Char('\n'))
                ++line_;
            else if (!c.isSpace())
                return;
            ++pos_;
        }
    }

    QString read_string()
    {
        QString out;
        ++pos_;
        while (!at_end())
        {
            const QChar c = text_.at(pos_++);
            if (c == QLatin1Char('"'))
                break;
            if (c != QLatin1Char('\\') || at_end())
            {
                out += c;
                continue;
            }
            const QChar escape = text_.at(pos_++);
            if (escape == QLatin1Char('n'))
                out += QLatin1Char('\n');
            else if (escape == QLatin1Char('t'))
                out += QLatin1Char('\t');
            else if (escape == QLatin1Char('r'))
                out += QLatin1Char('\r');
            else if (escape == QLatin1Char('b'))
                out += QLatin1Char('\b');
            else if (escape == QLatin1Char('f'))
                out += QLatin1Char('\f');
            else if (escape == QLatin1Char('u'))
            {
                bool ok = false;
                const unsigned code = text_.mid(pos_, 4).toUInt(&ok, 16);
                if (ok)
                {
                    out += QChar(static_cast<char16_t>(code));
                    pos_ += 4;
                }
            }
            else
                out += escape;
        }
        return out;
    }

    void value(const QString& path, int depth)
    {
        skip_space();
        if (!path.isEmpty())
            lines_.insert(path, line_);
        if (at_end() || depth > kMaxDepth)
            return;

        const QChar first = text_.at(pos_);
        if (first == QLatin1Char('{'))
        {
            ++pos_;
            for (;;)
            {
                skip_space();
                if (at_end())
                    return;
                const QChar c = text_.at(pos_);
                if (c == QLatin1Char('}'))
                {
                    ++pos_;
                    return;
                }
                if (c == QLatin1Char(','))
                {
                    ++pos_;
                    continue;
                }
                if (c != QLatin1Char('"'))
                    return;
                const int key_line = line_;
                const QString key = read_string();
                skip_space();
                if (!at_end() && text_.at(pos_) == QLatin1Char(':'))
                    ++pos_;
                const QString child =
                    path.isEmpty() ? key : path + QLatin1Char('/') + key;
                value(child, depth + 1);
                lines_.insert(child, key_line);
            }
        }
        if (first == QLatin1Char('['))
        {
            ++pos_;
            int position = 0;
            for (;;)
            {
                skip_space();
                if (at_end())
                    return;
                const QChar c = text_.at(pos_);
                if (c == QLatin1Char(']'))
                {
                    ++pos_;
                    return;
                }
                if (c == QLatin1Char(','))
                {
                    ++pos_;
                    continue;
                }
                const qsizetype before = pos_;
                const QString child = (path.isEmpty() ? QString() : path +
                                       QLatin1Char('/')) +
                                      QString::number(position);
                value(child, depth + 1);
                if (pos_ == before)
                    return;
                ++position;
            }
        }
        if (first == QLatin1Char('"'))
        {
            read_string();
            return;
        }
        while (!at_end())
        {
            const QChar c = text_.at(pos_);
            if (c.isSpace() || c == QLatin1Char(',') || c == QLatin1Char(']') ||
                c == QLatin1Char('}'))
                return;
            ++pos_;
        }
    }

    const QString& text_;
    qsizetype pos_ = 0;
    int line_ = 1;
    QHash<QString, int> lines_;
};

/// `Line N: text`, or `text` alone when there is no line to name.
QString with_line(int line, const QString& text)
{
    if (line <= 0)
        return text;
    return QCoreApplication::translate("TopologyJson", "Line %1: %2")
        .arg(line)
        .arg(text);
}

/// What is wrong with the `half_duplex` member of one link: the path of the
/// member to mark, within the link, and the words.  `available` is
/// `capabilities::half_duplex_bus`.  The limits are the engine's own
/// constants.
struct half_duplex_issue
{
    QString member;
    QString summary;
    QString detail;
};

/// Checks the `half_duplex` member of one link and returns the first problem
/// found, or nothing if the member is absent or correct.  `mode` is the mode
/// of the link and `available` says whether this build has half-duplex buses.
/// The checks run from the general to the specific: whether the feature
/// exists, whether the link is a bus, whether the member is an object, and
/// then each field inside it in turn.  The first failure ends the check, so
/// the person sees one thing to fix at a time.
std::optional<half_duplex_issue> half_duplex_issue_of(const QJsonObject& link,
                                                      const QString& mode,
                                                      bool available)
{
    const QString key = QStringLiteral("half_duplex");
    if (!link.contains(key))
        return std::nullopt;
    if (!available)
    {
        return half_duplex_issue{
            key,
            QCoreApplication::translate(
                "TopologyJson",
                "Half-duplex buses are not available in this build"),
            QCoreApplication::translate(
                "TopologyJson",
                "This build cannot run a bus as a half-duplex line. Remove "
                "\"half_duplex\" from the link.")};
    }
    if (mode != QLatin1String("bus"))
    {
        return half_duplex_issue{
            key,
            QCoreApplication::translate("TopologyJson",
                                        "Only a bus can be a half-duplex line"),
            QCoreApplication::translate(
                "TopologyJson",
                "Remove \"half_duplex\", or make the link a bus.")};
    }
    const QJsonValue value = link.value(key);
    if (!value.isObject())
    {
        return half_duplex_issue{
            key,
            QCoreApplication::translate("TopologyJson",
                                        "\"half_duplex\" must be an object"),
            QCoreApplication::translate(
                "TopologyJson",
                "Use bit_rate, turnaround_ms, collisions and rts_gating "
                "inside it, each one optional.")};
    }

    const QJsonObject line = value.toObject();
    // A missing field is fine because every field is optional.  A present
    // field must be a number with no fraction, inside the given limits.  The
    // comparison with the truncated value is what rejects a fraction.
    const auto whole_number =
        [&line](const QString& member, qint64 minimum, qint64 maximum)
    {
        if (!line.contains(member))
            return true;
        const QJsonValue entry = line.value(member);
        const double number = entry.toDouble(-1);
        return entry.isDouble() && number >= static_cast<double>(minimum) &&
               number <= static_cast<double>(maximum) &&
               number == static_cast<double>(static_cast<qint64>(number));
    };
    const QString base = key + QLatin1Char('/');
    if (!whole_number(QStringLiteral("bit_rate"), vss::min_bus_bit_rate,
                      vss::max_bus_bit_rate))
    {
        return half_duplex_issue{
            base + QStringLiteral("bit_rate"),
            QCoreApplication::translate(
                "TopologyJson", "The line rate must be between %1 and %2 bit/s")
                .arg(vss::min_bus_bit_rate)
                .arg(vss::max_bus_bit_rate),
            QCoreApplication::translate(
                "TopologyJson",
                "Set \"bit_rate\" to a whole number in that range.")};
    }
    if (!whole_number(QStringLiteral("turnaround_ms"), 0,
                      vss::max_link_delay_ms))
    {
        return half_duplex_issue{
            base + QStringLiteral("turnaround_ms"),
            QCoreApplication::translate(
                "TopologyJson", "The turn-around must be between 0 and %1 ms")
                .arg(vss::max_link_delay_ms),
            QCoreApplication::translate(
                "TopologyJson",
                "Set \"turnaround_ms\" to a whole number in that range.")};
    }
    // The two fields left are not numbers: one is a choice of two words and
    // the other is a flag.
    const QString collisions = QStringLiteral("collisions");
    if (line.contains(collisions))
    {
        const QJsonValue entry = line.value(collisions);
        if (!entry.isString() || (entry.toString() != QLatin1String("report") &&
                                  entry.toString() != QLatin1String("garble")))
        {
            return half_duplex_issue{
                base + collisions,
                QCoreApplication::translate(
                    "TopologyJson", "\"collisions\" must be report or garble"),
                QCoreApplication::translate("TopologyJson",
                                            "Use report or garble.")};
        }
    }
    const QString gating = QStringLiteral("rts_gating");
    if (line.contains(gating) && !line.value(gating).isBool())
    {
        return half_duplex_issue{
            base + gating,
            QCoreApplication::translate("TopologyJson",
                                        "\"rts_gating\" must be true or false"),
            QCoreApplication::translate("TopologyJson", "Use true or false.")};
    }
    return std::nullopt;
}

bool engine_rejects_port(const QJsonObject& port)
{
    QJsonObject document;
    document.insert(QStringLiteral("version"), 1);
    document.insert(QStringLiteral("name"), QStringLiteral("check"));
    QJsonArray ports;
    ports.append(port);
    document.insert(QStringLiteral("ports"), ports);
    const QByteArray text =
        QJsonDocument(document).toJson(QJsonDocument::Compact);
    return !vss::config::from_json(text.toStdString());
}

/// The line of the field the engine refused, found by asking the engine.
///
/// `vss::config::from_json` returns only an error code.  To place it, each
/// port is checked on its own, then each serial setting of the failing port,
/// so the line comes from the engine's own rules and none of them is copied
/// here.  A link's delay and jitter are placed from the engine's limits.
/// Returns 0 when the cause is in neither.
int engine_problem_line(const QJsonObject& root, const path_index& index)
{
    const QJsonValue name = root.value(QStringLiteral("name"));
    if (name.isString() && name.toString().isEmpty())
        return index.line(QStringLiteral("name"));

    const QJsonArray ports = root.value(QStringLiteral("ports")).toArray();
    for (int position = 0; position < ports.size(); ++position)
    {
        const QJsonObject port = ports.at(position).toObject();
        if (!engine_rejects_port(port))
            continue;

        const QString base = QStringLiteral("ports/%1").arg(position);
        QJsonObject without_serial = port;
        without_serial.remove(QStringLiteral("serial"));
        if (!engine_rejects_port(without_serial))
        {
            const QJsonValue serial = port.value(QStringLiteral("serial"));
            if (serial.isObject())
            {
                const QJsonObject settings = serial.toObject();
                for (auto it = settings.constBegin(); it != settings.constEnd();
                     ++it)
                {
                    QJsonObject only;
                    only.insert(it.key(), it.value());
                    QJsonObject trial = port;
                    trial.insert(QStringLiteral("serial"), only);
                    if (engine_rejects_port(trial))
                        return index.line(base + QStringLiteral("/serial/") +
                                          it.key());
                }
            }
            return index.line(base + QStringLiteral("/serial"));
        }
        if (port.contains(QStringLiteral("uri")))
            return index.line(base + QStringLiteral("/uri"));
        return index.line(base);
    }

    // A link's delay and jitter: the limits are the engine's own constants.
    const QJsonArray links = root.value(QStringLiteral("links")).toArray();
    for (int position = 0; position < links.size(); ++position)
    {
        const QJsonObject link = links.at(position).toObject();
        const QString base = QStringLiteral("links/%1/").arg(position);
        const auto milliseconds = [&link](const QString& key, qint64& out)
        {
            if (!link.contains(key))
                return true;
            const QJsonValue value = link.value(key);
            const double number = value.toDouble(-1);
            if (!value.isDouble() || number < 0 ||
                number > static_cast<double>(vss::max_link_delay_ms) ||
                number != static_cast<double>(static_cast<qint64>(number)))
                return false;
            out = static_cast<qint64>(number);
            return true;
        };
        if (const auto issue =
                half_duplex_issue_of(link,
                                     link.value(QStringLiteral("mode"))
                                         .toString(QStringLiteral("duplex")),
                                     true))
            return index.line(base + issue->member);
        qint64 delay = 0;
        qint64 jitter = 0;
        if (!milliseconds(QStringLiteral("delay_ms"), delay))
            return index.line(base + QStringLiteral("delay_ms"));
        if (!milliseconds(QStringLiteral("jitter_ms"), jitter) ||
            jitter > delay)
            return index.line(base + QStringLiteral("jitter_ms"));
    }
    return 0;
}

bool reachable(const QHash<QString, QSet<QString>>& edges, const QString& from,
               const QString& to)
{
    if (from == to)
        return true;
    QSet<QString> visited;
    QVector<QString> pending{from};
    while (!pending.isEmpty())
    {
        const QString current = pending.takeLast();
        if (visited.contains(current))
            continue;
        visited.insert(current);
        const auto it = edges.constFind(current);
        if (it == edges.constEnd())
            continue;
        for (const QString& next : it.value())
        {
            if (next == to)
                return true;
            pending.push_back(next);
        }
    }
    return false;
}

} // namespace

int line_for_offset(const QString& text, int offset)
{
    const int limit = std::clamp(offset, 0, static_cast<int>(text.size()));
    int line = 1;
    for (int index = 0; index < limit; ++index)
    {
        if (text.at(index) == QLatin1Char('\n'))
            ++line;
    }
    return line;
}

QString json_validation::status_text() const
{
    if (valid())
        return QCoreApplication::translate("TopologyJson", "Valid, version 1");
    if (problem)
        return problem->summary;
    return QCoreApplication::translate("TopologyJson", "Not valid");
}

json_validation validate_json_document(const QString& text, bool shared_bus,
                                       bool half_duplex_bus)
{
    VSS_PROFILE_ZONE("gui.advanced.json_validate");
    // The checks run in stages and stop at the first problem.  Each stage
    // that fails returns three check rows: the stages before it passed and the
    // ones after it are not_checked.
    json_validation out;

    // Stage 1: syntax.  A parse failure is placed at the line of its offset.
    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(text.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError)
    {
        const int line =
            line_for_offset(text, static_cast<int>(parse_error.offset));
        out.checks = {
            {QCoreApplication::translate("TopologyJson",
                                         "Valid JSON, version 1"),
             check_state::failed},
            {QCoreApplication::translate("TopologyJson",
                                         "Ports, all names unique"),
             check_state::not_checked},
            {QCoreApplication::translate("TopologyJson", "Links, no loops"),
             check_state::not_checked}};
        out.problem = json_problem{
            QCoreApplication::translate("TopologyJson", "Line %1: %2")
                .arg(line)
                .arg(parse_error.errorString()),
            QCoreApplication::translate(
                "TopologyJson",
                "The line is highlighted and nothing was applied."),
            {},
            {},
            line};
        return out;
    }
    // From here on the text is known to be well-formed JSON.
    out.json_valid = true;

    // Stage 2: shape.  A setup file must have an object at the top level.
    if (!document.isObject())
    {
        out.checks = {
            {QCoreApplication::translate("TopologyJson",
                                         "Valid JSON, version 1"),
             check_state::failed},
            {QCoreApplication::translate("TopologyJson",
                                         "Ports, all names unique"),
             check_state::not_checked},
            {QCoreApplication::translate("TopologyJson", "Links, no loops"),
             check_state::not_checked}};
        out.problem = json_problem{
            QCoreApplication::translate(
                "TopologyJson", "Line 1: the document must be a JSON object"),
            QCoreApplication::translate(
                "TopologyJson",
                "A setup file starts with a { and ends with a }."),
            {},
            {},
            1};
        return out;
    }

    // Stage 3: version.  Only version 1 is understood; a missing or
    // non-numeric version reads as 0.  path_index maps a JSON path to the
    // line it starts on, so each problem can point at the text.
    const QJsonObject root = document.object();
    const path_index lines(text);
    const QJsonValue version_value = root.value(QStringLiteral("version"));
    out.version = version_value.isDouble() ? version_value.toInt() : 0;
    if (out.version != 1)
    {
        const int line = lines.line(QStringLiteral("version"));
        out.checks = {
            {QCoreApplication::translate("TopologyJson",
                                         "Valid JSON, version 1"),
             check_state::failed},
            {QCoreApplication::translate("TopologyJson",
                                         "Ports, all names unique"),
             check_state::not_checked},
            {QCoreApplication::translate("TopologyJson", "Links, no loops"),
             check_state::not_checked}};
        json_problem problem;
        problem.line = line;
        problem.summary =
            line > 0 ? QCoreApplication::translate("TopologyJson",
                                                   "Line %1: version must be 1")
                           .arg(line)
                     : QCoreApplication::translate(
                           "TopologyJson", "The document needs \"version\": 1");
        problem.detail =
            QCoreApplication::translate("TopologyJson", "Nothing was applied.");
        out.problem = problem;
        return out;
    }

    // The name is for display only; a missing or empty one reads "untitled".
    out.name =
        root.value(QStringLiteral("name")).toString(QStringLiteral("untitled"));
    if (out.name.isEmpty())
        out.name = QStringLiteral("untitled");

    // -- ports ------------------------------------------------------------
    QSet<QString> port_names;
    // Leaving "ports" out is allowed (an empty topology); a value that is not
    // a list is not.
    const QJsonValue ports_value = root.value(QStringLiteral("ports"));
    if (!ports_value.isUndefined() && !ports_value.isArray())
    {
        out.checks = {
            {QCoreApplication::translate("TopologyJson",
                                         "Valid JSON, version 1"),
             check_state::passed},
            {QCoreApplication::translate("TopologyJson",
                                         "Ports, all names unique"),
             check_state::failed},
            {QCoreApplication::translate("TopologyJson", "Links, no loops"),
             check_state::not_checked}};
        const int ports_line = lines.line(QStringLiteral("ports"));
        out.problem = json_problem{
            with_line(ports_line,
                      QCoreApplication::translate("TopologyJson",
                                                  "\"ports\" must be a list")),
            QCoreApplication::translate("TopologyJson", "Nothing was applied."),
            {},
            {},
            ports_line};
        return out;
    }

    // Check each port in order.  A failure here marks the "Ports" row failed.
    const QJsonArray ports = ports_value.toArray();
    for (int index = 0; index < ports.size(); ++index)
    {
        const QJsonValue value = ports.at(index);
        if (!value.isObject())
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::failed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::not_checked}};
            const int port_line =
                lines.line(QStringLiteral("ports/%1").arg(index));
            out.problem = json_problem{
                with_line(port_line,
                          QCoreApplication::translate(
                              "TopologyJson", "Each port must be an object")),
                QCoreApplication::translate("TopologyJson",
                                            "Nothing was applied."),
                {},
                {},
                port_line};
            return out;
        }
        // Links refer to ports by name, so every port needs a non-empty
        // string name and the names must not repeat.
        const QJsonObject port = value.toObject();
        const QJsonValue name_value = port.value(QStringLiteral("name"));
        const QString name = name_value.toString();
        if (!name_value.isString() || name.isEmpty())
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::failed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::not_checked}};
            json_problem problem;
            problem.line = lines.line(QStringLiteral("ports/%1").arg(index));
            problem.summary =
                problem.has_line()
                    ? QCoreApplication::translate("TopologyJson",
                                                  "Line %1: a port has no name")
                          .arg(problem.line)
                    : QCoreApplication::translate("TopologyJson",
                                                  "A port has no name");
            problem.detail = QCoreApplication::translate(
                "TopologyJson",
                "Every port needs a name so links can refer to it.");
            out.problem = problem;
            return out;
        }
        if (port_names.contains(name))
        {
            const int line =
                lines.line(QStringLiteral("ports/%1/name").arg(index));
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::failed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::not_checked}};
            json_problem problem;
            problem.line = line;
            problem.summary =
                line > 0 ? QCoreApplication::translate(
                               "TopologyJson",
                               "Line %1: the name \"%2\" is used twice")
                               .arg(line)
                               .arg(name)
                         : QCoreApplication::translate(
                               "TopologyJson", "The name \"%1\" is used twice")
                               .arg(name);
            problem.detail = QCoreApplication::translate(
                "TopologyJson",
                "Port names identify ports in links, so each name must be "
                "unique. Nothing was applied.");
            out.problem = problem;
            return out;
        }
        // Remember the name: a later duplicate and the links check it.
        port_names.insert(name);

        // The kind defaults to virtual and must be one of the known kinds.
        const QString kind = port.value(QStringLiteral("kind"))
                                 .toString(QStringLiteral("virtual"));
        bool kind_known = false;
        for (const char* candidate : vss::config::port_kinds)
        {
            if (kind == QLatin1String(candidate))
                kind_known = true;
        }
        if (!kind_known)
        {
            const int line =
                lines.line(QStringLiteral("ports/%1/kind").arg(index));
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::failed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::not_checked}};
            json_problem problem;
            problem.line = line;
            problem.summary =
                line > 0
                    ? QCoreApplication::translate(
                          "TopologyJson", "Line %1: unknown port kind \"%2\"")
                          .arg(line)
                          .arg(kind)
                    : QCoreApplication::translate("TopologyJson",
                                                  "Unknown port kind \"%1\"")
                          .arg(kind);
            problem.detail = QCoreApplication::translate(
                "TopologyJson",
                "Use virtual, physical, tcp, udp, rfc2217 or mock. Nothing was "
                "applied.");
            out.problem = problem;
            return out;
        }

        // virtual_kind picks how a virtual port is made and defaults to auto.
        // Only virtual ports are checked for it.
        const QString virtual_kind = port.value(QStringLiteral("virtual_kind"))
                                         .toString(QStringLiteral("auto"));
        if (kind == QLatin1String("virtual"))
        {
            bool virtual_known = false;
            for (const char* candidate : vss::config::virtual_port_kinds)
            {
                if (virtual_kind == QLatin1String(candidate))
                    virtual_known = true;
            }
            if (!virtual_known)
            {
                const int line = lines.line(
                    QStringLiteral("ports/%1/virtual_kind").arg(index));
                out.checks = {{QCoreApplication::translate(
                                   "TopologyJson", "Valid JSON, version 1"),
                               check_state::passed},
                              {QCoreApplication::translate(
                                   "TopologyJson", "Ports, all names unique"),
                               check_state::failed},
                              {QCoreApplication::translate("TopologyJson",
                                                           "Links, no loops"),
                               check_state::not_checked}};
                json_problem problem;
                problem.line = line;
                problem.summary =
                    line > 0
                        ? QCoreApplication::translate(
                              "TopologyJson",
                              "Line %1: unknown virtual kind \"%2\"")
                              .arg(line)
                              .arg(virtual_kind)
                        : QCoreApplication::translate(
                              "TopologyJson", "Unknown virtual kind \"%1\"")
                              .arg(virtual_kind);
                problem.detail = QCoreApplication::translate(
                    "TopologyJson",
                    "Use auto, pty, named-pipe, loopback or windows-driver. "
                    "Nothing was applied.");
                out.problem = problem;
                return out;
            }
        }

        // Virtual and mock ports are created by Studio and need no address;
        // every other kind opens something outside, so it needs a uri.
        if (kind != QLatin1String("virtual") && kind != QLatin1String("mock"))
        {
            const QString uri = port.value(QStringLiteral("uri")).toString();
            if (uri.isEmpty())
            {
                out.checks = {{QCoreApplication::translate(
                                   "TopologyJson", "Valid JSON, version 1"),
                               check_state::passed},
                              {QCoreApplication::translate(
                                   "TopologyJson", "Ports, all names unique"),
                               check_state::failed},
                              {QCoreApplication::translate("TopologyJson",
                                                           "Links, no loops"),
                               check_state::not_checked}};
                json_problem problem;
                problem.line = lines.line(QStringLiteral("ports/%1").arg(index));
                problem.summary =
                    problem.has_line()
                        ? QCoreApplication::translate(
                              "TopologyJson",
                              "Line %1: port \"%2\" has no address")
                              .arg(problem.line)
                              .arg(name)
                        : QCoreApplication::translate(
                              "TopologyJson", "Port \"%1\" has no address")
                              .arg(name);
                problem.detail = QCoreApplication::translate(
                    "TopologyJson",
                    "A physical or network port needs a uri. Nothing was "
                    "applied.");
                out.problem = problem;
                return out;
            }
        }
    }

    // -- links ------------------------------------------------------------
    const QJsonValue links_value = root.value(QStringLiteral("links"));
    if (!links_value.isUndefined() && !links_value.isArray())
    {
        out.checks = {
            {QCoreApplication::translate("TopologyJson",
                                         "Valid JSON, version 1"),
             check_state::passed},
            {QCoreApplication::translate("TopologyJson",
                                         "Ports, all names unique"),
             check_state::passed},
            {QCoreApplication::translate("TopologyJson", "Links, no loops"),
             check_state::failed}};
        const int links_line = lines.line(QStringLiteral("links"));
        out.problem = json_problem{
            with_line(links_line,
                      QCoreApplication::translate("TopologyJson",
                                                  "\"links\" must be a list")),
            QCoreApplication::translate("TopologyJson", "Nothing was applied."),
            {},
            {},
            links_line};
        return out;
    }

    // Record the counts for the status text.  edges holds the routes added
    // by the links seen so far (port name to the names it feeds), so a link
    // that would close a loop can be refused.  bus_members tracks ports that
    // are already on a bus.
    const QJsonArray links = links_value.toArray();
    out.ports = static_cast<int>(ports.size());
    out.links = static_cast<int>(links.size());
    QHash<QString, QSet<QString>> edges;
    QSet<QString> bus_members;
    for (int index = 0; index < links.size(); ++index)
    {
        const QJsonValue value = links.at(index);
        const int link_line =
            lines.line(QStringLiteral("links/%1").arg(index));
        if (!value.isObject())
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::failed}};
            json_problem problem;
            problem.line = link_line;
            problem.summary =
                link_line > 0
                    ? QCoreApplication::translate(
                          "TopologyJson",
                          "Line %1: each link must be an object")
                          .arg(link_line)
                    : QCoreApplication::translate(
                          "TopologyJson", "Each link must be an object");
            problem.detail = QCoreApplication::translate(
                "TopologyJson", "Nothing was applied.");
            out.problem = problem;
            return out;
        }
        // Each link has a mode, which defaults to duplex and must be known.
        const QJsonObject link = value.toObject();
        const QString mode = link.value(QStringLiteral("mode"))
                                 .toString(QStringLiteral("duplex"));
        bool mode_known = false;
        for (const char* candidate : vss::config::link_modes)
        {
            if (mode == QLatin1String(candidate))
                mode_known = true;
        }
        if (!mode_known)
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::failed}};
            const int mode_line =
                lines.line(QStringLiteral("links/%1/mode").arg(index));
            json_problem problem;
            problem.line = mode_line;
            problem.summary =
                mode_line > 0
                    ? QCoreApplication::translate(
                          "TopologyJson", "Line %1: unknown link mode \"%2\"")
                          .arg(mode_line)
                          .arg(mode)
                    : QCoreApplication::translate("TopologyJson",
                                                  "Unknown link mode \"%1\"")
                          .arg(mode);
            // If a known mode is close to the typo, offer it as a fix the
            // editor can apply (fix_from and fix_to).
            const QString suggestion = closest_mode(mode);
            if (!suggestion.isEmpty() && suggestion != mode)
            {
                problem.detail =
                    QCoreApplication::translate(
                        "TopologyJson",
                        "Did you mean \"%1\"? The line is highlighted and "
                        "nothing was applied.")
                        .arg(suggestion);
                problem.fix_from = mode;
                problem.fix_to = suggestion;
            }
            else
            {
                problem.detail = QCoreApplication::translate(
                    "TopologyJson",
                    "Use duplex, split, join, redirect, monitor or bus. "
                    "Nothing was applied.");
            }
            out.problem = problem;
            return out;
        }

        // A half-duplex line is the option of a bus only, and a build without
        // it refuses the member.  A bus in a build without shared buses is
        // refused as a bus, below.
        if (mode != QLatin1String("bus") || shared_bus)
        {
            if (const auto issue =
                    half_duplex_issue_of(link, mode, half_duplex_bus))
            {
                out.checks = {{QCoreApplication::translate(
                                   "TopologyJson", "Valid JSON, version 1"),
                               check_state::passed},
                              {QCoreApplication::translate(
                                   "TopologyJson", "Ports, all names unique"),
                               check_state::passed},
                              {QCoreApplication::translate("TopologyJson",
                                                           "Links, no loops"),
                               check_state::failed}};
                const int issue_line = lines.line(
                    QStringLiteral("links/%1/").arg(index) + issue->member);
                out.problem =
                    json_problem{with_line(issue_line, issue->summary),
                                 issue->detail,
                                 {},
                                 {},
                                 issue_line};
                return out;
            }
        }

        // A bus link lists its "members" instead of sources and targets, and
        // is handled completely in this branch.
        if (mode == QLatin1String("bus"))
        {
            // Every bus problem fails the links row; only the line, summary
            // and detail differ.
            const auto reject =
                [&out](int line, const QString& summary, const QString& detail)
            {
                out.checks = {{QCoreApplication::translate(
                                   "TopologyJson", "Valid JSON, version 1"),
                               check_state::passed},
                              {QCoreApplication::translate(
                                   "TopologyJson", "Ports, all names unique"),
                               check_state::passed},
                              {QCoreApplication::translate("TopologyJson",
                                                           "Links, no loops"),
                               check_state::failed}};
                out.problem = json_problem{
                    with_line(line, summary), detail, {}, {}, line};
            };

            // A build without shared-bus support cannot run a bus link.
            if (!shared_bus)
            {
                reject(link_line,
                       QCoreApplication::translate(
                           "TopologyJson",
                           "Bus links are not available in this build"),
                       QCoreApplication::translate(
                           "TopologyJson",
                           "This build cannot put ports on a shared bus. "
                           "Nothing was applied."));
                return out;
            }

            // "members" must be a list of strings with between 2 and
            // max_bus_members entries.
            QStringList members;
            bool members_ok = link.value(QStringLiteral("members")).isArray();
            if (members_ok)
            {
                for (const auto& entry :
                     link.value(QStringLiteral("members")).toArray())
                {
                    if (!entry.isString())
                        members_ok = false;
                    else
                        members << entry.toString();
                }
            }
            const int members_line =
                lines.line(QStringLiteral("links/%1/members").arg(index));
            if (!members_ok || members.size() < 2 ||
                members.size() > static_cast<qsizetype>(vss::max_bus_members))
            {
                reject(members_line > 0 ? members_line : link_line,
                       QCoreApplication::translate(
                           "TopologyJson", "A bus needs between 2 and %1 ports")
                           .arg(vss::max_bus_members),
                       QCoreApplication::translate(
                           "TopologyJson",
                           "List the ports of the bus in \"members\". Nothing "
                           "was applied."));
                return out;
            }

            // Each member must be a known port, and a port can sit on only
            // one bus.
            for (qsizetype at = 0; at < members.size(); ++at)
            {
                const QString& name = members.at(at);
                const int member_line = lines.line(
                    QStringLiteral("links/%1/members/%2").arg(index).arg(at));
                if (!port_names.contains(name))
                {
                    reject(member_line,
                           QCoreApplication::translate(
                               "TopologyJson", "No port is called \"%1\"")
                               .arg(name),
                           QCoreApplication::translate("TopologyJson",
                                                       "Nothing was applied."));
                    return out;
                }
                if (bus_members.contains(name))
                {
                    reject(member_line,
                           QCoreApplication::translate(
                               "TopologyJson", "A port can be on one bus only"),
                           QCoreApplication::translate("TopologyJson",
                                                       "Nothing was applied."));
                    return out;
                }
                bus_members.insert(name);
            }

            // A route that already joins two members, directly or through
            // other ports, would be a loop through the bus.
            for (const QString& first : members)
            {
                for (const QString& second : members)
                {
                    if (first == second || !reachable(edges, first, second))
                        continue;
                    reject(link_line,
                           QCoreApplication::translate(
                               "TopologyJson", "These links create a loop"),
                           QCoreApplication::translate(
                               "TopologyJson",
                               "Bytes would go round forever, so the hub "
                               "refuses a loop and nothing was applied."));
                    return out;
                }
            }
            // Members of a bus hear each other, so add a route for every
            // ordered pair for the checks of later links.
            for (const QString& first : members)
            {
                for (const QString& second : members)
                {
                    if (first != second)
                        edges[first].insert(second);
                }
            }
            // The source and target checks below do not apply to a bus.
            continue;
        }

        // Every other mode reads "sources" and "targets": non-empty lists of
        // port names.
        QStringList sources;
        QStringList targets;
        const auto read_names = [](const QJsonObject& object, const char* key,
                                   QStringList& out_list) -> bool
        {
            const QJsonValue array_value =
                object.value(QString::fromLatin1(key));
            if (!array_value.isArray())
                return false;
            const QJsonArray array = array_value.toArray();
            for (const auto& entry : array)
            {
                if (!entry.isString())
                    return false;
                out_list << entry.toString();
            }
            return !out_list.isEmpty();
        };
        if (!read_names(link, "sources", sources) ||
            !read_names(link, "targets", targets))
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::failed}};
            json_problem problem;
            problem.line = link_line;
            problem.summary =
                link_line > 0
                    ? QCoreApplication::translate(
                          "TopologyJson",
                          "Line %1: a link needs ports on both sides")
                          .arg(link_line)
                    : QCoreApplication::translate(
                          "TopologyJson", "A link needs ports on both sides");
            problem.detail = QCoreApplication::translate(
                "TopologyJson",
                "sources and targets must be non-empty lists of port names. "
                "Nothing was applied.");
            out.problem = problem;
            return out;
        }

        // How many ports each side may hold depends on the mode: split fans
        // out from one source, join fans in to one target, and duplex,
        // redirect and monitor are one to one.
        const bool one_source = sources.size() == 1;
        const bool one_target = targets.size() == 1;
        bool arity_ok = true;
        if (mode == QLatin1String("duplex") ||
            mode == QLatin1String("redirect") ||
            mode == QLatin1String("monitor"))
            arity_ok = one_source && one_target;
        else if (mode == QLatin1String("split"))
            arity_ok = one_source;
        else if (mode == QLatin1String("join"))
            arity_ok = one_target;
        if (!arity_ok)
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::failed}};
            json_problem problem;
            problem.line = link_line;
            problem.summary =
                link_line > 0
                    ? QCoreApplication::translate(
                          "TopologyJson",
                          "Line %1: \"%2\" has the wrong number of ports")
                          .arg(link_line)
                          .arg(mode)
                    : QCoreApplication::translate(
                          "TopologyJson",
                          "\"%1\" has the wrong number of ports")
                          .arg(mode);
            problem.detail = QCoreApplication::translate(
                "TopologyJson",
                "A duplex, redirect or monitor link joins one port to one "
                "port; a split has one source; a join has one target. Nothing "
                "was applied.");
            out.problem = problem;
            return out;
        }

        // Every port a link names must exist.  The problem points at the
        // entry that named it, looking in the sources first.
        for (const QString& name : sources + targets)
        {
            if (!port_names.contains(name))
            {
                out.checks = {{QCoreApplication::translate(
                                   "TopologyJson", "Valid JSON, version 1"),
                               check_state::passed},
                              {QCoreApplication::translate(
                                   "TopologyJson", "Ports, all names unique"),
                               check_state::passed},
                              {QCoreApplication::translate("TopologyJson",
                                                           "Links, no loops"),
                               check_state::failed}};
                const qsizetype source_at = sources.indexOf(name);
                const int reference_line = lines.line(
                    source_at >= 0
                        ? QStringLiteral("links/%1/sources/%2")
                              .arg(index)
                              .arg(source_at)
                        : QStringLiteral("links/%1/targets/%2")
                              .arg(index)
                              .arg(targets.indexOf(name)));
                json_problem problem;
                problem.line = reference_line;
                problem.summary =
                    reference_line > 0
                        ? QCoreApplication::translate(
                              "TopologyJson",
                              "Line %1: no port is called \"%2\"")
                              .arg(reference_line)
                              .arg(name)
                        : QCoreApplication::translate(
                              "TopologyJson", "No port is called \"%1\"")
                              .arg(name);
                problem.detail = QCoreApplication::translate(
                    "TopologyJson", "Nothing was applied.");
                out.problem = problem;
                return out;
            }
        }

        // A loop is refused the same way the hub refuses one: check the
        // routes that already exist before adding this link.
        // A link loops if a port feeds itself, if its target already reaches
        // its source, or, for duplex, if the source already reaches the
        // target (the reverse direction would then close a loop).
        bool loop = false;
        for (const QString& source : sources)
        {
            for (const QString& target : targets)
            {
                if (source == target)
                    loop = true;
                if (reachable(edges, target, source))
                    loop = true;
                if (mode == QLatin1String("duplex") &&
                    reachable(edges, source, target))
                    loop = true;
            }
        }
        if (loop)
        {
            out.checks = {
                {QCoreApplication::translate("TopologyJson",
                                             "Valid JSON, version 1"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson",
                                             "Ports, all names unique"),
                 check_state::passed},
                {QCoreApplication::translate("TopologyJson", "Links, no loops"),
                 check_state::failed}};
            json_problem problem;
            problem.line = link_line;
            problem.summary =
                link_line > 0
                    ? QCoreApplication::translate(
                          "TopologyJson", "Line %1: these links create a loop")
                          .arg(link_line)
                    : QCoreApplication::translate("TopologyJson",
                                                  "These links create a loop");
            problem.detail = QCoreApplication::translate(
                "TopologyJson",
                "Bytes would go round forever, so the hub refuses a loop and "
                "nothing was applied.");
            out.problem = problem;
            return out;
        }
        // The link is accepted: record its routes for later links.  A duplex
        // link carries bytes both ways.
        for (const QString& source : sources)
        {
            for (const QString& target : targets)
            {
                edges[source].insert(target);
                if (mode == QLatin1String("duplex"))
                    edges[target].insert(source);
            }
        }
    }

    // -- the engine has the final word ------------------------------------
    // The checks above passed.  Show all three rows as passed, then let the
    // real config parser decide: it enforces rules not repeated here.
    out.checks = {
        {QCoreApplication::translate("TopologyJson", "Valid JSON, version 1"),
         check_state::passed},
        {ports_check_text(out.ports, true), check_state::passed},
        {links_check_text(out.links, true), check_state::passed}};
    const auto parsed = vss::config::from_json(text.toStdString());
    if (!parsed)
    {
        // The parser reports only an error code, so engine_problem_line()
        // works out which line to highlight.  A fourth row records the
        // failure.
        out.schema_valid = false;
        out.checks.push_back(
            {QCoreApplication::translate(
                 "TopologyJson", "The setup format rejected this document"),
             check_state::failed});
        const int engine_line = engine_problem_line(root, lines);
        out.problem = json_problem{
            with_line(engine_line,
                      QCoreApplication::translate(
                          "TopologyJson",
                          "The setup format rejected this document")),
            error_text(parsed.error()),
            {},
            {},
            engine_line};
        return out;
    }
    // Take the name and counts from the parsed document, since that is what
    // would be applied.
    out.document = *parsed;
    out.name = QString::fromStdString(out.document.name);
    out.ports = static_cast<int>(out.document.ports.size());
    out.links = static_cast<int>(out.document.links.size());
    out.checks[1] = {ports_check_text(out.ports, true), check_state::passed};
    out.checks[2] = {links_check_text(out.links, true), check_state::passed};
    // Write the document back out as indented JSON.  That text is what the
    // Format action shows, and a failure here counts as invalid too.
    const auto formatted = vss::config::to_json(out.document, true);
    if (!formatted)
    {
        out.schema_valid = false;
        out.problem = json_problem{
            QCoreApplication::translate(
                "TopologyJson", "The setup format rejected this document"),
            error_text(formatted.error()),
            {},
            {},
            0};
        return out;
    }
    out.formatted = QString::fromStdString(*formatted).trimmed();
    out.schema_valid = true;
    return out;
}

} // namespace vss::gui
