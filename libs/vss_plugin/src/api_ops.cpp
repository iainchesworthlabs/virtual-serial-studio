//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/api_generator.hpp>
#include <vss/plugin/api_instance.hpp>
#include <vss/plugin/api_message.hpp>

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

template <class M>
result<api_message> recode_as(const api_message& message)
{
    const auto read = decode_as<M>(message);
    if (read.has_error())
        return read.error();
    return encode_as<M>(read.value(), M::role == api_role::request
                                          ? message.id
                                          : message.re);
}

template <class M>
constexpr api_op entry() noexcept
{
    return api_op{M::op, M::from, M::role, &check_as<M>, &recode_as<M>};
}

constexpr std::array ops{
    entry<instance_create_message>(),  entry<instance_stop_message>(),
    entry<params_changed_message>(),   entry<generator_start_message>(),
    entry<generator_write_message>(),  entry<generator_credit_message>(),
    entry<generator_pattern_message>(), entry<generator_stop_message>(),
    entry<generator_done_message>()};

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
