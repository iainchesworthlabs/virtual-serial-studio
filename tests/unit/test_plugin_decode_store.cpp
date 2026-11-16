//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/decode_store.hpp>

#include "test_framework.hpp"

#include <boost/asio/io_context.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace vss::plugin::host;

namespace
{

constexpr std::uint64_t second = 1'000'000'000;

decoded_frame frame(std::uint64_t offset = 0)
{
    decoded_frame out;
    out.place = "COM7";
    out.offset = offset;
    out.length = 10;
    out.t_start_ns = 100 + offset;
    out.t_end_ns = 200 + offset;
    out.message = "read reply";
    out.summary = "ok";
    out.fields.push_back({"check", 8, 2, "3A91"});
    return out;
}

decoded_trigger trigger(std::uint64_t when = 5)
{
    decoded_trigger out;
    out.kind = decode_trigger_kind::check;
    out.when_ns = when;
    out.place = "bus-1";
    out.text = "Check value is 3A90, expected 3A91.";
    out.expected = "3A91";
    out.actual = "3A90";
    return out;
}

/// Limits wide enough that only the one under test applies.
decode_limits wide()
{
    decode_limits limits;
    limits.frames_per_second = 1'000'000;
    limits.triggers_per_second = 1'000'000;
    return limits;
}

add_result add_one(decode_store& store, const decoded_frame& value,
                   std::uint64_t now = 0)
{
    return store.add_records(std::span<const decoded_frame>(&value, 1), now);
}

add_result add_one(decode_store& store, const decoded_trigger& value,
                   std::uint64_t now = 0)
{
    return store.add_triggers(std::span<const decoded_trigger>(&value, 1),
                              now);
}

/// Whether the frame is taken (true) or refused (false).
bool taken(const decoded_frame& value)
{
    decode_store store(wide());
    return add_one(store, value).accepted == 1;
}

bool taken(const decoded_trigger& value)
{
    decode_store store(wide());
    return add_one(store, value).accepted == 1;
}

reject_reason refusal(const decoded_frame& value)
{
    decode_store store(wide());
    return add_one(store, value).first_reject;
}

reject_reason refusal(const decoded_trigger& value)
{
    decode_store store(wide());
    return add_one(store, value).first_reject;
}

} // namespace

TEST_CASE("decode store: frames and triggers keep their order and numbers")
{
    decode_store store;
    std::vector<decoded_frame> frames = {frame(0), frame(10), frame(20)};
    const add_result result = store.add_records(frames, 7);
    REQUIRE_EQ(result.accepted, std::size_t{3});
    REQUIRE_EQ(result.rejected, std::size_t{0});
    add_one(store, trigger(1), 8);
    add_one(store, trigger(2), 9);

    const auto view = store.snapshot();
    REQUIRE_EQ(view->frames->items.size(), std::size_t{3});
    for (std::size_t i = 0; i < 3; ++i)
    {
        REQUIRE_EQ(view->frames->items[i]->sequence, std::uint64_t{i + 1});
        REQUIRE_EQ(view->frames->items[i]->received_ns, std::uint64_t{7});
        REQUIRE(view->frames->items[i]->value == frames[i]);
    }
    REQUIRE_EQ(view->frames->first_sequence, std::uint64_t{1});
    REQUIRE_EQ(view->frames->next_sequence, std::uint64_t{4});
    // Triggers are numbered on their own.
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{2});
    REQUIRE_EQ(view->triggers->items[0]->sequence, std::uint64_t{1});
    REQUIRE_EQ(view->triggers->items[1]->value.when_ns, std::uint64_t{2});
    REQUIRE_EQ(view->triggers->items[1]->received_ns, std::uint64_t{9});
}

TEST_CASE("decode store: a frame's limits at the edge and one past")
{
    REQUIRE(taken(frame()));

    decoded_frame value = frame();
    value.place.assign(128, 'p');
    REQUIRE(taken(value));
    value.place.assign(129, 'p');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.place.clear();
    REQUIRE(refusal(value) == reject_reason::text_empty);

    value = frame();
    value.message.assign(64, 'm');
    REQUIRE(taken(value));
    value.message.assign(65, 'm');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.message.clear();
    REQUIRE(refusal(value) == reject_reason::text_empty);

    value = frame();
    value.summary.assign(200, 's');
    REQUIRE(taken(value));
    value.summary.assign(201, 's');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.summary.clear();
    REQUIRE(taken(value));

    value = frame();
    value.length = 65536;
    REQUIRE(taken(value));
    value.length = 65537;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);
    value.length = 0;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);

    value = frame();
    value.offset = (std::uint64_t{1} << 53) - 1 - value.length;
    REQUIRE(taken(value));
    value.offset += 1;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);

    value = frame();
    value.t_start_ns = (std::uint64_t{1} << 53) - 1;
    value.t_end_ns = value.t_start_ns;
    REQUIRE(taken(value));
    value.t_end_ns += 1;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);

    value = frame();
    value.t_end_ns = value.t_start_ns;
    REQUIRE(taken(value));
    value.t_end_ns = value.t_start_ns - 1;
    REQUIRE(refusal(value) == reject_reason::bad_combination);
}

TEST_CASE("decode store: a frame's fields at the edge and one past")
{
    decoded_frame value = frame();
    value.fields.assign(64, decode_field{"f", 0, 1, "v"});
    REQUIRE(taken(value));
    value.fields.push_back({"f", 0, 1, "v"});
    REQUIRE(refusal(value) == reject_reason::bad_combination);

    value = frame();
    value.fields[0] = {"f", 8, 2, "v"};
    REQUIRE(taken(value));
    value.fields[0] = {"f", 8, 3, "v"};
    REQUIRE(refusal(value) == reject_reason::bad_combination);
    value.fields[0] = {"f", 11, 0, "v"};
    REQUIRE(refusal(value) == reject_reason::bad_combination);
    value.fields[0] = {"f", 10, 0, "v"};
    REQUIRE(taken(value));

    value = frame();
    value.fields[0].name.assign(64, 'n');
    REQUIRE(taken(value));
    value.fields[0].name.assign(65, 'n');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.fields[0].name.clear();
    REQUIRE(refusal(value) == reject_reason::text_empty);

    value = frame();
    value.fields[0].value.assign(200, 'v');
    REQUIRE(taken(value));
    value.fields[0].value.assign(201, 'v');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.fields[0].value.clear();
    REQUIRE(taken(value));
}

TEST_CASE("decode store: a trigger's limits at the edge and one past")
{
    REQUIRE(taken(trigger()));

    decoded_trigger value = trigger();
    value.text.assign(200, 't');
    REQUIRE(taken(value));
    value.text.assign(201, 't');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.text.clear();
    REQUIRE(refusal(value) == reject_reason::text_empty);

    value = trigger();
    value.expected.assign(128, 'a');
    value.actual.assign(128, 'b');
    REQUIRE(taken(value));
    value.actual.assign(129, 'b');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value = trigger();
    value.actual.clear();
    REQUIRE(refusal(value) == reject_reason::bad_combination);
    value.expected.clear();
    REQUIRE(taken(value));

    value = trigger();
    value.place.assign(128, 'p');
    value.sender.assign(128, 's');
    REQUIRE(taken(value));
    value.sender.assign(129, 's');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value = trigger();
    value.place.assign(129, 'p');
    REQUIRE(refusal(value) == reject_reason::text_too_long);

    value = trigger();
    value.when_ns = (std::uint64_t{1} << 53) - 1;
    REQUIRE(taken(value));
    value.when_ns += 1;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);

    value = trigger();
    value.protocol = decode_protocol{std::string(128, 'i'),
                                     std::string(64, 'v')};
    REQUIRE(taken(value));
    value.protocol->id.assign(129, 'i');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.protocol->id.assign(128, 'i');
    value.protocol->version.assign(65, 'v');
    REQUIRE(refusal(value) == reject_reason::text_too_long);
    value.protocol->version.clear();
    REQUIRE(refusal(value) == reject_reason::text_empty);

    value = trigger();
    value.frame = decode_trigger_frame{0, 65536, 5, 5, std::string(64, 'm')};
    REQUIRE(taken(value));
    value.frame->length = 65537;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);
    value.frame->length = 0;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);
    value.frame->length = 1;
    value.frame->t_end_ns = 4;
    REQUIRE(refusal(value) == reject_reason::bad_combination);
    value.frame->t_end_ns = 5;
    value.frame->message.assign(65, 'm');
    REQUIRE(refusal(value) == reject_reason::text_too_long);

    value = trigger();
    value.field = decode_trigger_field{"check", 7, 2};
    REQUIRE(taken(value));
    // A field of no bytes is allowed, as the message allows it.
    value.field->length = 0;
    REQUIRE(taken(value));
    value.field->length = 65'537;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);
    value.field->length = 2;
    value.field->name.clear();
    REQUIRE(refusal(value) == reject_reason::text_empty);

    value = trigger();
    value.covers = decode_covers{1, 1};
    REQUIRE(taken(value));
    value.covers = decode_covers{2, 1};
    REQUIRE(refusal(value) == reject_reason::bad_combination);

    value = trigger();
    value.request = decode_request{0, 8, 9};
    REQUIRE(taken(value));
    value.request->length = 65537;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);

    value = trigger();
    decode_cause cause;
    cause.which = decode_cause::kind::bus_fault;
    cause.fault = decode_fault::overrun;
    cause.event = (std::uint64_t{1} << 53) - 1;
    value.cause = cause;
    REQUIRE(taken(value));
    value.cause->event += 1;
    REQUIRE(refusal(value) == reject_reason::number_out_of_range);
}

TEST_CASE("decode store: a custom trigger has a code and no other does")
{
    decoded_trigger value = trigger();
    value.kind = decode_trigger_kind::custom;
    REQUIRE(refusal(value) == reject_reason::bad_combination);
    value.code = "my-code_1.x";
    REQUIRE(taken(value));
    value.code.assign(64, 'c');
    REQUIRE(taken(value));
    value.code.assign(65, 'c');
    REQUIRE(refusal(value) == reject_reason::bad_value);
    value.code = "Upper";
    REQUIRE(refusal(value) == reject_reason::bad_value);
    value.code = "has space";
    REQUIRE(refusal(value) == reject_reason::bad_value);
    value.kind = decode_trigger_kind::timeout;
    value.code = "x";
    REQUIRE(refusal(value) == reject_reason::bad_combination);
}

TEST_CASE("decode store: a batch over the batch limit is cut and counted")
{
    decode_store store(wide());
    std::vector<decoded_frame> batch(max_decode_batch, frame());
    add_result result = store.add_records(batch, 0);
    REQUIRE_EQ(result.accepted, max_decode_batch);
    REQUIRE_EQ(result.rejected, std::size_t{0});

    batch.push_back(frame());
    result = store.add_records(batch, 0);
    REQUIRE_EQ(result.accepted, max_decode_batch);
    REQUIRE_EQ(result.rejected, std::size_t{1});
    REQUIRE(result.first_reject == reject_reason::batch_too_large);
    REQUIRE_EQ(result.first_reject_index, max_decode_batch);
}

TEST_CASE("decode store: a bad item does not stop the good ones")
{
    decode_store store;
    std::vector<decoded_frame> batch = {frame(0), frame(10), frame(20)};
    batch[1].message.clear();
    const add_result result = store.add_records(batch, 0);
    REQUIRE_EQ(result.accepted, std::size_t{2});
    REQUIRE_EQ(result.rejected, std::size_t{1});
    REQUIRE_EQ(result.first_reject_index, std::size_t{1});
    REQUIRE(result.first_reject == reject_reason::text_empty);

    const auto view = store.snapshot();
    REQUIRE_EQ(view->frames->items.size(), std::size_t{2});
    REQUIRE_EQ(view->frames->items[1]->value.offset, std::uint64_t{20});
    REQUIRE_EQ(view->frames->items[1]->sequence, std::uint64_t{2});
    REQUIRE_EQ(view->frames->rejected, std::uint64_t{1});
    REQUIRE(view->frames->last_reject == reject_reason::text_empty);
}

TEST_CASE("decode store: the oldest are dropped by count, with exact counts")
{
    decode_limits limits = wide();
    limits.max_frames = 3;
    decode_store store(limits);

    std::vector<decoded_frame> first = {frame(0), frame(1)};
    REQUIRE_EQ(store.add_records(first, 0).dropped, std::size_t{0});

    std::vector<decoded_frame> later = {frame(2), frame(3), frame(4)};
    const add_result result = store.add_records(later, 0);
    REQUIRE_EQ(result.accepted, std::size_t{3});
    REQUIRE_EQ(result.dropped, std::size_t{2});

    const auto view = store.snapshot();
    REQUIRE_EQ(view->frames->items.size(), std::size_t{3});
    REQUIRE_EQ(view->frames->first_sequence, std::uint64_t{3});
    REQUIRE_EQ(view->frames->next_sequence, std::uint64_t{6});
    REQUIRE_EQ(view->frames->dropped, std::uint64_t{2});
    REQUIRE_EQ(view->frames->items.front()->value.offset, std::uint64_t{2});

    // A batch larger than the whole store keeps only its newest.
    std::vector<decoded_frame> big;
    for (std::uint64_t i = 10; i < 20; ++i)
    {
        big.push_back(frame(i));
    }
    store.add_records(big, 0);
    const auto after = store.snapshot();
    REQUIRE_EQ(after->frames->items.size(), std::size_t{3});
    REQUIRE_EQ(after->frames->items.front()->value.offset, std::uint64_t{17});
    REQUIRE_EQ(after->frames->dropped, std::uint64_t{2 + 10});
    REQUIRE_EQ(after->frames->first_sequence, std::uint64_t{13});
}

TEST_CASE("decode store: the oldest are dropped by bytes")
{
    const std::size_t one = [] {
        decode_store probe;
        add_one(probe, trigger());
        return probe.snapshot()->triggers->bytes;
    }();
    REQUIRE(one > 0);

    decode_limits limits = wide();
    limits.max_trigger_bytes = one * 2;
    decode_store store(limits);
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        add_one(store, trigger(i));
    }
    const auto view = store.snapshot();
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{2});
    REQUIRE_EQ(view->triggers->bytes, one * 2);
    REQUIRE_EQ(view->triggers->dropped, std::uint64_t{3});
    REQUIRE_EQ(view->triggers->items.front()->value.when_ns,
               std::uint64_t{3});

    // One byte less than two items' worth holds one.
    limits.max_trigger_bytes = one * 2 - 1;
    decode_store tight(limits);
    for (std::uint64_t i = 0; i < 3; ++i)
    {
        add_one(tight, trigger(i));
    }
    REQUIRE_EQ(tight.snapshot()->triggers->items.size(), std::size_t{1});

    // An item larger than the whole limit is dropped at once.
    limits.max_trigger_bytes = one - 1;
    decode_store none(limits);
    const add_result result = add_one(none, trigger());
    REQUIRE_EQ(result.accepted, std::size_t{1});
    REQUIRE_EQ(result.dropped, std::size_t{1});
    REQUIRE(none.snapshot()->triggers->items.empty());
    REQUIRE_EQ(none.snapshot()->triggers->first_sequence, std::uint64_t{2});
}

TEST_CASE("decode store: triggers have a budget of 200 a second")
{
    decode_store store;
    std::vector<decoded_trigger> batch(250, trigger());

    add_result result = store.add_triggers(batch, 5 * second);
    REQUIRE_EQ(result.accepted, std::size_t{200});
    REQUIRE_EQ(result.over_budget, std::size_t{50});
    REQUIRE_EQ(result.rejected, std::size_t{0});

    // The rest of the same second is spent.
    result = store.add_triggers(batch, 6 * second - 1);
    REQUIRE_EQ(result.accepted, std::size_t{0});
    REQUIRE_EQ(result.over_budget, std::size_t{250});

    // The next second refills it.
    result = store.add_triggers(std::span(batch).first(199), 6 * second);
    REQUIRE_EQ(result.accepted, std::size_t{199});
    result = store.add_triggers(std::span(batch).first(2), 6 * second + 5);
    REQUIRE_EQ(result.accepted, std::size_t{1});
    REQUIRE_EQ(result.over_budget, std::size_t{1});

    const auto view = store.snapshot();
    REQUIRE_EQ(view->triggers->over_budget, std::uint64_t{50 + 250 + 1});
    REQUIRE_EQ(view->triggers->items.size(), std::size_t{400});
    // What was cut got no sequence number.
    REQUIRE_EQ(view->triggers->next_sequence, std::uint64_t{401});
}

TEST_CASE("decode store: a bad item does not use the budget")
{
    decode_limits limits;
    limits.triggers_per_second = 2;
    decode_store store(limits);
    std::vector<decoded_trigger> batch = {trigger(), trigger(), trigger(),
                                          trigger()};
    batch[0].text.clear();
    const add_result result = store.add_triggers(batch, 0);
    REQUIRE_EQ(result.rejected, std::size_t{1});
    REQUIRE_EQ(result.accepted, std::size_t{2});
    REQUIRE_EQ(result.over_budget, std::size_t{1});
}

TEST_CASE("decode store: frames have a budget of their own")
{
    decode_limits limits;
    limits.frames_per_second = 3;
    decode_store store(limits);
    std::vector<decoded_frame> batch(5, frame());
    REQUIRE_EQ(store.add_records(batch, 0).accepted, std::size_t{3});
    REQUIRE_EQ(store.add_records(batch, second - 1).accepted, std::size_t{0});
    REQUIRE_EQ(store.add_records(batch, second).accepted, std::size_t{3});
    // The triggers' budget is not touched.
    std::vector<decoded_trigger> triggers(10, trigger());
    REQUIRE_EQ(store.add_triggers(triggers, 0).accepted, std::size_t{10});
}

TEST_CASE("decode store: hostile text is kept as it came and flagged")
{
    decode_store store;

    decoded_frame plain = frame();
    add_one(store, plain);

    decoded_frame control = frame();
    control.summary = std::string("line\nfeed \x1b[31m and \x7f and nul") +
                      std::string(1, '\0') + "end";
    control.fields[0].value = "tab\there";
    add_one(store, control);

    decoded_frame bidi = frame();
    // A right-to-left override, U+202E, put together byte by byte.
    bidi.message = "gnp.exe\xE2\x80";
    bidi.message += static_cast<char>(0xAE);
    bidi.message += "txt";
    add_one(store, bidi);

    decoded_frame markup = frame();
    markup.summary = "<script>alert(1)</script> &amp; %s {0} \\u0041 ${x}";
    add_one(store, markup);

    decoded_frame c1 = frame();
    c1.place = "a\xC2\x85" "b";  // U+0085, a C1 control
    add_one(store, c1);

    decoded_frame isolate = frame();
    isolate.summary = "\xE2\x81\xA6x\xE2\x81\xA9";  // U+2066 and U+2069
    add_one(store, isolate);

    decoded_frame wide_chars = frame();
    wide_chars.summary = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D \xE2\x82\xAC "
                         "\xF0\x9F\x98\x80";
    add_one(store, wide_chars);

    const auto view = store.snapshot();
    REQUIRE_EQ(view->frames->items.size(), std::size_t{7});
    const auto& items = view->frames->items;
    REQUIRE(!items[0]->marks.control && !items[0]->marks.bidi);
    REQUIRE(items[1]->marks.control && !items[1]->marks.bidi);
    REQUIRE(!items[2]->marks.control && items[2]->marks.bidi);
    REQUIRE(!items[3]->marks.control && !items[3]->marks.bidi);
    REQUIRE(items[4]->marks.control);
    REQUIRE(items[5]->marks.bidi);
    REQUIRE(!items[6]->marks.control && !items[6]->marks.bidi);

    // Nothing was changed.
    REQUIRE(items[1]->value == control);
    REQUIRE(items[2]->value == bidi);
    REQUIRE(items[3]->value == markup);
    REQUIRE(items[4]->value == c1);
    REQUIRE(items[5]->value == isolate);
    REQUIRE(items[6]->value == wide_chars);

    decoded_trigger flagged = trigger();
    flagged.text = "a\x01 b";
    flagged.sender = "x\xE2\x80\x8F";  // right-to-left mark
    add_one(store, flagged);
    const auto triggers = store.snapshot()->triggers;
    REQUIRE(triggers->items[0]->marks.control);
    REQUIRE(triggers->items[0]->marks.bidi);
    REQUIRE(triggers->items[0]->value == flagged);
}

TEST_CASE("decode store: text that is not valid UTF-8 is refused")
{
    const std::vector<std::string> bad = {
        "\xC0\x80",          // overlong NUL
        "\xC1\xBF",          // overlong
        "\xE0\x80\x80",      // overlong
        "\xED\xA0\x80",      // a surrogate
        "\xF4\x90\x80\x80",  // past U+10FFFF
        "\xF5\x80\x80\x80",  // lead byte that cannot occur
        "\x80",              // a continuation with no lead
        "abc\xE2\x82",       // cut short
        "\xFF",
    };
    for (const std::string& text : bad)
    {
        decoded_frame value = frame();
        value.summary = text;
        REQUIRE(refusal(value) == reject_reason::invalid_utf8);
        decoded_trigger event = trigger();
        event.text = text;
        REQUIRE(refusal(event) == reject_reason::invalid_utf8);
        REQUIRE(!check_text(text).valid_utf8);
    }
    REQUIRE(check_text("").valid_utf8);
    REQUIRE(check_text("\xF4\x8F\xBF\xBF").valid_utf8);  // U+10FFFF
    REQUIRE(check_text("\xEF\xBF\xBD").valid_utf8);      // U+FFFD

    // The length limit is of bytes, and is looked at before the text.
    decoded_frame many = frame();
    many.summary.assign(200, '\x80');
    REQUIRE(refusal(many) == reject_reason::invalid_utf8);
    many.summary.assign(10'000, 'x');
    REQUIRE(refusal(many) == reject_reason::text_too_long);
}

TEST_CASE("decode store: a snapshot never changes while writers add")
{
    decode_limits limits = wide();
    limits.max_frames = 4;
    decode_store store(limits);
    add_one(store, frame(0));
    const auto before = store.snapshot();
    REQUIRE_EQ(before->frames->items.size(), std::size_t{1});

    for (std::uint64_t i = 1; i < 20; ++i)
    {
        add_one(store, frame(i));
        add_one(store, trigger(i));
    }
    store.clear();

    REQUIRE_EQ(before->frames->items.size(), std::size_t{1});
    REQUIRE_EQ(before->frames->items[0]->sequence, std::uint64_t{1});
    REQUIRE_EQ(before->frames->next_sequence, std::uint64_t{2});
    REQUIRE_EQ(before->frames->dropped, std::uint64_t{0});
    REQUIRE(before->triggers->items.empty());
    REQUIRE(store.snapshot() != before);
}

TEST_CASE("decode store: since gives what came after, and says what is gone")
{
    decode_limits limits = wide();
    limits.max_frames = 4;
    decode_store store(limits);

    auto none = store.records_since(0);
    REQUIRE(none.items.empty());
    REQUIRE(!none.gap);
    REQUIRE_EQ(none.last_sequence, std::uint64_t{0});

    for (std::uint64_t i = 0; i < 3; ++i)
    {
        add_one(store, frame(i));
    }
    auto all = store.records_since(0);
    REQUIRE_EQ(all.items.size(), std::size_t{3});
    REQUIRE(!all.gap);
    REQUIRE_EQ(all.last_sequence, std::uint64_t{3});

    auto tail = store.records_since(2);
    REQUIRE_EQ(tail.items.size(), std::size_t{1});
    REQUIRE_EQ(tail.items[0]->sequence, std::uint64_t{3});
    REQUIRE(!tail.gap);

    auto caught_up = store.records_since(3);
    REQUIRE(caught_up.items.empty());
    REQUIRE(!caught_up.gap);
    REQUIRE(store.records_since(1'000).items.empty());
    REQUIRE(store.records_since(~std::uint64_t{0}).items.empty());

    // Sequences 1 and 2 are dropped, 3 to 6 are held.
    for (std::uint64_t i = 3; i < 6; ++i)
    {
        add_one(store, frame(i));
    }
    REQUIRE_EQ(store.snapshot()->frames->first_sequence, std::uint64_t{3});

    // A reader that had 2 lost nothing; one that had 1 lost the second.
    auto from_two = store.records_since(2);
    REQUIRE_EQ(from_two.items.size(), std::size_t{4});
    REQUIRE(!from_two.gap);
    auto from_one = store.records_since(1);
    REQUIRE_EQ(from_one.items.size(), std::size_t{4});
    REQUIRE(from_one.gap);
    auto from_start = store.records_since(0);
    REQUIRE(from_start.gap);
    REQUIRE_EQ(from_start.items.front()->sequence, std::uint64_t{3});
    REQUIRE_EQ(from_start.last_sequence, std::uint64_t{6});

    // Triggers have their own.
    add_one(store, trigger());
    REQUIRE_EQ(store.triggers_since(0).items.size(), std::size_t{1});
    REQUIRE(!store.triggers_since(0).gap);
}

TEST_CASE("decode store: clear empties the store and numbers go on")
{
    decode_store store;
    add_one(store, frame());
    add_one(store, frame());
    add_one(store, trigger());
    store.clear();

    auto view = store.snapshot();
    REQUIRE(view->frames->items.empty());
    REQUIRE(view->triggers->items.empty());
    REQUIRE_EQ(view->frames->bytes, std::size_t{0});
    REQUIRE_EQ(view->frames->dropped, std::uint64_t{0});
    REQUIRE_EQ(view->frames->first_sequence, std::uint64_t{3});
    REQUIRE_EQ(view->frames->next_sequence, std::uint64_t{3});

    add_one(store, frame());
    auto after = store.snapshot();
    REQUIRE_EQ(after->frames->items[0]->sequence, std::uint64_t{3});
    // A reader that had the first two sees that something is gone.
    REQUIRE(store.records_since(1).gap);
    REQUIRE(!store.records_since(2).gap);
}

TEST_CASE("decode store: clear gives the budget back")
{
    decode_limits limits;
    limits.triggers_per_second = 2;
    decode_store store(limits);
    std::vector<decoded_trigger> batch(3, trigger());
    REQUIRE_EQ(store.add_triggers(batch, 0).accepted, std::size_t{2});
    store.clear();
    REQUIRE_EQ(store.add_triggers(batch, 0).accepted, std::size_t{2});
}

TEST_CASE("decode store: readers on many threads and one writer")
{
    decode_limits limits = wide();
    limits.max_frames = 50;
    limits.max_triggers = 50;
    decode_store store(limits);

    constexpr std::uint64_t writes = 600;
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> broken{0};

    std::vector<std::thread> readers;
    readers.reserve(6);
    for (int r = 0; r < 6; ++r)
    {
        readers.emplace_back([&store, &done, &broken] {
            std::uint64_t seen = 0;
            while (!done.load())
            {
                const auto view = store.snapshot();
                const auto& window = *view->frames;
                // The numbers are contiguous and agree with the counts.
                if (window.next_sequence != window.first_sequence +
                                                window.items.size())
                {
                    broken.fetch_add(1);
                }
                for (std::size_t i = 0; i < window.items.size(); ++i)
                {
                    if (window.items[i]->sequence !=
                        window.first_sequence + i)
                    {
                        broken.fetch_add(1);
                    }
                }
                if (window.next_sequence < seen)
                {
                    broken.fetch_add(1);
                }
                seen = window.next_sequence;
                const auto more = store.records_since(seen > 5 ? seen - 5 : 0);
                if (!more.items.empty() && more.items.back()->sequence >
                                               more.last_sequence)
                {
                    broken.fetch_add(1);
                }
                (void)store.triggers_since(0);
            }
        });
    }

    std::thread writer([&store] {
        for (std::uint64_t i = 0; i < writes; ++i)
        {
            std::vector<decoded_frame> batch = {frame(i), frame(i + 1000)};
            store.add_records(batch, i);
            add_one(store, trigger(i), i);
            if (i == 300)
            {
                store.clear();
            }
        }
    });
    writer.join();
    done.store(true);
    for (std::thread& reader : readers)
    {
        reader.join();
    }
    REQUIRE_EQ(broken.load(), std::uint64_t{0});
    REQUIRE_EQ(store.snapshot()->frames->next_sequence,
               std::uint64_t{2 * writes + 1});
}

TEST_CASE("decode store: a notice is posted to the executor and coalesced")
{
    boost::asio::io_context context;
    decode_store store;
    int calls = 0;
    std::thread::id ran_on;
    store.set_notify(context.get_executor(), [&] {
        ++calls;
        ran_on = std::this_thread::get_id();
    });

    // Nothing is told for a change that did not happen.
    std::vector<decoded_frame> nothing;
    store.add_records(nothing, 0);
    context.poll();
    REQUIRE_EQ(calls, 0);

    // Three changes before the executor runs are one call, and that call is
    // on the executor's thread and not on the writer's.
    std::thread writer([&store] {
        add_one(store, frame());
        add_one(store, trigger());
        store.clear();
    });
    writer.join();
    REQUIRE_EQ(calls, 0);
    context.restart();
    context.poll();
    REQUIRE_EQ(calls, 1);
    REQUIRE(ran_on == std::this_thread::get_id());
    REQUIRE(ran_on != writer.get_id());

    // The call sees everything that was added.
    context.restart();
    add_one(store, frame());
    context.poll();
    REQUIRE_EQ(calls, 2);

    // A refused item is still a change, since the counts show it.
    context.restart();
    decoded_frame bad = frame();
    bad.message.clear();
    add_one(store, bad);
    context.poll();
    REQUIRE_EQ(calls, 3);

    // An empty callback stops them.
    store.set_notify(context.get_executor(), nullptr);
    context.restart();
    add_one(store, frame());
    context.poll();
    REQUIRE_EQ(calls, 3);
}

TEST_CASE("decode store: a notice that is pending when the store ends is "
          "not called")
{
    boost::asio::io_context context;
    int calls = 0;
    {
        decode_store store;
        store.set_notify(context.get_executor(), [&calls] { ++calls; });
        add_one(store, frame());
    }
    context.poll();
    REQUIRE_EQ(calls, 0);
}

TEST_CASE("decode store: a snapshot outlives its store")
{
    std::shared_ptr<const decode_snapshot> kept;
    {
        decode_store store;
        add_one(store, frame());
        kept = store.snapshot();
    }
    REQUIRE_EQ(kept->frames->items.size(), std::size_t{1});
    REQUIRE_EQ(kept->frames->items[0]->value.place, "COM7");
}
