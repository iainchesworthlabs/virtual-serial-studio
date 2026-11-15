//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Fuzz harness for the values a user gives to a plugin.  Any bytes may be in a
// setup file or arrive from a plugin as the `params` of a message, so the
// reader must refuse bad input with a reason and never crash.  What is
// accepted is inside every limit.  Each value that was read is then checked
// as every type a parameter may have; what passes is written, read and
// checked again, and must come out equal, with the same text.
//

#include <vss/plugin/manifest.hpp>
#include <vss/plugin/parameters.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace
{

namespace plugin = vss::plugin;

/// A property did not hold: stop, so the fuzzer keeps the input.
void require(bool holds)
{
    if (!holds)
    {
        std::abort();
    }
}

/// One declared parameter of each type, with the id of the value.
std::vector<plugin::parameter> specimens(const std::string& id)
{
    std::vector<plugin::parameter> out;
    const auto add = [&](plugin::parameter_type type) -> plugin::parameter& {
        plugin::parameter item;
        item.id = id;
        item.type = type;
        item.max_length = 64;
        out.push_back(std::move(item));
        return out.back();
    };
    add(plugin::parameter_type::boolean);
    {
        plugin::parameter& item = add(plugin::parameter_type::integer);
        item.min = std::int64_t{-1000};
        item.max = std::int64_t{100000};
    }
    {
        plugin::parameter& item = add(plugin::parameter_type::number);
        item.min = -1.0e6;
        item.max = 1.0e6;
    }
    {
        plugin::parameter& item = add(plugin::parameter_type::duration);
        item.max = std::int64_t{1'000'000'000};
    }
    {
        plugin::parameter& item = add(plugin::parameter_type::choice);
        item.choices.push_back({"a", {}});
        item.choices.push_back({"b", {}});
    }
    add(plugin::parameter_type::string);
    add(plugin::parameter_type::bytes);
    add(plugin::parameter_type::port);
    add(plugin::parameter_type::link);
    add(plugin::parameter_type::secret);
    add(plugin::parameter_type::path);
    {
        plugin::parameter& item = add(plugin::parameter_type::path);
        item.kind = plugin::path_kind::directory;
        item.access = plugin::path_access::write;
    }
    return out;
}

void require_checked(const std::vector<plugin::checked_parameter>& values)
{
    for (const plugin::checked_parameter& item : values)
    {
        require(!std::holds_alternative<std::monostate>(item.value));
        require((item.type == plugin::parameter_type::path) ==
                item.scope.has_value());
        if (item.scope)
        {
            const auto* const text = std::get_if<std::string>(&item.value);
            require(text != nullptr);
            require(*text == item.scope->path);
            require(!item.scope->path.empty());
            require(item.scope->path.size() <=
                    plugin::max_path_parameter_length);
        }
        if (const auto* const text = std::get_if<std::string>(&item.value);
            text != nullptr && item.type == plugin::parameter_type::string)
        {
            require(text->size() <= 64);
        }
        if (const auto* const ns = std::get_if<std::int64_t>(&item.value);
            ns != nullptr && item.type == plugin::parameter_type::duration)
        {
            require(*ns >= 0 && *ns <= 1'000'000'000);
        }
    }
}

void check_one_value(const plugin::raw_parameter& raw)
{
    const std::vector<plugin::parameter> declared = specimens(raw.id);
    for (const plugin::parameter& one : declared)
    {
        const std::vector<plugin::parameter> alone{one};
        const std::vector<plugin::raw_parameter> given{raw};
        const plugin::validation first = plugin::validate(alone, given);
        if (!first.ok())
        {
            require(first.values.empty());
            require(!first.errors.empty());
            for (const plugin::parameter_error& error : first.errors)
            {
                require(!error.line.empty());
            }
            continue;
        }
        require(first.values.size() == 1);
        require_checked(first.values);
        const std::string text =
            plugin::to_json(first.values, plugin::value_source::user);
        std::string problem;
        const auto again = plugin::from_json(text, problem);
        if (!again.has_value())
        {
            std::abort();
        }
        const plugin::validation second =
            plugin::validate(alone, *again);
        require(second.ok());
        require(second.values == first.values);
        require(plugin::stable_text(second.values) ==
                plugin::stable_text(first.values));
    }
}

} // namespace

// libFuzzer looks for exactly this name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    std::string problem;
    const auto first = plugin::from_json(text, problem);
    if (first.has_value())
    {
        require(size <= plugin::max_parameters_json_size);
        require(first->size() <= plugin::max_parameters);
        for (std::size_t i = 0; i < first->size(); ++i)
        {
            require(!(*first)[i].id.empty());
            for (std::size_t j = i + 1; j < first->size(); ++j)
            {
                require((*first)[i].id != (*first)[j].id);
            }
        }
        std::string again_problem;
        const auto second = plugin::from_json(text, again_problem);
        if (!second.has_value())
        {
            std::abort();
        }
        require(*first == *second);
        for (const plugin::raw_parameter& item : *first)
        {
            check_one_value(item);
        }
    }
    else
    {
        require(!problem.empty());
    }

    std::string binding_problem;
    const auto binding = plugin::from_binding_params(text, binding_problem);
    require(binding.has_value() || !binding_problem.empty());
    return 0;
}
