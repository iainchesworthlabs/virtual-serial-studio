//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Windows says about elevation and Windows Firewall, and the two things
// Studio may do about it once the user agrees: add or remove one firewall rule
// (in a helper that was started elevated) and start that helper through the
// UAC prompt.
//
// Reading is done with the Windows Firewall COM interfaces (INetFwPolicy2), the
// same ones the Windows Defender Firewall console uses. Nothing here shells out
// to netsh, and nothing here changes a setting except `apply` below.
//

#include <vss/core/profile.hpp>

#include "../permissions_platform.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

// clang-format off
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
// clang-format on

namespace vss::detail
{
namespace
{

using namespace permissions;

std::string narrow(const wchar_t* text, int length = -1)
{
    if (text == nullptr)
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0,
                                           nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), size, nullptr,
                          nullptr);
    if (length == -1 && !out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

std::wstring widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                          static_cast<int>(text.size()), out.data(), size);
    return out;
}

/// COM for this thread, for the life of the object.
class com_scope
{
public:
    com_scope()
    {
        const HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // S_FALSE: already initialised here, balance it. RPC_E_CHANGED_MODE:
        // the thread chose another model, which is fine and is not ours to
        // undo.
        initialised_ = SUCCEEDED(result);
        usable_ = SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
    }
    ~com_scope()
    {
        if (initialised_)
            ::CoUninitialize();
    }
    com_scope(const com_scope&) = delete;
    com_scope& operator=(const com_scope&) = delete;
    bool usable() const noexcept { return usable_; }

private:
    bool initialised_ = false;
    bool usable_ = false;
};

template <class T> class com_ptr
{
public:
    com_ptr() = default;
    ~com_ptr() { reset(); }
    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;
    T* get() const noexcept { return pointer_; }
    T* operator->() const noexcept { return pointer_; }
    T** out() noexcept
    {
        reset();
        return &pointer_;
    }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }
    void reset() noexcept
    {
        if (pointer_ != nullptr)
            pointer_->Release();
        pointer_ = nullptr;
    }

private:
    T* pointer_ = nullptr;
};

class bstr
{
public:
    bstr() = default;
    explicit bstr(const std::wstring& text)
        : value_(
              ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size())))
    {
    }
    ~bstr() { ::SysFreeString(value_); }
    bstr(const bstr&) = delete;
    bstr& operator=(const bstr&) = delete;
    BSTR get() const noexcept { return value_; }
    BSTR* out() noexcept
    {
        ::SysFreeString(value_);
        value_ = nullptr;
        return &value_;
    }
    std::string text() const
    {
        return value_ == nullptr
                   ? std::string()
                   : narrow(value_, static_cast<int>(::SysStringLen(value_)));
    }

private:
    BSTR value_ = nullptr;
};

tri token_elevated(HANDLE token, tri& administrator)
{
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (::GetTokenInformation(token, TokenElevation, &elevation,
                              sizeof(elevation), &size) == 0)
        return tri::unknown;

    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    const bool have_type =
        ::GetTokenInformation(token, TokenElevationType, &type, sizeof(type),
                              &size) != 0;

    if (elevation.TokenIsElevated != 0)
    {
        // Elevated: the account is an administrator (or User Account Control is
        // off and every account is).
        administrator = tri::yes;
        return tri::yes;
    }
    if (have_type && type == TokenElevationTypeLimited)
    {
        // A filtered token: an administrator account that has not elevated.
        administrator = tri::yes;
        return tri::no;
    }

    // A plain token. It is an administrator only when the group is enabled in
    // it, which it cannot be without elevation, so this is a standard user.
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    if (::AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                   DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                   &group) == 0)
        return tri::no;
    BOOL member = FALSE;
    const BOOL checked = ::CheckTokenMembership(nullptr, group, &member);
    ::FreeSid(group);
    administrator =
        checked == 0 ? tri::unknown : (member != 0 ? tri::yes : tri::no);
    return tri::no;
}

void observe_elevation(windows_observation& seen)
{
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
        return;
    seen.elevated = token_elevated(token, seen.administrator);
    ::CloseHandle(token);
}

bool read_profile_flag(INetFwPolicy2* policy, NET_FW_PROFILE_TYPE2 profile,
                       bool& enabled)
{
    VARIANT_BOOL value = VARIANT_FALSE;
    if (FAILED(policy->get_FirewallEnabled(profile, &value)))
        return false;
    enabled = value != VARIANT_FALSE;
    return true;
}

void observe_firewall(windows_observation& seen)
{
    com_scope com;
    if (!com.usable())
        return;

    com_ptr<INetFwPolicy2> policy;
    if (FAILED(::CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2),
                                  reinterpret_cast<void**>(policy.out()))))
        return;

    long current = 0;
    if (SUCCEEDED(policy->get_CurrentProfileTypes(&current)))
        seen.active_profiles = current;

    // On when any network the computer is on now has the firewall on.
    const struct
    {
        long bit = 0;
        NET_FW_PROFILE_TYPE2 profile = NET_FW_PROFILE2_DOMAIN;
    } profiles[] = {{profile_domain, NET_FW_PROFILE2_DOMAIN},
                    {profile_private, NET_FW_PROFILE2_PRIVATE},
                    {profile_public, NET_FW_PROFILE2_PUBLIC}};
    bool any_known = false;
    bool any_on = false;
    bool any_block_all = false;
    bool block_all_known = false;
    for (const auto& entry : profiles)
    {
        if (seen.active_profiles != 0 &&
            (seen.active_profiles & entry.bit) == 0)
            continue;
        bool enabled = false;
        if (!read_profile_flag(policy.get(), entry.profile, enabled))
            continue;
        any_known = true;
        if (!enabled)
            continue;
        any_on = true;
        VARIANT_BOOL block = VARIANT_FALSE;
        if (SUCCEEDED(
                policy->get_BlockAllInboundTraffic(entry.profile, &block)))
        {
            block_all_known = true;
            any_block_all = any_block_all || block != VARIANT_FALSE;
        }
    }
    if (any_known)
        seen.firewall_on = any_on ? tri::yes : tri::no;
    if (any_on && block_all_known)
        seen.block_all_inbound = any_block_all ? tri::yes : tri::no;

    com_ptr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(rules.out())))
        return;
    com_ptr<IUnknown> enumeration_unknown;
    if (FAILED(rules->get__NewEnum(enumeration_unknown.out())))
        return;
    com_ptr<IEnumVARIANT> enumeration;
    if (FAILED(enumeration_unknown->QueryInterface(
            __uuidof(IEnumVARIANT),
            reinterpret_cast<void**>(enumeration.out()))))
        return;

    std::vector<firewall_rule_record> records;
    for (;;)
    {
        VARIANT item;
        ::VariantInit(&item);
        ULONG fetched = 0;
        const HRESULT next = enumeration->Next(1, &item, &fetched);
        if (next != S_OK || fetched == 0)
            break;

        if (item.vt == VT_DISPATCH && item.pdispVal != nullptr)
        {
            com_ptr<INetFwRule> rule;
            if (SUCCEEDED(item.pdispVal->QueryInterface(
                    __uuidof(INetFwRule),
                    reinterpret_cast<void**>(rule.out()))))
            {
                VARIANT_BOOL enabled = VARIANT_FALSE;
                NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_IN;
                NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
                // Only enabled inbound rules can decide a listener.
                if (SUCCEEDED(rule->get_Enabled(&enabled)) &&
                    enabled != VARIANT_FALSE &&
                    SUCCEEDED(rule->get_Direction(&direction)) &&
                    direction == NET_FW_RULE_DIR_IN &&
                    SUCCEEDED(rule->get_Action(&action)))
                {
                    firewall_rule_record record;
                    record.enabled = true;
                    record.inbound = true;
                    record.allow = action == NET_FW_ACTION_ALLOW;

                    bstr service;
                    const bool is_service =
                        SUCCEEDED(rule->get_ServiceName(service.out())) &&
                        service.get() != nullptr &&
                        ::SysStringLen(service.get()) != 0;

                    bstr application;
                    bstr ports;
                    long protocol = 256;
                    long rule_profiles = profile_all;
                    if (!is_service &&
                        SUCCEEDED(
                            rule->get_ApplicationName(application.out())) &&
                        SUCCEEDED(rule->get_LocalPorts(ports.out())))
                    {
                        rule->get_Protocol(&protocol);
                        rule->get_Profiles(&rule_profiles);
                        record.program = application.text();
                        record.protocol = static_cast<int>(protocol);
                        record.local_ports = ports.text();
                        record.profiles = rule_profiles;
                        records.push_back(std::move(record));
                    }
                }
            }
        }
        ::VariantClear(&item);
    }
    seen.rules = std::move(records);
}

std::string quote_argument(const std::string& argument)
{
    if (!argument.empty() && argument.find_first_of(" \t") == std::string::npos)
        return argument;
    std::string out = "\"";
    out += argument;
    // A backslash before the closing quote would escape it.
    if (!out.empty() && out.back() == '\\')
        out += '\\';
    out += '"';
    return out;
}

step_status status_for_exit(DWORD code) noexcept
{
    switch (code)
    {
    case static_cast<DWORD>(exit_done):
        return step_status::done;
    case static_cast<DWORD>(exit_not_elevated):
        return step_status::not_elevated;
    case static_cast<DWORD>(exit_invalid):
        return step_status::invalid;
    default:
        break;
    }
    return step_status::failed;
}

std::wstring rule_text(const firewall_rule_spec& spec)
{
    std::string text = "Lets ";
    text += spec.scope == rule_scope::local_subnet ? "computers on the local "
                                                     "network"
                                                   : "any computer";
    text += " connect to Virtual Serial Studio on ";
    text += spec.protocol == transport_protocol::tcp ? "TCP" : "UDP";
    text += " port " + std::to_string(spec.port) +
            ". Added at the request of the user. Remove it in Windows Defender "
            "Firewall, or in Virtual Serial Studio.";
    return widen(text);
}

} // namespace

std::string_view platform_host_name() noexcept
{
    return "windows";
}

permissions::observation platform_observe(const permissions::context&)
{
    VSS_PROFILE_ZONE("ports.permissions.observe_windows");
    observation seen;
    observe_elevation(seen.windows);
    observe_firewall(seen.windows);
    return seen;
}

std::string platform_this_program()
{
    std::wstring buffer(512, L'\0');
    for (;;)
    {
        const DWORD length = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return narrow(buffer.c_str());
        }
        if (buffer.size() >= 32768)
            return {};
        buffer.resize(buffer.size() * 2);
    }
}

bool platform_can_manage_firewall_rules() noexcept
{
    return true;
}

permissions::step_result
platform_apply_firewall_rule(const permissions::firewall_rule_spec& spec,
                             bool remove)
{
    VSS_PROFILE_ZONE("ports.permissions.firewall_windows");
    windows_observation seen;
    observe_elevation(seen);
    if (seen.elevated != tri::yes)
        return {step_status::not_elevated,
                "the firewall can be changed only with administrator rights"};

    com_scope com;
    if (!com.usable())
        return {step_status::failed, "COM could not be started"};

    com_ptr<INetFwPolicy2> policy;
    if (FAILED(::CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2),
                                  reinterpret_cast<void**>(policy.out()))))
        return {step_status::failed, "Windows Firewall could not be opened"};
    com_ptr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(rules.out())))
        return {step_status::failed, "the firewall rules could not be opened"};

    const bstr name(widen(firewall_rule_name(spec)));

    // Our own rule of this name is replaced, so asking twice leaves one rule.
    // Remove deletes by name only; the name is ours.
    if (FAILED(rules->Remove(name.get())) && remove)
        return {step_status::failed, "the rule could not be removed"};
    if (remove)
        return {step_status::done, "the rule was removed"};

    com_ptr<INetFwRule> rule;
    if (FAILED(::CoCreateInstance(__uuidof(NetFwRule), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(INetFwRule),
                                  reinterpret_cast<void**>(rule.out()))))
        return {step_status::failed, "a firewall rule could not be created"};

    const bstr description(rule_text(spec));
    const bstr group(widen(std::string(firewall_rule_group())));
    const bstr program(widen(spec.program));
    const bstr ports(widen(std::to_string(spec.port)));
    const bstr remote(spec.scope == rule_scope::local_subnet ? L"LocalSubnet"
                                                             : L"*");

    const bool ok =
        SUCCEEDED(rule->put_Name(name.get())) &&
        SUCCEEDED(rule->put_Description(description.get())) &&
        SUCCEEDED(rule->put_Grouping(group.get())) &&
        SUCCEEDED(rule->put_ApplicationName(program.get())) &&
        SUCCEEDED(rule->put_Protocol(spec.protocol == transport_protocol::tcp
                                         ? NET_FW_IP_PROTOCOL_TCP
                                         : NET_FW_IP_PROTOCOL_UDP)) &&
        SUCCEEDED(rule->put_LocalPorts(ports.get())) &&
        SUCCEEDED(rule->put_RemoteAddresses(remote.get())) &&
        SUCCEEDED(rule->put_Profiles(spec.profiles)) &&
        SUCCEEDED(rule->put_Direction(NET_FW_RULE_DIR_IN)) &&
        SUCCEEDED(rule->put_Action(NET_FW_ACTION_ALLOW)) &&
        SUCCEEDED(rule->put_Enabled(VARIANT_TRUE));
    if (!ok)
        return {step_status::failed, "the rule could not be filled in"};
    if (FAILED(rules->Add(rule.get())))
        return {step_status::failed, "Windows refused the rule"};
    return {step_status::done, "the rule was added"};
}

bool platform_can_run_elevated() noexcept
{
    return true;
}

permissions::step_result
platform_run_elevated(const std::string& executable,
                      const std::vector<std::string>& arguments,
                      int wait_seconds)
{
    VSS_PROFILE_ZONE("ports.permissions.elevate_windows");
    std::string command_line;
    for (const std::string& argument : arguments)
    {
        if (!command_line.empty())
            command_line += ' ';
        command_line += quote_argument(argument);
    }
    const std::wstring file = widen(executable);
    const std::wstring parameters = widen(command_line);

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask =
        SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = file.c_str();
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (::ShellExecuteExW(&info) == 0)
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_CANCELLED)
            return {step_status::declined, "the elevation prompt was declined"};
        return {step_status::failed, "the helper could not be started (error " +
                                         std::to_string(error) + ")"};
    }
    if (info.hProcess == nullptr)
        return {step_status::failed, "the helper did not start a process"};

    // The caller says how long its step may take: a call into the firewall is
    // short, a driver install is not, and both must end so that a hang is seen.
    const DWORD waited = ::WaitForSingleObject(
        info.hProcess, static_cast<DWORD>(wait_seconds) * 1000U);
    DWORD code = 0;
    const bool have_code = waited == WAIT_OBJECT_0 &&
                           ::GetExitCodeProcess(info.hProcess, &code) != 0;
    ::CloseHandle(info.hProcess);
    if (!have_code)
        return {step_status::failed, "the helper did not finish"};
    return {status_for_exit(code), "the helper finished",
            static_cast<int>(code)};
}

bool platform_can_add_user_to_group() noexcept
{
    return false;
}

permissions::step_result
platform_add_user_to_group(const permissions::group_membership_spec&)
{
    return {permissions::step_status::unsupported,
            "this platform has no group step"};
}

} // namespace vss::detail
