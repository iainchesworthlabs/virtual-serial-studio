//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `licence.*` (`make_licence_point`) on its own.  A test
// makes the handler with a hand-made `extension_context`: the link records
// what is answered or sent, the source of the licence file is a fake that
// counts its calls, and `licence_reported` keeps what it is given.  Nothing
// here starts a process.  The same point with a real plugin is in
// test_system_plugin_service.cpp.
//

#include <vss/core/bytes.hpp>
#include <vss/license/license.hpp>
#include <vss/plugin/api_licence.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/licence_point.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/protocol.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using vss::byte;
using vss::byte_buffer;
using vss::license::reason;
using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::decode_as;
using vss::plugin::encode_as;
using vss::plugin::error_message;
using vss::plugin::licence_file_message;
using vss::plugin::licence_get_message;
using vss::plugin::licence_report_message;
using vss::plugin::max_licence_file;
using vss::plugin::max_licence_id;
using vss::plugin::other_message;
using vss::plugin::permission;
using vss::plugin::ping_message;
using vss::plugin::host::extension_context;
using vss::plugin::host::extension_handler;
using vss::plugin::host::licence_file_source;
using vss::plugin::host::make_licence_point;
using vss::plugin::host::plugin_licence_report;
using vss::plugin::host::plugin_link;

namespace
{

constexpr std::uint64_t first_id = 7;

/// What the handler said to the plugin: the request it answers, and the
/// answer.
struct answered
{
    std::uint64_t re = 0;
    api_message message{};
};

/// What a test says of the plugin and of the program around the handler.
struct rig_options
{
    bool official = false;
    std::vector<permission> granted{};
    /// Empty: the program offers no licence file.
    licence_file_source source{};
    /// False: the service keeps nothing of a report, and gives no function.
    bool keeps_reports = true;
};

/// The handler with everything around it.  `answers` has what was answered,
/// `notices` and `requests` what was sent to the plugin on the handler's own
/// account, `reports` what `licence_reported` was given and `calls` how
/// often the source was asked.
struct rig
{
    explicit rig(const rig_options& options)
    {
        extension_context context;
        context.plugin_id = "com.example.licence";
        context.plugin_name = "Licence plugin";
        context.plugin_version = "1.0.0";
        context.official = options.official;
        context.granted = options.granted;
        context.link.answer = [this](std::uint64_t re, api_message message)
        { answers.push_back(answered{re, std::move(message)}); };
        context.link.notify = [this](api_message message)
        { notices.push_back(std::move(message)); };
        context.link.request =
            [this](api_message message,
                   const plugin_link::answer_handler& /*handler*/,
                   std::chrono::milliseconds /*timeout*/)
        { requests.push_back(std::move(message)); };
        if (options.keeps_reports)
        {
            context.licence_reported = [this](plugin_licence_report said)
            { reports.push_back(std::move(said)); };
        }
        licence_file_source source;
        if (options.source)
        {
            source = [this, inner = options.source]()
            {
                ++calls;
                return inner();
            };
        }
        handler = make_licence_point(context, std::move(source));
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    /// Offers the handler a message and says whether it took it.
    bool offer(const api_message& message) const
    {
        return handler->take(message);
    }

    /// Whether the handler has sent the plugin nothing at all.
    bool silent() const
    {
        return answers.empty() && notices.empty() && requests.empty();
    }

    std::vector<answered> answers{};
    std::vector<api_message> notices{};
    std::vector<api_message> requests{};
    std::vector<plugin_licence_report> reports{};
    std::size_t calls = 0;
    std::unique_ptr<extension_handler> handler{};
};

// -- sources ------------------------------------------------------------------

/// A source that hands out the bytes, as it would when the file was read
/// before.
licence_file_source giving(byte_buffer bytes)
{
    return [bytes = std::move(bytes)]() -> std::optional<byte_buffer>
    { return bytes; };
}

/// A source that says there is no licence file.
licence_file_source giving_nothing()
{
    return []() -> std::optional<byte_buffer> { return std::nullopt; };
}

/// Bytes that have a zero byte and bytes over 0x7f in them, so that a copy
/// that treats them as text or as signed would show.
byte_buffer odd_bytes()
{
    return byte_buffer{0x00, 0x41, 0x7f, 0x80, 0xff, 0x0a, 0x00, 0xc3, 0x28};
}

/// `count` bytes that are not all the same.
byte_buffer pattern(std::size_t count)
{
    byte_buffer bytes(count);
    for (std::size_t index = 0; index < count; ++index)
        bytes[index] = static_cast<byte>(index % 251);
    return bytes;
}

// -- messages -----------------------------------------------------------------

template <class Message>
api_message message_of(const Message& body, std::uint64_t id)
{
    auto encoded = encode_as(body, id);
    if (encoded.has_error())
        throw vss_test::failure("a message would not encode: " +
                                std::string(Message::op()));
    return std::move(encoded).value();
}

api_message get_request(std::uint64_t id = first_id)
{
    return message_of(licence_get_message{}, id);
}

/// A request whose body is the header text as it is, whatever it holds: the
/// handler reads strictly, and a session would not have passed some of these.
api_message raw_message(std::string_view op, std::uint64_t id,
                        std::string_view header)
{
    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.body = other_message{std::string(header)};
    return message;
}

/// A `licence.report` with the given members after its op and, when `id` is
/// not zero, its id.  With `id` 0 it is a notification, which is what the
/// message is.
api_message report_with(std::string_view members, std::uint64_t id = 0)
{
    std::string header = R"({"op":"licence.report")";
    if (id != 0)
    {
        header.append(R"(,"id":)");
        header.append(std::to_string(id));
    }
    if (!members.empty())
    {
        header.push_back(',');
        header.append(members);
    }
    header.push_back('}');
    return raw_message("licence.report", id, header);
}

/// The code an answer carries: `ok`, or the name of the `api_errc` of an
/// error.
std::string code_of(const answered& answer)
{
    if (const auto* const error =
            std::get_if<error_message>(&answer.message.body))
    {
        REQUIRE_EQ(error->category, std::string("vss.plugin.api"));
        return error->value;
    }
    throw vss_test::failure("an answer is not an error: " + answer.message.op);
}

std::string code_name(api_errc code)
{
    return std::string(vss::plugin::to_string(code));
}

/// The one answer of a rig as a `licence.file`, which must be there and be
/// the answer to the request `re`.
licence_file_message file_answer(const rig& fixture, std::uint64_t re)
{
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    const answered& answer = fixture.answers.front();
    REQUIRE_EQ(answer.re, re);
    REQUIRE_EQ(answer.message.op, std::string("licence.file"));
    REQUIRE_EQ(answer.message.re, re);
    REQUIRE_EQ(answer.message.id, std::uint64_t{0});
    std::string detail;
    auto file = decode_as<licence_file_message>(answer.message, &detail);
    if (file.has_error())
        throw vss_test::failure("the answer is not a licence.file: " + detail);
    return std::move(file).value();
}

/// Checks that the one answer of a rig is the error `code` for the request
/// `re`.
void require_error(const rig& fixture, std::uint64_t re, api_errc code)
{
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(fixture.answers.front().re, re);
    REQUIRE_EQ(code_of(fixture.answers.front()), code_name(code));
}

/// An official plugin with a source that gives the bytes.
rig_options official_with(licence_file_source source)
{
    rig_options options;
    options.official = true;
    options.source = std::move(source);
    return options;
}

/// A plugin that is not official, with the permission `licence.read`.
rig_options granted_with(licence_file_source source)
{
    rig_options options;
    options.granted = {permission::licence_read};
    options.source = std::move(source);
    return options;
}

} // namespace

TEST_CASE("plugin licence point: an official plugin with no permission is "
          "sent the file exactly")
{
    const byte_buffer bytes = odd_bytes();
    rig fixture(official_with(giving(bytes)));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(file.present);
    REQUIRE(file.data == bytes);
    REQUIRE_EQ(fixture.calls, std::size_t{1});
    REQUIRE(fixture.notices.empty());
    REQUIRE(fixture.requests.empty());
    REQUIRE(fixture.reports.empty());
}

TEST_CASE("plugin licence point: every request is answered with its own id")
{
    rig fixture(official_with(giving(odd_bytes())));
    REQUIRE(fixture.offer(get_request(3)));
    REQUIRE(fixture.offer(get_request(9)));
    REQUIRE_EQ(fixture.answers.size(), std::size_t{2});
    REQUIRE_EQ(fixture.answers[0].re, std::uint64_t{3});
    REQUIRE_EQ(fixture.answers[0].message.re, std::uint64_t{3});
    REQUIRE_EQ(fixture.answers[1].re, std::uint64_t{9});
    REQUIRE_EQ(fixture.answers[1].message.re, std::uint64_t{9});
    REQUIRE_EQ(fixture.calls, std::size_t{2});
}

TEST_CASE("plugin licence point: a plugin that is not official and has no "
          "licence.read is not granted")
{
    // Another permission does not stand in for `licence.read`.
    rig_options options;
    options.granted = {permission::traffic_tap};
    options.source = giving(odd_bytes());
    rig fixture(options);
    REQUIRE(fixture.offer(get_request()));

    require_error(fixture, first_id, api_errc::not_granted);
    REQUIRE_EQ(fixture.calls, std::size_t{0});
    REQUIRE(fixture.notices.empty());
    REQUIRE(fixture.requests.empty());
}

TEST_CASE("plugin licence point: a plugin with no permission at all is not "
          "granted")
{
    rig_options options;
    options.source = giving(odd_bytes());
    rig fixture(options);
    REQUIRE(fixture.offer(get_request()));

    require_error(fixture, first_id, api_errc::not_granted);
    REQUIRE_EQ(fixture.calls, std::size_t{0});
}

TEST_CASE("plugin licence point: a plugin that is not official but has "
          "licence.read is sent the file")
{
    const byte_buffer bytes = odd_bytes();
    rig fixture(granted_with(giving(bytes)));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(file.present);
    REQUIRE(file.data == bytes);
    REQUIRE_EQ(fixture.calls, std::size_t{1});
}

TEST_CASE("plugin licence point: with no licence file the answer says there "
          "is none")
{
    rig fixture(official_with(giving_nothing()));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(!file.present);
    REQUIRE(file.data.empty());
    REQUIRE_EQ(fixture.calls, std::size_t{1});
}

TEST_CASE("plugin licence point: a file of no bytes is no licence file")
{
    rig fixture(official_with(giving(byte_buffer{})));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(!file.present);
    REQUIRE(file.data.empty());
}

TEST_CASE("plugin licence point: a file one byte over 64 KiB is no licence "
          "file")
{
    const std::size_t limit = 65536;
    REQUIRE_EQ(max_licence_file, limit);
    rig fixture(official_with(giving(pattern(limit + 1))));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(!file.present);
    REQUIRE(file.data.empty());
    // The source was asked, and what it gave was too large.
    REQUIRE_EQ(fixture.calls, std::size_t{1});
}

TEST_CASE("plugin licence point: a file of exactly 64 KiB is sent")
{
    const std::size_t limit = 65536;
    const byte_buffer bytes = pattern(limit);
    rig fixture(official_with(giving(bytes)));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(file.present);
    REQUIRE_EQ(file.data.size(), limit);
    REQUIRE(file.data == bytes);
}

TEST_CASE("plugin licence point: a file of one byte is sent")
{
    const byte_buffer bytes{0xff};
    rig fixture(official_with(giving(bytes)));
    REQUIRE(fixture.offer(get_request()));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(file.present);
    REQUIRE(file.data == bytes);
}

TEST_CASE("plugin licence point: with no source every plugin is told there "
          "is no licence")
{
    rig official(official_with(licence_file_source{}));
    REQUIRE(official.offer(get_request()));
    const licence_file_message first = file_answer(official, first_id);
    REQUIRE(!first.present);
    REQUIRE(first.data.empty());

    rig granted(granted_with(licence_file_source{}));
    REQUIRE(granted.offer(get_request()));
    const licence_file_message second = file_answer(granted, first_id);
    REQUIRE(!second.present);
    REQUIRE(second.data.empty());
}

TEST_CASE("plugin licence point: a member that the decoder does not know is "
          "passed over")
{
    // A newer minor version may add a member, and the message is still good.
    const byte_buffer bytes = odd_bytes();
    rig fixture(official_with(giving(bytes)));
    REQUIRE(fixture.offer(raw_message(
        "licence.get", first_id, R"({"op":"licence.get","id":7,"later":1})")));

    const licence_file_message file = file_answer(fixture, first_id);
    REQUIRE(file.present);
    REQUIRE(file.data == bytes);
}

namespace
{

/// A `licence.get` that must be answered `bad_request` and never read from
/// the source.
struct bad_get
{
    std::string name{};
    api_message message{};
};

std::vector<bad_get> bad_gets()
{
    std::vector<bad_get> cases;
    const auto add = [&cases](std::string name, api_message message)
    { cases.push_back(bad_get{std::move(name), std::move(message)}); };

    // A body that is not an `other_message`.
    api_message ping;
    ping.op = "licence.get";
    ping.id = first_id;
    ping.body = ping_message{};
    add("a body this code knows", std::move(ping));

    // The message has no blob.
    api_message with_blob =
        raw_message("licence.get", first_id, R"({"op":"licence.get","id":7})");
    with_blob.blob = byte_buffer{0x01};
    add("a blob", std::move(with_blob));

    // An answer's number on a request.
    api_message with_re =
        raw_message("licence.get", first_id, R"({"op":"licence.get","id":7})");
    with_re.re = 3;
    add("a re", std::move(with_re));

    // A header that is not an object cannot come from a decoded frame, but
    // the handler does not rely on that.
    for (const std::string_view header :
         {R"([1,2,3])", R"("licence.get")", R"(42)", R"(null)", R"({)",
          R"({"op":"licence.get","id":7,"id":7})"})
    {
        std::string name = "a header that is ";
        name.append(header);
        add(std::move(name), raw_message("licence.get", first_id, header));
    }
    return cases;
}

} // namespace

TEST_CASE("plugin licence point: a malformed licence.get is bad_request")
{
    for (const bad_get& item : bad_gets())
    {
        // Official, so that only the form of the message can be the reason.
        rig fixture(official_with(giving(odd_bytes())));
        if (!fixture.offer(item.message))
            throw vss_test::failure("not taken: " + item.name);
        if (fixture.answers.size() != 1)
            throw vss_test::failure("not answered once: " + item.name);
        if (fixture.answers.front().re != first_id)
            throw vss_test::failure("answered the wrong request: " + item.name);
        if (code_of(fixture.answers.front()) !=
            code_name(api_errc::bad_request))
            throw vss_test::failure("not bad_request: " + item.name);
        if (fixture.calls != 0)
            throw vss_test::failure("the source was asked: " + item.name);
    }
}

TEST_CASE("plugin licence point: a licence.get with no id is taken and not "
          "answered")
{
    // A request must have an id, so this is refused, and there is nobody to
    // tell.
    rig fixture(official_with(giving(odd_bytes())));
    REQUIRE(fixture.offer(
        raw_message("licence.get", 0, R"({"op":"licence.get"})")));
    REQUIRE(fixture.silent());
    REQUIRE_EQ(fixture.calls, std::size_t{0});
}

TEST_CASE("plugin licence point: a licence.report with reason expired and a "
          "licence id is kept as it came")
{
    rig fixture(rig_options{});
    licence_report_message body;
    body.reason = reason::expired;
    body.licence_id = "LIC-2026-0042";
    REQUIRE(fixture.offer(message_of(body, 0)));

    REQUIRE_EQ(fixture.reports.size(), std::size_t{1});
    REQUIRE(fixture.reports.front() ==
            (plugin_licence_report{reason::expired, "LIC-2026-0042"}));
    // A report is a notification, and the host sends nothing for it.
    REQUIRE(fixture.silent());
    REQUIRE_EQ(fixture.calls, std::size_t{0});
}

TEST_CASE("plugin licence point: a licence.report with reason ok and no id is "
          "kept with an empty id")
{
    rig fixture(rig_options{});
    licence_report_message body;
    body.reason = reason::ok;
    REQUIRE(fixture.offer(message_of(body, 0)));

    REQUIRE_EQ(fixture.reports.size(), std::size_t{1});
    REQUIRE(fixture.reports.front().reason == reason::ok);
    REQUIRE(fixture.reports.front().licence_id.empty());
    REQUIRE(fixture.silent());
}

TEST_CASE("plugin licence point: each reason a plugin may name is kept")
{
    const std::vector<std::pair<std::string_view, reason>> names{
        {"ok", reason::ok},
        {"no_licence", reason::no_licence},
        {"expired", reason::expired},
        {"not_yet_valid", reason::not_yet_valid},
        {"clock_suspect", reason::clock_suspect},
        {"malformed", reason::malformed},
        {"newer_version", reason::newer_version},
        {"bad_signature", reason::bad_signature},
        {"revoked", reason::revoked},
        {"wrong_computer", reason::wrong_computer},
        {"not_in_licence", reason::not_in_licence}};
    rig fixture(rig_options{});
    for (const auto& [name, value] : names)
    {
        std::string members = R"("reason":")";
        members.append(name);
        members.push_back('"');
        REQUIRE(fixture.offer(report_with(members)));
    }
    REQUIRE_EQ(fixture.reports.size(), names.size());
    for (std::size_t index = 0; index < names.size(); ++index)
    {
        REQUIRE(fixture.reports[index].reason == names[index].second);
        REQUIRE(fixture.reports[index].licence_id.empty());
    }
}

TEST_CASE("plugin licence point: a licence id of 64 bytes is kept")
{
    rig fixture(rig_options{});
    const std::string id(max_licence_id, 'a');
    REQUIRE_EQ(id.size(), std::size_t{64});
    REQUIRE(fixture.offer(
        report_with(R"("reason":"revoked","licence_id":")" + id + "\"")));

    REQUIRE_EQ(fixture.reports.size(), std::size_t{1});
    REQUIRE(fixture.reports.front() ==
            (plugin_licence_report{reason::revoked, id}));
}

TEST_CASE("plugin licence point: a report from a plugin that is not official "
          "and has no permission is still kept")
{
    // A report changes nothing the host does, so it needs no permission.
    rig fixture(rig_options{});
    REQUIRE(fixture.offer(report_with(R"("reason":"expired")")));
    REQUIRE_EQ(fixture.reports.size(), std::size_t{1});
    REQUIRE(fixture.reports.front().reason == reason::expired);
}

namespace
{

/// The members of a `licence.report` that must not be kept.
struct bad_report
{
    std::string name{};
    std::string members{};
};

std::vector<bad_report> bad_reports()
{
    const std::string id65(max_licence_id + 1, 'a');
    std::vector<bad_report> cases;
    const auto add = [&cases](std::string name, std::string members)
    { cases.push_back(bad_report{std::move(name), std::move(members)}); };

    add("an unknown reason", R"("reason":"gone")");
    add("a reason in capitals", R"("reason":"Expired")");
    add("a reason that is a number", R"("reason":2)");
    add("a reason that is null", R"("reason":null)");
    add("no reason", R"("licence_id":"LIC-1")");
    add("no members at all", "");
    add("a licence id of 65 bytes",
        R"("reason":"expired","licence_id":")" + id65 + "\"");
    add("an empty licence id", R"("reason":"expired","licence_id":"")");
    add("a licence id that is a number",
        R"("reason":"expired","licence_id":5)");
    add("a licence id with a newline",
        R"("reason":"expired","licence_id":"LIC\n1")");
    add("a licence id with an escape character",
        R"("reason":"expired","licence_id":"LIC\u001b[31m")");
    return cases;
}

} // namespace

TEST_CASE("plugin licence point: a malformed report is not kept, and with id "
          "0 nothing is sent")
{
    for (const bad_report& item : bad_reports())
    {
        rig fixture(rig_options{});
        if (!fixture.offer(report_with(item.members)))
            throw vss_test::failure("not taken: " + item.name);
        if (!fixture.reports.empty())
            throw vss_test::failure("kept: " + item.name);
        if (!fixture.silent())
            throw vss_test::failure("something was sent: " + item.name);
    }
}

TEST_CASE("plugin licence point: a malformed report that has an id is "
          "answered bad_request")
{
    for (const bad_report& item : bad_reports())
    {
        rig fixture(rig_options{});
        if (!fixture.offer(report_with(item.members, first_id)))
            throw vss_test::failure("not taken: " + item.name);
        if (!fixture.reports.empty())
            throw vss_test::failure("kept: " + item.name);
        if (fixture.answers.size() != 1)
            throw vss_test::failure("not answered once: " + item.name);
        if (fixture.answers.front().re != first_id)
            throw vss_test::failure("answered the wrong request: " + item.name);
        if (code_of(fixture.answers.front()) !=
            code_name(api_errc::bad_request))
            throw vss_test::failure("not bad_request: " + item.name);
    }
}

TEST_CASE("plugin licence point: a report sent as a request is bad_request")
{
    // The message is a notification: one with an id is not as its role says,
    // even when its members are good.
    rig fixture(rig_options{});
    REQUIRE(fixture.offer(report_with(R"("reason":"expired")", first_id)));
    REQUIRE(fixture.reports.empty());
    require_error(fixture, first_id, api_errc::bad_request);
}

TEST_CASE("plugin licence point: a report with a blob or a body this code "
          "knows is not kept")
{
    rig fixture(rig_options{});

    api_message with_blob = report_with(R"("reason":"expired")");
    with_blob.blob = byte_buffer{0x01, 0x02};
    REQUIRE(fixture.offer(with_blob));

    api_message ping;
    ping.op = "licence.report";
    ping.body = ping_message{};
    REQUIRE(fixture.offer(ping));

    REQUIRE(fixture.reports.empty());
    REQUIRE(fixture.silent());
}

TEST_CASE("plugin licence point: a report when licence_reported is empty "
          "does not crash")
{
    rig_options options;
    options.keeps_reports = false;
    rig fixture(options);

    REQUIRE(fixture.offer(report_with(R"("reason":"expired",)"
                                      R"("licence_id":"LIC-1")")));
    REQUIRE(fixture.silent());

    // A bad one is still answered when it has an id.
    REQUIRE(fixture.offer(report_with(R"("reason":"gone")", first_id)));
    require_error(fixture, first_id, api_errc::bad_request);
}

TEST_CASE("plugin licence point: an op that is not the handler's is not "
          "taken")
{
    rig fixture(official_with(giving(odd_bytes())));
    for (const std::string_view op :
         {"licence.file", "licence.gets", "licence.report.more", "licence",
          "licence.", "secret.show", "capture.start", "generator.start"})
    {
        std::string header = R"({"op":")";
        header.append(op);
        header.append(R"(","id":7})");
        if (fixture.offer(raw_message(op, first_id, header)))
            throw vss_test::failure("taken: " + std::string(op));
    }
    api_message ping;
    ping.op = "ping";
    ping.id = 3;
    ping.body = ping_message{};
    REQUIRE(!fixture.offer(ping));

    REQUIRE(fixture.silent());
    REQUIRE(fixture.reports.empty());
    REQUIRE_EQ(fixture.calls, std::size_t{0});
}
