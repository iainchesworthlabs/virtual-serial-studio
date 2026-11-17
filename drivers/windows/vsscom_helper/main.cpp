//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// vsscom_helper - makes the ports of the user-mode driver for ordinary users.
//
// A service that runs as SYSTEM, because only SYSTEM and administrators may
// make the software devices that the user-mode driver's ports are. A caller
// opens the named pipe in protocol.h and asks for a port; the helper makes a
// software device with the hardware ID VSSCOMUM\PORT, records the caller's SID
// on it as the owner, and keeps the device for as long as the pipe is open.
// When the pipe closes, for any reason, the device is closed and the port goes.
//
//   vsscom_helper.exe             run as a service (the service control manager
//                                 starts it this way);
//   vsscom_helper.exe --console   run in the foreground, for a test or a trial;
//                                 it needs an elevated command prompt. Ctrl+C
//                                 stops it.
//
// The rules (the limits, and which user may open which port) are in protocol.h.
// The helper writes nothing but short lines to the debugger.
//

// windows.h first: the others need its types. Sorted, they would not follow it.
// clang-format off
#include <windows.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
#include <sddl.h>
#include <setupapi.h>
#include <swdevice.h>
#include <winioctl.h>
// Defines the interface GUIDs of public.h in this file only.
#include <initguid.h>
// After initguid.h: this file defines GUID_DEVCLASS_PORTS too.
#include <devguid.h>
// clang-format on

#include "../vsscom/public.h"
#include "com_number.hpp"
#include "lease_table.hpp"
#include "protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

// The hardware IDs of the software device: a multi-string, so two terminators.
const WCHAR hardware_ids[] = L"VSSCOMUM\\PORT\0";

// The most connections at once, and for one user. A connection that holds no
// port and says nothing is dropped after the idle time; the first request has
// the shorter time, because until it arrives the helper does not know who is
// there.
constexpr std::size_t max_connections = 64;
constexpr std::size_t max_connections_per_user = 20;
constexpr DWORD first_request_ms = 5000;
constexpr DWORD idle_ms = 30000;
constexpr DWORD create_wait_ms = 30000;

void trace(const wchar_t* text)
{
    ::OutputDebugStringW(L"vsscom_helper: ");
    ::OutputDebugStringW(text);
    ::OutputDebugStringW(L"\n");
}

struct handle_closer
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};
using unique_handle = std::unique_ptr<void, handle_closer>;

/// What the connections share.
struct shared_state
{
    shared_state()
        : leases(VSSCOM_HELPER_MAX_PORTS, VSSCOM_HELPER_MAX_PORTS_PER_USER),
          stop(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    vss::helper::lease_table leases;
    unique_handle stop;

    std::mutex mutex;
    // One change of a COM number at a time: it claims, writes and restarts, and two
    // of them would race for the same number.
    std::mutex com_mutex;
    std::condition_variable idle;
    std::size_t connections = 0;
    std::map<std::string, std::size_t> connections_of;
};

DWORD win32_from(HRESULT result) noexcept
{
    if (HRESULT_FACILITY(result) == FACILITY_WIN32)
        return HRESULT_CODE(result);
    return ERROR_NO_SYSTEM_RESOURCES;
}

struct creation
{
    HANDLE done;
    HRESULT result;
};

void WINAPI on_created(HSWDEVICE, HRESULT result, PVOID context, PCWSTR)
{
    auto* made = static_cast<creation*>(context);
    made->result = result;
    ::SetEvent(made->done);
}

/// Waits until a driver has started on the device. The device has no driver
/// until Windows has found one in the driver store, and a machine where the
/// user-mode driver is not installed never gets one: that is reported as
/// ERROR_DEV_NOT_EXIST, so that the caller does not wait for a port that will
/// not come.
DWORD wait_started(std::wstring instance_id, HANDLE stop)
{
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::milliseconds(create_wait_ms);
    for (;;)
    {
        DEVINST node = 0;
        if (::CM_Locate_DevNodeW(&node, instance_id.data(),
                                 CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        {
            ULONG status = 0;
            ULONG problem = 0;
            if (::CM_Get_DevNode_Status(&status, &problem, node, 0) ==
                CR_SUCCESS)
            {
                if (problem == 0 && (status & DN_STARTED) != 0)
                    return ERROR_SUCCESS;
                if (problem == CM_PROB_FAILED_INSTALL)
                    return ERROR_DEV_NOT_EXIST;
            }
        }
        if (clock::now() >= until)
            return ERROR_TIMEOUT;
        if (::WaitForSingleObject(stop, 100) == WAIT_OBJECT_0)
            return ERROR_OPERATION_ABORTED;
    }
}

/// Makes the software device of one port and waits until its driver has
/// started. The caller then waits for the port's hub interface, which follows
/// at once.
DWORD make_device(std::size_t slot, const std::string& owner, HANDLE stop,
                  HSWDEVICE& device)
{
    wchar_t instance[16]{};
    swprintf_s(instance, VssComUmPortInstanceFormat,
               static_cast<unsigned long>(slot));

    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = instance;
    info.pszzHardwareIds = hardware_ids;
    info.CapabilityFlags = SWDeviceCapabilitiesRemovable |
                           SWDeviceCapabilitiesSilentInstall |
                           SWDeviceCapabilitiesDriverRequired;
    info.pszDeviceDescription = L"Virtual Serial Studio COM Port (user mode)";

    DEVPROPERTY property{};
    property.CompKey.Key = VssComUmOwnerKey;
    property.CompKey.LocaleName = nullptr;
    property.CompKey.Store = DEVPROP_STORE_SYSTEM;
    property.Type = DEVPROP_TYPE_BINARY;
    property.BufferSize = static_cast<ULONG>(owner.size());
    property.Buffer = const_cast<char*>(owner.data());

    unique_handle done(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!done)
        return ::GetLastError();
    creation made{done.get(), E_PENDING};

    device = nullptr;
    const HRESULT hr =
        ::SwDeviceCreate(VssComUmEnumerator, L"HTREE\\ROOT\\0", &info, 1,
                         &property, on_created, &made, &device);
    if (FAILED(hr))
    {
        device = nullptr;
        return win32_from(hr);
    }

    if (::WaitForSingleObject(done.get(), create_wait_ms) != WAIT_OBJECT_0 ||
        FAILED(made.result))
    {
        // The callback may still arrive after SwDeviceClose; `made` must not go
        // first. SwDeviceClose waits for it.
        ::SwDeviceClose(device);
        device = nullptr;
        return FAILED(made.result) ? win32_from(made.result) : ERROR_TIMEOUT;
    }

    const DWORD started =
        wait_started(std::wstring(VssComUmPortInstancePrefix) + instance, stop);
    if (started != ERROR_SUCCESS)
    {
        ::SwDeviceClose(device);
        device = nullptr;
    }
    return started;
}

// ---------------------------------------------------------------------------
// COM numbers (see "COM numbers" in protocol.h)
// ---------------------------------------------------------------------------

/// The COM name database of msports.dll. Loaded when needed, so that the helper
/// runs on a machine whose Ports class is somehow missing, and answers
/// ERROR_GEN_FAILURE to the one request that wants it.
class com_database
{
public:
    static std::unique_ptr<com_database> open()
    {
        std::unique_ptr<com_database> db(new com_database);
        db->module_ = ::LoadLibraryExW(L"msports.dll", nullptr,
                                       LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (db->module_ == nullptr)
            return nullptr;
        db->open_ = reinterpret_cast<open_fn>(
            ::GetProcAddress(db->module_, "ComDBOpen"));
        db->close_ = reinterpret_cast<close_fn>(
            ::GetProcAddress(db->module_, "ComDBClose"));
        db->claim_ = reinterpret_cast<claim_fn>(
            ::GetProcAddress(db->module_, "ComDBClaimPort"));
        db->release_ = reinterpret_cast<release_fn>(
            ::GetProcAddress(db->module_, "ComDBReleasePort"));
        if (db->open_ == nullptr || db->close_ == nullptr ||
            db->claim_ == nullptr || db->release_ == nullptr ||
            db->open_(&db->handle_) != ERROR_SUCCESS)
            return nullptr;
        return db;
    }

    ~com_database()
    {
        if (handle_ != nullptr && close_ != nullptr)
            (void)close_(handle_);
        if (module_ != nullptr)
            ::FreeLibrary(module_);
    }

    com_database(const com_database&) = delete;
    com_database& operator=(const com_database&) = delete;

    /// Takes the number if nobody else holds it, and never takes it from
    /// another device. False when it is held.
    bool claim(DWORD number)
    {
        BOOL forced = FALSE;
        return claim_(handle_, number, FALSE, &forced) == ERROR_SUCCESS &&
               !forced;
    }

    void release(DWORD number) { (void)release_(handle_, number); }

private:
    com_database() = default;

    using open_fn = LONG(WINAPI*)(HANDLE*);
    using close_fn = LONG(WINAPI*)(HANDLE);
    using claim_fn = LONG(WINAPI*)(HANDLE, DWORD, BOOL, BOOL*);
    using release_fn = LONG(WINAPI*)(HANDLE, DWORD);

    HMODULE module_ = nullptr;
    HANDLE handle_ = nullptr;
    open_fn open_ = nullptr;
    close_fn close_ = nullptr;
    claim_fn claim_ = nullptr;
    release_fn release_ = nullptr;
};

/// True when \\.\COMn already resolves to something: a real port, com0com, or a
/// port of this driver. A name that is not in the database but is in use is as
/// taken as one that is.
bool dos_name_exists(const std::wstring& name)
{
    wchar_t target[512];
    return ::QueryDosDeviceW(name.c_str(), target, 512) != 0;
}

bool read_port_name(HKEY key, std::wstring& name)
{
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    if (::RegQueryValueExW(key, L"PortName", nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) !=
            ERROR_SUCCESS ||
        type != REG_SZ)
        return false;
    name = buffer;
    return true;
}

bool write_port_name(HKEY key, const std::wstring& name)
{
    return ::RegSetValueExW(key, L"PortName", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(name.c_str()),
                            static_cast<DWORD>((name.size() + 1) *
                                               sizeof(wchar_t))) ==
           ERROR_SUCCESS;
}

std::wstring read_friendly_name(DEVINST node)
{
    wchar_t buffer[256]{};
    ULONG size = sizeof(buffer) - sizeof(wchar_t);
    if (::CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_FRIENDLYNAME, nullptr,
                                            buffer, &size, 0) != CR_SUCCESS)
        return {};
    return buffer;
}

void write_friendly_name(DEVINST node, const std::wstring& name)
{
    (void)::CM_Set_DevNode_Registry_PropertyW(
        node, CM_DRP_FRIENDLYNAME, const_cast<wchar_t*>(name.c_str()),
        static_cast<ULONG>((name.size() + 1) * sizeof(wchar_t)), 0);
}

/// True when no program has the COM port open. A restart with a handle open fails,
/// and Windows then keeps the port from restarting at all until it is made again,
/// so the helper looks first, by opening the port itself: the driver lets one
/// handle in at a time. The handle is let go at once; the restart follows it.
bool port_is_free(const std::wstring& name)
{
    unique_handle probe(::CreateFileW((L"\\\\.\\" + name).c_str(),
                                      GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr));
    if (probe.get() != INVALID_HANDLE_VALUE)
        return true;
    const DWORD error = ::GetLastError();
    return error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION;
}

/// Restarts the device, as Device Manager does after a COM number changed. Windows
/// refuses while a program has a handle on it.
bool restart_device(const std::wstring& instance)
{
    const HDEVINFO set = ::SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE)
        return false;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    bool ok = ::SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0,
                                       &data) != FALSE;
    if (ok)
        ok = ::SetupDiRestartDevices(set, &data) != FALSE;
    // Windows can answer yes and still not have restarted the port, because a
    // handle is open: it sets a flag instead. Device Manager would ask for a
    // restart of the machine.
    if (ok)
    {
        SP_DEVINSTALL_PARAMS_W parameters{};
        parameters.cbSize = sizeof(parameters);
        if (::SetupDiGetDeviceInstallParamsW(set, &data, &parameters) != FALSE
            && (parameters.Flags & (DI_NEEDRESTART | DI_NEEDREBOOT)) != 0)
            ok = false;
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return ok;
}

/// Waits until the COM name of a restarted port resolves again.
bool wait_name(const std::wstring& name, HANDLE stop)
{
    using clock = std::chrono::steady_clock;
    const auto until = clock::now() + std::chrono::seconds(15);
    for (;;)
    {
        if (dos_name_exists(name))
            return true;
        if (clock::now() >= until ||
            ::WaitForSingleObject(stop, 100) == WAIT_OBJECT_0)
            return false;
    }
}

/// The devices of the Ports class, present or not, whose PortName is `wanted`,
/// other than `except` (the port being renamed). A port that is gone stays in the
/// Ports class with its PortName, and Windows keeps its number marked in the COM
/// name database; this is how those are told from a number that a device in use
/// holds.
std::vector<vss::helper::number_holder>
find_holders(const std::wstring& wanted, const std::wstring& except)
{
    std::vector<vss::helper::number_holder> holders;
    const HDEVINFO set = ::SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr,
                                                nullptr, 0);
    if (set == INVALID_HANDLE_VALUE)
        return holders;

    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; ::SetupDiEnumDeviceInfo(set, index, &data); ++index)
    {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if (!::SetupDiGetDeviceInstanceIdW(set, &data, id, MAX_DEVICE_ID_LEN,
                                           nullptr) ||
            _wcsicmp(id, except.c_str()) == 0)
            continue;

        const HKEY key = ::SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0,
                                                DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        std::wstring name;
        const bool has_name = read_port_name(key, name);
        ::RegCloseKey(key);
        if (!has_name || _wcsicmp(name.c_str(), wanted.c_str()) != 0)
            continue;

        DEVINST node = 0;
        const bool present =
            ::CM_Locate_DevNodeW(&node, id, CM_LOCATE_DEVNODE_NORMAL) ==
            CR_SUCCESS;

        vss::helper::number_holder holder;
        holder.instance = id;
        holder.kind =
            present ? vss::helper::holder_kind::live_device
                    : (vss::helper::is_studio_port_instance(id)
                           ? vss::helper::holder_kind::studio_ghost
                           : vss::helper::holder_kind::other_ghost);
        holders.push_back(std::move(holder));
    }
    ::SetupDiDestroyDeviceInfoList(set);
    return holders;
}

/// Removes the entry of a port that is gone, as Device Manager does for a
/// hidden device. The Ports class installer lets go of its number in the database.
void remove_ghost(const std::wstring& instance)
{
    const HDEVINFO set = ::SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE)
        return;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    if (::SetupDiOpenDeviceInfoW(set, instance.c_str(), nullptr, 0, &data))
        (void)::SetupDiCallClassInstaller(DIF_REMOVE, set, &data);
    ::SetupDiDestroyDeviceInfoList(set);
}

/// Takes `number` in the database for the port `instance`, or says it is not
/// Studio's to take (ERROR_ALREADY_ASSIGNED). A number that the database marks
/// for a Studio port that is gone, or for no device at all, is taken back; one that
/// a present device holds, or a port of something else that is only unplugged, is
/// not.
DWORD claim_number(com_database& database, const std::wstring& instance,
                   DWORD number)
{
    const std::wstring wanted = vss::helper::com_name(number);
    const auto plan =
        vss::helper::plan_claim(dos_name_exists(wanted),
                                find_holders(wanted, instance));
    if (!plan.allowed)
        return ERROR_ALREADY_ASSIGNED;
    for (const auto& ghost : plan.remove)
        remove_ghost(ghost);
    if (database.claim(number))
        return ERROR_SUCCESS;
    // Nothing holds the number but the database: let it go and take it.
    database.release(number);
    return database.claim(number) ? ERROR_SUCCESS : ERROR_ALREADY_ASSIGNED;
}

/// Gives the port of the device `instance` the COM number `number`.
///
/// Nothing is changed unless the whole change goes through: a failure puts the
/// old name and the old claim back. See "COM numbers" in protocol.h for the steps.
DWORD set_com_number(const std::wstring& instance, DWORD number, HANDLE stop)
{
    if (!vss::helper::com_number_valid(number))
        return ERROR_INVALID_PARAMETER;
    const std::wstring wanted = vss::helper::com_name(number);

    DEVINST node = 0;
    if (::CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()),
                             CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return ERROR_NOT_FOUND;

    HKEY raw = nullptr;
    if (::CM_Open_DevNode_Key(node, KEY_QUERY_VALUE | KEY_SET_VALUE, 0,
                              RegDisposition_OpenExisting, &raw,
                              CM_REGISTRY_HARDWARE) != CR_SUCCESS)
        return ERROR_GEN_FAILURE;
    struct key_closer
    {
        HKEY key;
        ~key_closer() { ::RegCloseKey(key); }
    } closer{raw};

    std::wstring old_name;
    if (!read_port_name(raw, old_name))
        return ERROR_GEN_FAILURE;
    if (_wcsicmp(old_name.c_str(), wanted.c_str()) == 0)
        return ERROR_SUCCESS; // it is that number already
    const auto old_number = vss::helper::parse_com_name(old_name);

    const auto database = com_database::open();
    if (!database)
        return ERROR_GEN_FAILURE;
    if (const DWORD claimed = claim_number(*database, instance, number);
        claimed != ERROR_SUCCESS)
        return claimed;

    const std::wstring old_friendly = read_friendly_name(node);
    auto put_back = [&]
    {
        (void)write_port_name(raw, old_name);
        if (!old_friendly.empty())
            write_friendly_name(node, old_friendly);
        database->release(number);
    };

    if (!write_port_name(raw, wanted))
    {
        database->release(number);
        return ERROR_GEN_FAILURE;
    }
    write_friendly_name(node,
                        vss::helper::friendly_name_for(old_friendly, number));

    // Nothing is restarted if a program has the port: the port would not restart
    // again. A failed restart (the hub end was open after all) leaves the port
    // that way, and no second try is made.
    if (!port_is_free(old_name) || !restart_device(instance))
    {
        put_back();
        return ERROR_BUSY;
    }
    const DWORD started = wait_started(instance, stop);
    if (started != ERROR_SUCCESS || !wait_name(wanted, stop))
    {
        put_back();
        (void)restart_device(instance);
        return started == ERROR_SUCCESS ? ERROR_GEN_FAILURE : started;
    }

    if (old_number)
        database->release(*old_number);
    return ERROR_SUCCESS;
}

/// The path of the control device of the kernel-mode driver, or empty.
std::wstring control_device_path()
{
    GUID guid = GUID_DEVINTERFACE_VSSCOMCTL;
    ULONG length = 0;
    if (::CM_Get_Device_Interface_List_SizeW(
            &length, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
            CR_SUCCESS ||
        length <= 1)
        return {};
    std::vector<wchar_t> list(length);
    if (::CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), length,
                                        CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS)
        return {};
    return list.data();
}

/// Asks the control device whether the port in `slot` was made by `sid`.
/// ERROR_SUCCESS: it was. ERROR_ACCESS_DENIED: it was someone else. ERROR_NOT_FOUND:
/// no control device, or no port in that slot. ERROR_NOT_SUPPORTED: the driver is
/// too old to say.
DWORD control_port_owner_is(DWORD slot, const std::string& sid)
{
    const std::wstring path = control_device_path();
    if (path.empty())
        return ERROR_NOT_FOUND;
    unique_handle control(::CreateFileW(path.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr));
    if (control.get() == INVALID_HANDLE_VALUE)
        return ERROR_NOT_FOUND;

    VSSCOM_CTL_PORT in{};
    in.Slot = slot;
    VSSCOM_CTL_OWNER out{};
    DWORD returned = 0;
    if (!::DeviceIoControl(control.get(),
                           static_cast<DWORD>(IOCTL_VSSCOM_CTL_GET_OWNER), &in,
                           sizeof(in), &out, sizeof(out), &returned, nullptr))
    {
        switch (::GetLastError())
        {
        case ERROR_NOT_FOUND:
            return ERROR_NOT_FOUND;
        case ERROR_INVALID_FUNCTION:
            return ERROR_NOT_SUPPORTED;
        default:
            return ERROR_ACCESS_DENIED;
        }
    }
    if (returned != sizeof(out) || out.Valid == 0 ||
        out.SidLength != sid.size() || out.SidLength > sizeof(out.Sid) ||
        !::IsValidSid(const_cast<char*>(sid.data())) ||
        !::EqualSid(out.Sid, const_cast<char*>(sid.data())))
        return ERROR_ACCESS_DENIED;
    return ERROR_SUCCESS;
}

/// The user of the caller at the other end of the pipe, as a binary SID.
/// Needs the pipe to have delivered a message already.
bool caller_sid(HANDLE pipe, std::string& sid)
{
    if (!::ImpersonateNamedPipeClient(pipe))
        return false;

    HANDLE raw = nullptr;
    // OpenAsSelf: the check uses the service's own token, because the thread
    // token is at identification level.
    const BOOL opened =
        ::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &raw);
    ::RevertToSelf();
    if (!opened)
        return false;
    unique_handle token(raw);

    DWORD size = 0;
    (void)::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0)
        return false;
    std::vector<unsigned char> storage(size);
    if (!::GetTokenInformation(token.get(), TokenUser, storage.data(), size,
                               &size))
        return false;

    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    if (!::IsValidSid(user->User.Sid))
        return false;
    const DWORD length = ::GetLengthSid(user->User.Sid);
    if (length == 0 || length > VSSCOM_HELPER_MAX_SID_BYTES)
        return false;
    sid.assign(reinterpret_cast<const char*>(user->User.Sid), length);
    return true;
}

/// One client of the pipe, on a thread of its own. It owns the devices that the
/// client asked for and closes them when the client goes.
class connection
{
public:
    connection(std::shared_ptr<shared_state> state, HANDLE pipe)
        : state_(std::move(state)), pipe_(pipe),
          event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
    }

    void run()
    {
        serve();
        for (auto& [slot, device] : devices_)
            drop(slot, device);
        devices_.clear();

        ::DisconnectNamedPipe(pipe_.get());
        pipe_.reset();

        const std::lock_guard lock(state_->mutex);
        if (!owner_.empty())
        {
            auto found = state_->connections_of.find(owner_);
            if (found != state_->connections_of.end() && --found->second == 0)
                state_->connections_of.erase(found);
        }
        --state_->connections;
        state_->idle.notify_all();
    }

private:
    enum class wait_result
    {
        done,
        timeout,
        stopped,
        failed
    };

    /// Waits for the overlapped operation on the pipe, or for the service to
    /// stop, or for the time to pass.
    wait_result wait(OVERLAPPED& overlapped, DWORD timeout_ms, DWORD& bytes)
    {
        const HANDLE handles[] = {event_.get(), state_->stop.get()};
        const DWORD which =
            ::WaitForMultipleObjects(2, handles, FALSE, timeout_ms);
        if (which == WAIT_OBJECT_0)
        {
            return ::GetOverlappedResult(pipe_.get(), &overlapped, &bytes,
                                         FALSE)
                       ? wait_result::done
                       : wait_result::failed;
        }

        // Stopped, timed out or failed: take the operation back before the
        // OVERLAPPED goes out of scope.
        ::CancelIoEx(pipe_.get(), &overlapped);
        (void)::GetOverlappedResult(pipe_.get(), &overlapped, &bytes, TRUE);
        if (which == WAIT_OBJECT_0 + 1)
            return wait_result::stopped;
        return which == WAIT_TIMEOUT ? wait_result::timeout
                                     : wait_result::failed;
    }

    wait_result read(VSSCOM_HELPER_REQUEST& request, DWORD timeout_ms)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event_.get();
        ::ResetEvent(event_.get());
        DWORD bytes = 0;
        if (!::ReadFile(pipe_.get(), &request, sizeof(request), &bytes,
                        &overlapped) &&
            ::GetLastError() != ERROR_IO_PENDING)
            return wait_result::failed;
        const wait_result got = wait(overlapped, timeout_ms, bytes);
        // A request is exactly one structure.
        return got == wait_result::done && bytes != sizeof(request)
                   ? wait_result::failed
                   : got;
    }

    bool write(const VSSCOM_HELPER_REPLY& reply)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = event_.get();
        ::ResetEvent(event_.get());
        DWORD bytes = 0;
        if (!::WriteFile(pipe_.get(), &reply, sizeof(reply), &bytes,
                         &overlapped) &&
            ::GetLastError() != ERROR_IO_PENDING)
            return false;
        return wait(overlapped, 5000, bytes) == wait_result::done &&
               bytes == sizeof(reply);
    }

    static VSSCOM_HELPER_REPLY reply_of(DWORD status, std::size_t slot = 0)
    {
        VSSCOM_HELPER_REPLY reply{};
        reply.Version = VSSCOM_HELPER_VERSION;
        reply.Status = status;
        reply.Slot = static_cast<ULONG>(slot);
        return reply;
    }

    void serve()
    {
        bool first = true;
        for (;;)
        {
            VSSCOM_HELPER_REQUEST request{};
            const DWORD timeout = first
                                      ? first_request_ms
                                      : (devices_.empty() ? idle_ms : INFINITE);
            const wait_result got = read(request, timeout);
            if (got != wait_result::done)
                return;

            if (first)
            {
                first = false;
                if (!caller_sid(pipe_.get(), owner_))
                {
                    owner_.clear();
                    (void)write(reply_of(ERROR_ACCESS_DENIED));
                    return;
                }
                if (!admit())
                {
                    (void)write(reply_of(ERROR_NOT_ENOUGH_QUOTA));
                    return;
                }
            }

            if (!write(handle(request)))
                return;
        }
    }

    /// Counts this connection against its user.
    bool admit()
    {
        const std::lock_guard lock(state_->mutex);
        const auto found = state_->connections_of.find(owner_);
        if (found != state_->connections_of.end() &&
            found->second >= max_connections_per_user)
        {
            owner_.clear(); // not counted, so not uncounted
            return false;
        }
        ++state_->connections_of[owner_];
        return true;
    }

    VSSCOM_HELPER_REPLY handle(const VSSCOM_HELPER_REQUEST& request)
    {
        if (request.Version != VSSCOM_HELPER_VERSION)
            return reply_of(ERROR_NOT_SUPPORTED);

        switch (request.Op)
        {
        case VSSCOM_HELPER_OP_CREATE:
            return request.Argument != 0 ? reply_of(ERROR_NOT_SUPPORTED)
                                         : create(request);
        case VSSCOM_HELPER_OP_DESTROY:
            return request.Argument != 0 ? reply_of(ERROR_NOT_SUPPORTED)
                                         : destroy(request);
        case VSSCOM_HELPER_OP_SET_COM_NUMBER:
            return set_number(request, false);
        case VSSCOM_HELPER_OP_SET_COM_NUMBER_KM:
            return set_number(request, true);
        default:
            return reply_of(ERROR_NOT_SUPPORTED);
        }
    }

    /// Changes the COM number of a port. For the user-mode driver the port is one
    /// of this pipe's; for the kernel-mode driver it is one the caller made through
    /// the control device, which the control device vouches for.
    VSSCOM_HELPER_REPLY set_number(const VSSCOM_HELPER_REQUEST& request,
                                   bool kernel_mode)
    {
        if (!vss::helper::com_number_valid(request.Argument))
            return reply_of(ERROR_INVALID_PARAMETER, request.Slot);

        std::wstring instance;
        wchar_t suffix[16]{};
        swprintf_s(suffix, VssComUmPortInstanceFormat,
                   static_cast<unsigned long>(request.Slot));
        if (kernel_mode)
        {
            if (request.Slot >= VSSCOM_CTL_MAX_PORTS)
                return reply_of(ERROR_NOT_FOUND, request.Slot);
            const DWORD owned = control_port_owner_is(request.Slot, owner_);
            if (owned != ERROR_SUCCESS)
                return reply_of(owned, request.Slot);
            instance = std::wstring(VSSCOM_PORT_HARDWARE_ID) + L"\\" + suffix;
        }
        else
        {
            if (devices_.find(request.Slot) == devices_.end())
                return reply_of(ERROR_NOT_FOUND, request.Slot);
            instance = std::wstring(VssComUmPortInstancePrefix) + suffix;
        }

        const std::lock_guard lock(state_->com_mutex);
        return reply_of(set_com_number(instance, request.Argument,
                                       state_->stop.get()),
                        request.Slot);
    }

    VSSCOM_HELPER_REPLY create(const VSSCOM_HELPER_REQUEST& request)
    {
        if (request.Slot != 0)
            return reply_of(ERROR_NOT_SUPPORTED);

        const auto grant = state_->leases.acquire(owner_);
        if (grant.status == vss::helper::lease_status::owner_quota)
            return reply_of(ERROR_NOT_ENOUGH_QUOTA);
        if (grant.status != vss::helper::lease_status::granted)
            return reply_of(ERROR_NO_SYSTEM_RESOURCES);

        HSWDEVICE device = nullptr;
        const DWORD error =
            make_device(grant.slot, owner_, state_->stop.get(), device);
        if (error != ERROR_SUCCESS)
        {
            (void)state_->leases.release(owner_, grant.slot);
            return reply_of(error);
        }

        devices_[grant.slot] = device;
        return reply_of(ERROR_SUCCESS, grant.slot);
    }

    VSSCOM_HELPER_REPLY destroy(const VSSCOM_HELPER_REQUEST& request)
    {
        const auto found = devices_.find(request.Slot);
        if (found == devices_.end())
            return reply_of(ERROR_NOT_FOUND);

        drop(found->first, found->second);
        devices_.erase(found);
        return reply_of(ERROR_SUCCESS);
    }

    /// Closes the device and gives the slot back once Windows has finished with
    /// it, so that a new port never meets the old one still being removed.
    void drop(std::size_t slot, HSWDEVICE device)
    {
        ::SwDeviceClose(device);
        (void)state_->leases.release(owner_, slot);
    }

    std::shared_ptr<shared_state> state_;
    unique_handle pipe_;
    unique_handle event_;
    std::string owner_;
    std::map<std::size_t, HSWDEVICE> devices_;
};

/// The pipe may be opened by any signed-in user and by no machine on the
/// network. SYSTEM and administrators may do everything.
struct pipe_security
{
    pipe_security()
    {
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = FALSE;
        attributes.lpSecurityDescriptor = nullptr;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1,
                &attributes.lpSecurityDescriptor, nullptr))
            attributes.lpSecurityDescriptor = nullptr;
    }

    ~pipe_security()
    {
        if (attributes.lpSecurityDescriptor != nullptr)
            ::LocalFree(attributes.lpSecurityDescriptor);
    }

    pipe_security(const pipe_security&) = delete;
    pipe_security& operator=(const pipe_security&) = delete;

    bool ok() const noexcept
    {
        return attributes.lpSecurityDescriptor != nullptr;
    }

    SECURITY_ATTRIBUTES attributes{};
};

HANDLE make_pipe(SECURITY_ATTRIBUTES* attributes, bool first)
{
    // FILE_FLAG_FIRST_PIPE_INSTANCE: if something else already has the name,
    // this fails and the service does not start, rather than serve a pipe that
    // somebody else could be listening on too.
    return ::CreateNamedPipeW(VssComHelperPipeName,
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                  (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                  PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                              PIPE_UNLIMITED_INSTANCES, 512, 512, 0,
                              attributes);
}

/// Accepts clients until `state->stop` is set, then waits for the connections
/// to close their devices. Returns the Win32 error that ended it, or zero.
DWORD serve(const std::shared_ptr<shared_state>& state)
{
    pipe_security security;
    if (!security.ok())
        return ::GetLastError();

    unique_handle connected(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!connected || !state->stop)
        return ERROR_OUTOFMEMORY;

    DWORD result = ERROR_SUCCESS;
    // The instance that waits for the next client. The first one has the
    // first-instance flag, so that the name is ours.
    unique_handle pending(make_pipe(&security.attributes, true));
    if (pending.get() == INVALID_HANDLE_VALUE)
    {
        trace(L"cannot create the pipe");
        return ::GetLastError();
    }

    for (;;)
    {
        OVERLAPPED overlapped{};
        overlapped.hEvent = connected.get();
        ::ResetEvent(connected.get());
        if (!::ConnectNamedPipe(pending.get(), &overlapped))
        {
            const DWORD error = ::GetLastError();
            if (error == ERROR_IO_PENDING)
            {
                const HANDLE handles[] = {connected.get(), state->stop.get()};
                if (::WaitForMultipleObjects(2, handles, FALSE, INFINITE) !=
                    WAIT_OBJECT_0)
                {
                    ::CancelIoEx(pending.get(), &overlapped);
                    DWORD ignored = 0;
                    (void)::GetOverlappedResult(pending.get(), &overlapped,
                                                &ignored, TRUE);
                    break;
                }
            }
            else if (error != ERROR_PIPE_CONNECTED)
            {
                // A client that went away at once: wait for the next on the
                // same instance.
                ::DisconnectNamedPipe(pending.get());
                continue;
            }
        }

        // A new instance listens before this client is handed on, so that the
        // next client never finds every instance of the pipe busy.
        unique_handle pipe = std::move(pending);
        pending.reset(make_pipe(&security.attributes, false));
        if (pending.get() == INVALID_HANDLE_VALUE)
        {
            result = ::GetLastError();
            trace(L"cannot create the pipe");
            break;
        }

        {
            const std::lock_guard lock(state->mutex);
            if (state->connections >= max_connections)
            {
                ::DisconnectNamedPipe(pipe.get());
                continue;
            }
            ++state->connections;
        }

        auto client = std::make_shared<connection>(state, pipe.release());
        try
        {
            std::thread([client] { client->run(); }).detach();
        }
        catch (...)
        {
            client->run(); // no thread: the pipe is closed by run() at once
        }
    }

    // Wait for the connections: each closes its devices before it goes.
    std::unique_lock lock(state->mutex);
    state->idle.wait_for(lock, std::chrono::seconds(30),
                         [&] { return state->connections == 0; });
    return result;
}

// ---------------------------------------------------------------------------
// The service.
// ---------------------------------------------------------------------------

SERVICE_STATUS_HANDLE service_handle = nullptr;
std::shared_ptr<shared_state> service_state;

void report(DWORD current, DWORD exit_code = ERROR_SUCCESS)
{
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = current;
    status.dwControlsAccepted =
        current == SERVICE_RUNNING
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)
            : 0;
    status.dwWin32ExitCode = exit_code;
    status.dwWaitHint = current == SERVICE_STOP_PENDING ? 35000 : 0;
    ::SetServiceStatus(service_handle, &status);
}

DWORD WINAPI service_control(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
    {
        report(SERVICE_STOP_PENDING);
        ::SetEvent(service_state->stop.get());
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR
                                                  : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*)
{
    service_state = std::make_shared<shared_state>();
    service_handle = ::RegisterServiceCtrlHandlerExW(VssComHelperServiceName,
                                                     service_control, nullptr);
    if (service_handle == nullptr)
        return;

    report(SERVICE_RUNNING);
    const DWORD result = serve(service_state);
    report(SERVICE_STOPPED, result);
}

BOOL WINAPI console_control(DWORD)
{
    if (service_state)
        ::SetEvent(service_state->stop.get());
    return TRUE;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc > 1 && std::wcscmp(argv[1], L"--console") == 0)
    {
        service_state = std::make_shared<shared_state>();
        ::SetConsoleCtrlHandler(console_control, TRUE);
        std::fputws(L"vsscom_helper: serving; Ctrl+C stops it\n", stdout);
        std::fflush(stdout);
        const DWORD result = serve(service_state);
        if (result != ERROR_SUCCESS)
            std::fwprintf(stderr, L"vsscom_helper: stopped with error %lu\n",
                          static_cast<unsigned long>(result));
        return result == ERROR_SUCCESS ? 0 : 1;
    }

    wchar_t name[sizeof(VssComHelperServiceName) / sizeof(WCHAR)];
    std::wmemcpy(name, VssComHelperServiceName,
                 sizeof(VssComHelperServiceName) / sizeof(WCHAR));
    const SERVICE_TABLE_ENTRYW table[] = {{name, service_main},
                                          {nullptr, nullptr}};
    return ::StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
