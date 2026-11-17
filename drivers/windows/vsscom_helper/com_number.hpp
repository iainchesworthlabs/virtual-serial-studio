//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rules for COM numbers, with nothing of Windows in them, so that a test can
// run them anywhere: which numbers a port may be given, how a COM name reads, and
// what the friendly name of a port becomes when its number changes.
//

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::helper
{

/// The numbers a port may have. The same range as protocol.h; the COM name
/// database holds 256 ports until somebody resizes it.
constexpr unsigned long min_com_number = 1;
constexpr unsigned long max_com_number = 255;

constexpr bool com_number_valid(unsigned long number) noexcept
{
    return number >= min_com_number && number <= max_com_number;
}

inline std::wstring com_name(unsigned long number)
{
    return L"COM" + std::to_wstring(number);
}

/// "COM7" is 7. Anything else (another prefix, no digits, a sign, trailing
/// text, a number too large to be a port) is nothing. Not case sensitive.
inline std::optional<unsigned long> parse_com_name(std::wstring_view name)
{
    if (name.size() < 4 || name.size() > 7)
        return std::nullopt;
    const wchar_t* prefix = L"COM";
    for (std::size_t i = 0; i < 3; ++i)
    {
        const wchar_t c = name[i];
        const wchar_t upper = (c >= L'a' && c <= L'z')
                                  ? static_cast<wchar_t>(c - L'a' + L'A')
                                  : c;
        if (upper != prefix[i])
            return std::nullopt;
    }

    unsigned long number = 0;
    for (std::size_t i = 3; i < name.size(); ++i)
    {
        if (name[i] < L'0' || name[i] > L'9')
            return std::nullopt;
        number = number * 10 + static_cast<unsigned long>(name[i] - L'0');
    }
    return number;
}

/// What holds a COM number, as far as the helper can tell.
enum class holder_kind
{
    /// A device that is present: a real port, com0com, or a Studio port that
    /// is running. The number is taken.
    live_device,
    /// A Studio port that is gone. Windows keeps its device entry, with the
    /// PortName, and the number stays marked in the COM name database. Studio
    /// made it, so Studio may remove it and take the number back.
    studio_ghost,
    /// A port of something else that is not plugged in (a USB adapter that
    /// comes back to the same number). Not Studio's to take.
    other_ghost
};

struct number_holder
{
    holder_kind kind = holder_kind::live_device;
    /// The device instance id, for the helper to remove a Studio ghost.
    std::wstring instance;
};

/// What to do when a program wants a COM number. `allowed` is false when the
/// number belongs to something that is not Studio's to take; otherwise the
/// helper removes `remove` (Studio ghosts holding the number), and then claims
/// the number, releasing it first if the database still marks it with no
/// device behind it.
struct claim_plan
{
    bool allowed = false;
    std::vector<std::wstring> remove;
};

/// `name_resolves` is true when \\.\COMn already resolves to something.
/// `holders` are the devices (present or not) whose PortName is that number,
/// other than the port being renamed. A number that only the database marks,
/// with no device named in `holders`, is free to take: nothing owns it.
inline claim_plan plan_claim(bool name_resolves,
                             const std::vector<number_holder>& holders)
{
    claim_plan plan;
    if (name_resolves)
        return plan;
    for (const auto& holder : holders)
    {
        if (holder.kind != holder_kind::studio_ghost)
            return plan;
    }
    plan.allowed = true;
    for (const auto& holder : holders)
        plan.remove.push_back(holder.instance);
    return plan;
}

/// True when the device instance id is one of Studio's ports: the user-mode
/// driver's (SWD\VSSCOMUM\...) or the kernel-mode driver's (VSSCOM\PORT\...).
inline bool is_studio_port_instance(std::wstring_view instance)
{
    const auto starts_with = [&](std::wstring_view prefix)
    {
        if (instance.size() < prefix.size())
            return false;
        for (std::size_t i = 0; i < prefix.size(); ++i)
        {
            wchar_t c = instance[i];
            if (c >= L'a' && c <= L'z')
                c = static_cast<wchar_t>(c - L'a' + L'A');
            if (c != prefix[i])
                return false;
        }
        return true;
    };
    return starts_with(L"SWD\\VSSCOMUM\\") || starts_with(L"VSSCOM\\PORT\\");
}

/// The friendly name of a port after its number changed: "Name (COM4)" becomes
/// "Name (COM9)". A name that does not end in a bracketed COM name gets one added,
/// which is what the Ports class installer would have made of it.
inline std::wstring friendly_name_for(std::wstring_view current,
                                      unsigned long number)
{
    const std::wstring tail = L"(" + com_name(number) + L")";

    while (!current.empty() && (current.back() == L' ' || current.back() == L'\0'))
        current.remove_suffix(1);

    if (!current.empty() && current.back() == L')')
    {
        const std::size_t open = current.rfind(L'(');
        if (open != std::wstring_view::npos)
        {
            const std::wstring_view inside =
                current.substr(open + 1, current.size() - open - 2);
            if (parse_com_name(inside))
            {
                std::wstring name(current.substr(0, open));
                name += tail;
                return name;
            }
        }
    }

    std::wstring name(current);
    if (!name.empty())
        name += L' ';
    name += tail;
    return name;
}

} // namespace vss::helper
