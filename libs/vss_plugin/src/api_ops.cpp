//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_adapter.hpp>
#include <vss/plugin/api_capture.hpp>
#include <vss/plugin/api_decoder.hpp>
#include <vss/plugin/api_fixture.hpp>
#include <vss/plugin/api_generator.hpp>
#include <vss/plugin/api_instance.hpp>
#include <vss/plugin/api_message.hpp>
#include <vss/plugin/api_modifier.hpp>

#include <algorithm>
#include <array>

namespace vss::plugin
{

namespace
{

template <class M>
boost::system::error_code check_as(const api_message& message,
                                   std::string* detail)
{
    const auto read = decode_as<M>(message, detail);
    return read.has_error() ? read.error() : boost::system::error_code{};
}

template <class M> result<api_message> recode_as(const api_message& message)
{
    const auto read = decode_as<M>(message);
    if (read.has_error())
        return read.error();
    return encode_as<M>(
        read.value(), M::role() == api_role::request ? message.id : message.re);
}

template <class M> constexpr api_op entry() noexcept
{
    return api_op{M::op(), M::from(), M::role(), &check_as<M>, &recode_as<M>};
}

constexpr std::array ops{
    entry<instance_create_message>(),   entry<instance_stop_message>(),
    entry<params_changed_message>(),    entry<generator_start_message>(),
    entry<generator_write_message>(),   entry<generator_credit_message>(),
    entry<generator_pattern_message>(), entry<generator_stop_message>(),
    entry<generator_done_message>(),    entry<fixture_create_message>(),
    entry<fixture_data_message>(),      entry<fixture_line_message>(),
    entry<fixture_reply_message>(),     entry<fixture_line_set_message>(),
    entry<fixture_fault_message>(),     entry<rules_set_message>(),
    entry<rules_result_message>(),      entry<rules_event_message>(),
    entry<modifier_create_message>(),   entry<modifier_batch_message>(),
    entry<modifier_result_message>(),   entry<modifier_stop_message>(),
    entry<agent_call_message>(),        entry<agent_result_message>(),
    entry<agent_subscribe_message>(),   entry<adapter_state_message>(),
    entry<capture_start_message>(),     entry<capture_stop_message>(),
    entry<capture_status_message>(),    entry<capture_result_message>(),
    entry<timeline_load_message>(),     entry<bundle_prepare_message>(),
    entry<bundle_result_message>(),     entry<decoder_create_message>(),
    entry<decoder_data_message>(),      entry<decoder_ack_message>(),
    entry<decoder_frame_message>(),     entry<trigger_publish_message>(),
    entry<decoder_stop_message>()};

} // namespace

std::span<const api_op> api_ops() noexcept
{
    return ops;
}

const api_op* find_api_op(std::string_view op) noexcept
{
    const auto found = std::ranges::find(ops, op, &api_op::op);
    return found == ops.end() ? nullptr : &*found;
}

} // namespace vss::plugin
