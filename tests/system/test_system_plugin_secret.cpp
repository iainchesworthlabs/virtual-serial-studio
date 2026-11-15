//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The extension point `secret.show` (`make_secret_point`) on its own.  A test
// makes the handler with a hand-made `extension_context`: the link records
// what is answered, the executor is an `io_context` that the test runs by
// hand, and the shower is a fake that keeps what it was given and the `done`
// it must call.  Nothing here starts a process.  The same point with a real
// plugin is in test_system_plugin_service.cpp and
// test_system_plugin_run_cli.cpp.
//

#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/secret_point.hpp>
#include <vss/plugin/protocol.hpp>

#include <boost/asio/io_context.hpp>

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using vss::plugin::api_errc;
using vss::plugin::api_message;
using vss::plugin::decode_message;
using vss::plugin::encode_message;
using vss::plugin::error_message;
using vss::plugin::ok_message;
using vss::plugin::other_message;
using vss::plugin::ping_message;
using vss::plugin::with_ids;
using vss::plugin::host::extension_context;
using vss::plugin::host::extension_handler;
using vss::plugin::host::make_secret_point;
using vss::plugin::host::secret_shower;
using vss::plugin::host::secret_to_show;
using namespace std::chrono_literals;

namespace
{

constexpr std::string_view show_op = "secret.show";
constexpr std::uint64_t first_id = 7;

/// What the handler said to the plugin: the request it answers, and the
/// answer.
struct answered
{
    std::uint64_t re = 0;
    api_message message{};
};

/// The handler with everything around it.  `answers` has what was answered,
/// `shown` what the shower was given and `dones` the `done` of each, in the
/// order they came.
class rig
{
public:
    explicit rig(bool with_shower = true,
                 std::string plugin_name = "Pairing plugin")
    {
        context_.plugin_id = "com.example.secret";
        context_.plugin_name = std::move(plugin_name);
        context_.executor = io.get_executor();
        context_.link.answer = [this](std::uint64_t re, api_message message)
        { answers.push_back(answered{re, std::move(message)}); };
        secret_shower show;
        if (with_shower)
        {
            show = [this](secret_to_show secret, std::function<void(bool)> done)
            {
                shown.push_back(std::move(secret));
                dones.push_back(std::move(done));
            };
        }
        handler = make_secret_point(context_, std::move(show));
    }

    rig(const rig&) = delete;
    rig& operator=(const rig&) = delete;

    /// The strand's work, run now.
    void pump()
    {
        io.poll();
        io.restart();
    }

    /// Offers the handler a message and says whether it took it.
    bool offer(const api_message& message) const
    {
        return handler->take(message);
    }

    boost::asio::io_context io{};
    std::vector<answered> answers{};
    std::vector<secret_to_show> shown{};
    std::vector<std::function<void(bool)>> dones{};
    std::unique_ptr<extension_handler> handler{};

private:
    extension_context context_{};
};

// -- messages -----------------------------------------------------------------

/// A request as the session would hand it over: the header decoded as a
/// frame, which must be accepted.
api_message decoded(std::string_view header)
{
    vss::agent::frame frame;
    frame.header = std::string(header);
    std::string detail;
    auto message = decode_message(frame, &detail);
    if (message.has_error())
        throw vss_test::failure("the header was refused: " + detail + "\n" +
                                std::string(header));
    return std::move(message).value();
}

/// A header of a `secret.show` with the given members after its op and id.
std::string header_with(std::string_view members, std::uint64_t id = first_id)
{
    std::string header = R"({"op":"secret.show","id":)";
    header.append(std::to_string(id));
    header.push_back(',');
    header.append(members);
    header.push_back('}');
    return header;
}

/// What a plugin asks that is good in every way.
constexpr std::string_view good_members =
    R"("label":"Pairing code","text":"1234-5678","expires_s":300)";

/// A request whose body is the header text as it is, whatever it holds: the
/// handler reads strictly, and a session would not have passed some of these.
api_message raw_request(std::string_view op, std::uint64_t id,
                        std::string_view header)
{
    api_message message;
    message.op = std::string(op);
    message.id = id;
    message.body = other_message{std::string(header)};
    return message;
}

api_message good_request(std::uint64_t id = first_id)
{
    return decoded(header_with(good_members, id));
}

/// The code an answer carries: `ok`, or the name of the `api_errc` of an
/// error.
std::string code_of(const answered& answer)
{
    if (std::holds_alternative<ok_message>(answer.message.body))
        return "ok";
    const auto* const error = std::get_if<error_message>(&answer.message.body);
    if (error == nullptr)
        throw vss_test::failure("an answer is neither ok nor an error: " +
                                answer.message.op);
    REQUIRE_EQ(error->category, std::string("vss.plugin.api"));
    return error->value;
}

std::string code_name(api_errc code)
{
    return std::string(vss::plugin::to_string(code));
}

/// Everything an answer would put on the wire, as text: its header.
std::string wire_text_of(const answered& answer)
{
    auto sent = with_ids(answer.message, 0, answer.re);
    REQUIRE(sent.has_value());
    auto frame = encode_message(sent.value());
    REQUIRE(frame.has_value());
    return frame.value().header;
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

/// Says the user has seen the secret that was shown last, or has not, and runs
/// the strand.
void finish_shown(rig& fixture, bool seen)
{
    REQUIRE(!fixture.dones.empty());
    fixture.dones.back()(seen);
    fixture.pump();
}

} // namespace

TEST_CASE("plugin secret: a good request shows the secret, cleaned, and is "
          "answered ok only after done(true)")
{
    // The label and the plugin's name have control characters and spaces to
    // clean; the secret is shown as it came, spaces and all.
    rig fixture(true, "  Test\a   plugin ");
    const api_message request =
        decoded(header_with(R"("label":"  Pairing\u0007   code ",)"
                            R"("text":" 12 34  56 \u00e9 ","expires_s":300)"));
    REQUIRE(fixture.offer(request));

    REQUIRE_EQ(fixture.shown.size(), std::size_t{1});
    const secret_to_show& secret = fixture.shown.front();
    REQUIRE_EQ(secret.plugin_id, std::string("com.example.secret"));
    REQUIRE_EQ(secret.plugin_name, std::string("Test plugin"));
    REQUIRE_EQ(secret.label, std::string("Pairing code"));
    REQUIRE_EQ(secret.text, std::string(" 12 34  56 \xc3\xa9 "));
    REQUIRE(secret.expires == 300s);

    // Nothing is answered while the user has not seen it.
    fixture.pump();
    REQUIRE(fixture.answers.empty());

    finish_shown(fixture, true);
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(fixture.answers.front().re, first_id);
    REQUIRE_EQ(code_of(fixture.answers.front()), std::string("ok"));
}

TEST_CASE("plugin secret: the plugin's name is cut to 128 bytes")
{
    rig fixture(true, std::string(300, 'n'));
    REQUIRE(fixture.offer(good_request()));
    REQUIRE_EQ(fixture.shown.size(), std::size_t{1});
    REQUIRE_EQ(fixture.shown.front().plugin_name, std::string(128, 'n'));
}

TEST_CASE("plugin secret: nothing is answered until done is called")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request()));
    REQUIRE_EQ(fixture.shown.size(), std::size_t{1});
    REQUIRE_EQ(fixture.shown.front().label, std::string("Pairing code"));
    REQUIRE_EQ(fixture.shown.front().text, std::string("1234-5678"));
    REQUIRE(fixture.shown.front().expires == 300s);
    for (int round = 0; round < 3; ++round)
        fixture.pump();
    REQUIRE(fixture.answers.empty());
}

TEST_CASE("plugin secret: done(false) answers not_granted")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request()));
    finish_shown(fixture, false);
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(fixture.answers.front().re, first_id);
    REQUIRE_EQ(code_of(fixture.answers.front()),
               code_name(api_errc::not_granted));

    // The secret is not shown any more, so the plugin may ask again.
    REQUIRE(fixture.offer(good_request(8)));
    REQUIRE_EQ(fixture.shown.size(), std::size_t{2});
}

TEST_CASE("plugin secret: done may be called from another thread")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request()));
    REQUIRE_EQ(fixture.dones.size(), std::size_t{1});
    const std::function<void(bool)> done = fixture.dones.front();
    std::thread other([done] { done(true); });
    other.join();

    // The answer is made on the strand, which here is the test's thread.
    REQUIRE(fixture.answers.empty());
    fixture.pump();
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(code_of(fixture.answers.front()), std::string("ok"));
}

TEST_CASE("plugin secret: a second request while one is shown is busy")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request(7)));
    REQUIRE(fixture.offer(good_request(8)));

    // Busy at once, and the shower was not called again.
    REQUIRE_EQ(fixture.shown.size(), std::size_t{1});
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(fixture.answers.front().re, std::uint64_t{8});
    REQUIRE_EQ(code_of(fixture.answers.front()), code_name(api_errc::busy));

    // The first is still shown, and is answered when it is done.
    finish_shown(fixture, true);
    REQUIRE_EQ(fixture.answers.size(), std::size_t{2});
    REQUIRE_EQ(fixture.answers.back().re, std::uint64_t{7});
    REQUIRE_EQ(code_of(fixture.answers.back()), std::string("ok"));
}

TEST_CASE("plugin secret: after the first is answered a new one is shown")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request(7)));
    finish_shown(fixture, true);
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});

    REQUIRE(fixture.offer(good_request(8)));
    REQUIRE_EQ(fixture.shown.size(), std::size_t{2});
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    finish_shown(fixture, true);
    REQUIRE_EQ(fixture.answers.size(), std::size_t{2});
    REQUIRE_EQ(fixture.answers.back().re, std::uint64_t{8});
    REQUIRE_EQ(code_of(fixture.answers.back()), std::string("ok"));
}

TEST_CASE("plugin secret: with no shower every request is not_granted")
{
    rig fixture(false);
    for (std::uint64_t id = 1; id <= 3; ++id)
        REQUIRE(fixture.offer(good_request(id)));
    REQUIRE_EQ(fixture.answers.size(), std::size_t{3});
    for (std::size_t index = 0; index < fixture.answers.size(); ++index)
    {
        REQUIRE_EQ(fixture.answers[index].re,
                   static_cast<std::uint64_t>(index + 1));
        REQUIRE_EQ(code_of(fixture.answers[index]),
                   code_name(api_errc::not_granted));
    }
    fixture.pump();
    REQUIRE_EQ(fixture.answers.size(), std::size_t{3});
}

namespace
{

/// A request that must be answered `bad_request` and never shown.
struct bad_case
{
    std::string name{};
    std::string header{};
};

std::vector<bad_case> bad_requests()
{
    const std::string label65(65, 'a');
    const std::string text513(513, 'b');
    std::vector<bad_case> cases;
    const auto add = [&cases](std::string name, std::string_view members)
    { cases.push_back(bad_case{std::move(name), header_with(members)}); };

    add("no label", R"("text":"1234","expires_s":300)");
    add("no text", R"("label":"Code","expires_s":300)");
    add("no expires_s", R"("label":"Code","text":"1234")");
    add("an empty label", R"("label":"","text":"1234","expires_s":300)");
    add("an empty text", R"("label":"Code","text":"","expires_s":300)");
    add("a label of 65 bytes",
        R"("label":")" + label65 + R"(","text":"1234","expires_s":300)");
    add("a text of 513 bytes",
        R"("label":"Code","text":")" + text513 + R"(","expires_s":300)");
    add("expires_s 0", R"("label":"Code","text":"1234","expires_s":0)");
    add("expires_s 3601", R"("label":"Code","text":"1234","expires_s":3601)");
    add("expires_s negative", R"("label":"Code","text":"1234","expires_s":-5)");
    add("expires_s a fraction",
        R"("label":"Code","text":"1234","expires_s":300.5)");
    add("expires_s a string",
        R"("label":"Code","text":"1234","expires_s":"300")");
    add("expires_s null", R"("label":"Code","text":"1234","expires_s":null)");
    add("a label that is a number",
        R"("label":5,"text":"1234","expires_s":300)");
    add("a text that is a number",
        R"("label":"Code","text":1234,"expires_s":300)");
    add("a text with a newline",
        R"("label":"Code","text":"12\n34","expires_s":300)");
    add("a text with a carriage return",
        R"("label":"Code","text":"12\r34","expires_s":300)");
    add("a text with an escape character",
        R"("label":"Code","text":"12\u001b[31m34","expires_s":300)");
    add("a text with a tab",
        R"("label":"Code","text":"12\t34","expires_s":300)");
    add("a text with a delete character",
        R"("label":"Code","text":"12\u007f34","expires_s":300)");
    add("a label of control characters only",
        R"("label":"\u0001\u0002\u0003","text":"1234","expires_s":300)");
    add("a label of spaces only",
        R"("label":"     ","text":"1234","expires_s":300)");
    add("a label of zero width characters only",
        R"("label":"\u200b\u200b","text":"1234","expires_s":300)");

    // A header that is not an object cannot come from a decoded frame, but
    // the handler does not rely on that.
    for (const std::string_view header :
         {R"([1,2,3])", R"("secret.show")", R"(42)", R"(null)", R"({)"})
    {
        std::string name = "a header that is ";
        name.append(header);
        cases.push_back(bad_case{std::move(name), std::string(header)});
    }
    return cases;
}

} // namespace

TEST_CASE("plugin secret: a bad request is answered bad_request and never "
          "shown")
{
    for (const bad_case& item : bad_requests())
    {
        rig fixture;
        // A header that is an object has its op and id; one that is not is
        // handed over as it is.
        const bool object = item.header.starts_with("{\"op\"");
        const api_message request =
            object ? decoded(item.header)
                   : raw_request(show_op, first_id, item.header);
        if (!fixture.offer(request))
            throw vss_test::failure("not taken: " + item.name);
        if (!fixture.shown.empty())
            throw vss_test::failure("shown: " + item.name);
        if (fixture.answers.size() != 1)
            throw vss_test::failure("not answered once: " + item.name);
        if (fixture.answers.front().re != first_id)
            throw vss_test::failure("answered the wrong request: " + item.name);
        if (code_of(fixture.answers.front()) !=
            code_name(api_errc::bad_request))
            throw vss_test::failure("not bad_request: " + item.name);
        fixture.pump();
        if (fixture.answers.size() != 1 || !fixture.shown.empty())
            throw vss_test::failure("changed later: " + item.name);
    }
}

TEST_CASE("plugin secret: a message with the op and a body this code knows is "
          "bad_request")
{
    rig fixture;
    api_message request;
    request.op = std::string(show_op);
    request.id = first_id;
    request.body = ping_message{};
    REQUIRE(fixture.offer(request));
    REQUIRE(fixture.shown.empty());
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(code_of(fixture.answers.front()),
               code_name(api_errc::bad_request));
}

TEST_CASE("plugin secret: the limits themselves are good")
{
    const std::string label64(64, 'a');
    const std::string text512(512, 'b');
    const std::vector<std::string> members{
        R"("label":")" + label64 + R"(","text":"1234","expires_s":300)",
        R"("label":"Code","text":")" + text512 + R"(","expires_s":300)",
        R"("label":"Code","text":"1234","expires_s":1)",
        R"("label":"Code","text":"1234","expires_s":3600)",
    };
    for (const std::string& item : members)
    {
        rig fixture;
        REQUIRE(fixture.offer(decoded(header_with(item))));
        REQUIRE_EQ(fixture.shown.size(), std::size_t{1});
        REQUIRE(fixture.answers.empty());
    }
}

TEST_CASE("plugin secret: an op that is not secret.show is not taken")
{
    rig fixture;
    for (const std::string_view op :
         {"secret.hide", "secret", "secret.show.more", "generator.start"})
    {
        std::string header = R"({"op":")";
        header.append(op);
        header.append(R"(","id":7,)");
        header.append(good_members);
        header.push_back('}');
        REQUIRE(!fixture.offer(decoded(header)));
    }
    api_message ping;
    ping.op = "ping";
    ping.id = 3;
    ping.body = ping_message{};
    REQUIRE(!fixture.offer(ping));
    REQUIRE(fixture.shown.empty());
    REQUIRE(fixture.answers.empty());
}

TEST_CASE("plugin secret: a secret.show with id 0 is taken and ignored")
{
    rig fixture;
    const api_message request = raw_request(
        show_op, 0,
        R"({"op":"secret.show",)" + std::string(good_members) + "}");
    REQUIRE(fixture.offer(request));
    fixture.pump();
    REQUIRE(fixture.shown.empty());
    REQUIRE(fixture.answers.empty());

    // And a bad one without an id is not answered either.
    REQUIRE(fixture.offer(raw_request(show_op, 0, R"({"op":"secret.show"})")));
    REQUIRE(fixture.shown.empty());
    REQUIRE(fixture.answers.empty());
}

TEST_CASE("plugin secret: plugin_gone while a secret is shown drops its "
          "answer")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request(7)));
    fixture.handler->plugin_gone();
    finish_shown(fixture, true);
    REQUIRE(fixture.answers.empty());

    // A plugin that starts again may show a secret, and a late answer of the
    // old one neither answers it nor ends it.
    REQUIRE(fixture.offer(good_request(9)));
    REQUIRE_EQ(fixture.shown.size(), std::size_t{2});
    REQUIRE(fixture.answers.empty());
    REQUIRE_EQ(fixture.dones.size(), std::size_t{2});
    fixture.dones.front()(false);
    fixture.pump();
    REQUIRE(fixture.answers.empty());

    // While the second is shown a third is busy.
    REQUIRE(fixture.offer(good_request(10)));
    REQUIRE_EQ(fixture.answers.size(), std::size_t{1});
    REQUIRE_EQ(fixture.answers.front().re, std::uint64_t{10});
    REQUIRE_EQ(code_of(fixture.answers.front()), code_name(api_errc::busy));

    finish_shown(fixture, true);
    REQUIRE_EQ(fixture.answers.size(), std::size_t{2});
    REQUIRE_EQ(fixture.answers.back().re, std::uint64_t{9});
    REQUIRE_EQ(code_of(fixture.answers.back()), std::string("ok"));
}

TEST_CASE("plugin secret: a request after plugin_gone is shown")
{
    rig fixture;
    fixture.handler->plugin_gone();
    REQUIRE(fixture.offer(good_request()));
    REQUIRE_EQ(fixture.shown.size(), std::size_t{1});
}

TEST_CASE("plugin secret: destroying the handler while a secret is shown "
          "leaves a late done harmless")
{
    rig fixture;
    REQUIRE(fixture.offer(good_request()));
    REQUIRE_EQ(fixture.dones.size(), std::size_t{1});
    fixture.handler.reset();
    fixture.dones.front()(true);
    fixture.pump();
    REQUIRE(fixture.answers.empty());

    // The same when done is called before the handler is destroyed but the
    // strand has not run yet.
    rig early;
    REQUIRE(early.offer(good_request()));
    early.dones.front()(true);
    early.handler.reset();
    early.pump();
    REQUIRE(early.answers.empty());
}

TEST_CASE("plugin secret: the secret is in no answer")
{
    const std::string secret = "SECRET-4711-XYZ";
    const std::string members =
        R"("label":"Code","text":")" + secret + R"(","expires_s":300)";
    const std::string with_newline =
        R"("label":"Code","text":")" + secret + R"(\n","expires_s":300)";

    // Seen, not seen, busy, bad_request and no shower.
    rig fixture;
    REQUIRE(fixture.offer(decoded(header_with(members, 1))));
    REQUIRE(fixture.offer(decoded(header_with(members, 2))));
    finish_shown(fixture, true);
    REQUIRE(fixture.offer(decoded(header_with(members, 3))));
    finish_shown(fixture, false);
    REQUIRE(fixture.offer(decoded(header_with(with_newline, 4))));
    rig bare(false);
    REQUIRE(bare.offer(decoded(header_with(members, 5))));

    std::vector<answered> all = fixture.answers;
    all.insert(all.end(), bare.answers.begin(), bare.answers.end());
    REQUIRE_EQ(all.size(), std::size_t{5});
    for (const answered& item : all)
    {
        const std::string wire = wire_text_of(item);
        REQUIRE(!contains(wire, "SECRET-4711"));
        REQUIRE(!contains(wire, "4711"));
    }
    // The secret was given to the shower, and to nobody else.
    REQUIRE_EQ(fixture.shown.size(), std::size_t{2});
    REQUIRE_EQ(fixture.shown.front().text, secret);
}
