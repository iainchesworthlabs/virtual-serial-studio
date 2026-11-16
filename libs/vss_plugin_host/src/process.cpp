//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/process.hpp>

#include <vss/core/error.hpp>
#include <vss/plugin/error.hpp>

#include "platform/process_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

namespace
{

bool has_zero(std::string_view text) noexcept
{
    return text.find('\0') != std::string_view::npos;
}

char upper(char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

/// Whether two names are the same when the case of ASCII letters is put
/// aside.  Windows takes "Path" and "PATH" for one variable, so a request
/// may not hold both on any system.
bool same_name(std::string_view left, std::string_view right) noexcept
{
    return std::ranges::equal(left, right, [](char a, char b)
                              { return upper(a) == upper(b); });
}

/// A path as UTF-8, whatever the system keeps it as.
std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

namespace detail
{

bool valid_request(const process_request& request) noexcept
{
    if (!request.program.is_absolute() ||
        !request.working_directory.is_absolute())
        return false;
    if (std::ranges::any_of(request.arguments, has_zero))
        return false;
    for (std::size_t i = 0; i < request.environment.size(); ++i)
    {
        const auto& [name, value] = request.environment[i];
        if (name.empty() || has_zero(name) || has_zero(value) ||
            name.find('=') != std::string::npos ||
            same_name(name, channel_variable))
            return false;
        for (std::size_t j = 0; j < i; ++j)
        {
            if (same_name(name, request.environment[j].first))
                return false;
        }
    }
    return true;
}

std::wstring windows_command_line(const std::vector<std::wstring>& words)
{
    std::wstring line;
    for (const std::wstring& word : words)
    {
        if (!line.empty())
            line.push_back(L' ');
        if (!word.empty() &&
            word.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        {
            line.append(word);
            continue;
        }
        // Inside quotes a backslash is itself, except in a run that ends
        // at a quote: there each one is written twice, and a quote of the
        // word's own gets one more in front of it.
        line.push_back(L'"');
        std::size_t backslashes = 0;
        for (const wchar_t c : word)
        {
            if (c == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (c == L'"')
                line.append(backslashes * 2 + 1, L'\\');
            else
                line.append(backslashes, L'\\');
            backslashes = 0;
            line.push_back(c);
        }
        line.append(backslashes * 2, L'\\');
        line.push_back(L'"');
    }
    return line;
}

} // namespace detail

std::vector<std::pair<std::string, std::string>>
plugin_environment(std::string_view plugin_id,
                   const std::filesystem::path& data_directory)
{
    const std::string data = utf8(data_directory);
    std::vector<std::pair<std::string, std::string>> environment{
        {"VSS_PLUGIN_ID", std::string(plugin_id)},
        {"VSS_PLUGIN_DATA", data},
        {"HOME", data},
        {"LANG", "C.UTF-8"}};
    for (auto& entry : platform::system_environment())
        environment.push_back(std::move(entry));
    return environment;
}

result<plugin_process> plugin_process::start(const process_request& request)
{
    if (!detail::valid_request(request))
        return make_error_code(plugin_errc::start_failed);
    auto started = platform::start_process(request);
    if (!started)
        return started.error();
    return plugin_process(std::move(*started));
}

plugin_process::plugin_process(std::unique_ptr<impl> state) noexcept
    : impl_(std::move(state))
{
}

plugin_process::~plugin_process() = default;
plugin_process::plugin_process(plugin_process&&) noexcept = default;
plugin_process& plugin_process::operator=(plugin_process&&) noexcept = default;

sandbox_report plugin_process::sandbox() const
{
    return impl_ ? impl_->sandbox() : sandbox_report{};
}

std::uint64_t plugin_process::id() const noexcept
{
    return impl_ ? impl_->id() : 0;
}

native_handle plugin_process::channel() const noexcept
{
    return impl_ ? impl_->channel() : no_handle;
}

native_handle plugin_process::output() const noexcept
{
    return impl_ ? impl_->output() : no_handle;
}

native_handle plugin_process::take_channel() noexcept
{
    return impl_ ? impl_->take_channel() : no_handle;
}

native_handle plugin_process::take_output() noexcept
{
    return impl_ ? impl_->take_output() : no_handle;
}

bool plugin_process::request_stop() noexcept
{
    return impl_ && impl_->request_stop();
}

void plugin_process::kill() noexcept
{
    if (impl_)
        impl_->kill();
}

std::optional<exit_status> plugin_process::poll() noexcept
{
    return impl_ ? impl_->poll() : std::nullopt;
}

std::optional<exit_status>
plugin_process::wait(std::chrono::milliseconds limit) noexcept
{
    return impl_ ? impl_->wait(limit) : std::nullopt;
}

} // namespace vss::plugin::host
