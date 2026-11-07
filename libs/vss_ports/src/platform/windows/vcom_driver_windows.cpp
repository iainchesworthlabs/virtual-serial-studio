//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The studio's side of the vsscom drivers: asking for a port, finding the ports
// that exist, and opening the hub end of one.
// drivers/windows/vsscom/public.h says what the kernel-mode driver expects; its
// control device makes the ports. The user-mode driver has no control device, and
// the vsscom_helper service makes its ports (drivers/windows/vsscom_helper/
// protocol.h). Both kinds of port answer the same hub requests.
//

#include <vss/core/profile.hpp>

#include "../vcom_driver_platform.hpp"

#include <vss/core/error.hpp>

#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/windows/stream_handle.hpp>

#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h> // MAX_DEVICE_ID_LEN
#include <devpropdef.h>
// Defines the interface GUID of public.h in this file only.
#include <initguid.h>

#include <vsscom/public.h>
#include <vsscom_helper/protocol.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vss::detail
{
namespace
{

std::string to_utf8(const std::wstring& text)
{
    if (text.empty())
        return {};

    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};

    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                          static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
}

/// The last step of making a port that failed in this process, so a refusal can
/// say which call it came from.
std::mutex failure_mutex;
driver_failure failure_record;

void note_failure(const char* step, DWORD code)
{
    const std::lock_guard lock(failure_mutex);
    failure_record.step = step;
    failure_record.code = code;
}

/// One port of the driver: the COM name an application opens, the path of the
/// hub end, and the device instance id (VSSCOM\PORT\0000 for a port that the
/// control device made).
struct hub_interface
{
    std::wstring port;
    std::wstring path;
    std::wstring instance;
};

std::wstring read_port_name(HDEVINFO set, SP_DEVINFO_DATA& info)
{
    const auto key = ::SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0,
                                            DIREG_DEV, KEY_QUERY_VALUE);
    if (key == INVALID_HANDLE_VALUE)
        return {};

    wchar_t buffer[32]{};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    const LONG status = ::RegQueryValueExW(
        key, L"PortName", nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
        &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ)
        return {};
    return buffer;
}

/// The present ports of the driver, in the order Windows lists them.
std::vector<hub_interface> enumerate_hubs()
{
    VSS_PROFILE_ZONE("ports.vcom.enumerate_hubs");
    std::vector<hub_interface> hubs;
    GUID guid = GUID_DEVINTERFACE_VSSCOM;
    const HDEVINFO set = ::SetupDiGetClassDevsW(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return hubs;

    for (DWORD index = 0;; ++index)
    {
        SP_DEVICE_INTERFACE_DATA data{};
        data.cbSize = sizeof(data);
        if (!::SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &data))
            break;

        SP_DEVINFO_DATA info{};
        info.cbSize = sizeof(info);
        DWORD required = 0;
        ::SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &required,
                                           &info);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
            continue;

        std::vector<unsigned char> storage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!::SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required,
                                                nullptr, &info))
            continue;

        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (!::SetupDiGetDeviceInstanceIdW(set, &info, instance,
                                           MAX_DEVICE_ID_LEN, nullptr))
            instance[0] = L'\0';

        hubs.push_back({read_port_name(set, info), detail->DevicePath, instance});
    }

    ::SetupDiDestroyDeviceInfoList(set);
    return hubs;
}

/// The path of the control device's interface, or empty when the control
/// device is not installed.
std::wstring find_control_path()
{
    VSS_PROFILE_ZONE("ports.vcom.find_control");
    GUID guid = GUID_DEVINTERFACE_VSSCOMCTL;
    const HDEVINFO set = ::SetupDiGetClassDevsW(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return {};

    std::wstring path;
    SP_DEVICE_INTERFACE_DATA data{};
    data.cbSize = sizeof(data);
    if (::SetupDiEnumDeviceInterfaces(set, nullptr, &guid, 0, &data))
    {
        DWORD required = 0;
        ::SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &required,
                                           nullptr);
        if (required >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
        {
            std::vector<unsigned char> storage(required);
            auto* detail =
                reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
                    storage.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (::SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required,
                                                   nullptr, nullptr))
                path = detail->DevicePath;
        }
    }

    ::SetupDiDestroyDeviceInfoList(set);
    return path;
}

/// A handle that is closed when it goes out of scope.
struct handle_closer
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle);
    }
};

using unique_handle = std::unique_ptr<void, handle_closer>;

/// Opens the control device. A plain handle: its requests are short and waited
/// for. A port made through it lives as long as this handle does.
HANDLE open_control(const std::wstring& path, DWORD& error)
{
    VSS_PROFILE_ZONE("ports.vcom.open_control");
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    error = handle == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
    return handle;
}

bool control_request(HANDLE handle, DWORD code, void* input, DWORD input_size,
                     void* output, DWORD output_size, DWORD& error)
{
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(handle, code, input, input_size, output,
                                      output_size, &returned, nullptr);
    error = ok ? 0 : ::GetLastError();
    return ok != FALSE && returned == output_size;
}

/// Opens the hub end. An overlapped handle, so that asio can use it.
HANDLE open_hub(const std::wstring& path, DWORD& error)
{
    VSS_PROFILE_ZONE("ports.vcom.open_hub");
    const auto handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    error = handle == INVALID_HANDLE_VALUE ? ::GetLastError() : 0;
    return handle;
}

/// A control request on an overlapped handle, waited for here. The low bit of
/// the event handle keeps the completion off the I/O completion port that asio
/// reads, which would not know what to do with it.
bool control(HANDLE handle, DWORD code, void* input, DWORD input_size,
             void* output, DWORD output_size, DWORD* returned = nullptr)
{
    const auto event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr)
        return false;

    OVERLAPPED overlapped{};
    // Setting the low bit of OVERLAPPED::hEvent keeps the completion off the
    // I/O completion port, as the Win32 documentation describes.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    overlapped.hEvent = reinterpret_cast<HANDLE>(
        reinterpret_cast<ULONG_PTR>(event) | 1);

    DWORD count = 0;
    BOOL ok = ::DeviceIoControl(handle, code, input, input_size, output,
                                output_size, nullptr, &overlapped);
    if (!ok && ::GetLastError() != ERROR_IO_PENDING)
    {
        ::CloseHandle(event);
        return false;
    }
    ok = ::GetOverlappedResult(handle, &overlapped, &count, TRUE);
    ::CloseHandle(event);

    if (returned != nullptr)
        *returned = count;
    return ok != FALSE;
}

bool read_state(HANDLE handle, VSSCOM_HUB_STATE& state)
{
    DWORD returned = 0;
    return control(handle, static_cast<DWORD>(IOCTL_VSSCOM_HUB_GET_STATE), nullptr, 0, &state,
                   sizeof(state), &returned) &&
           returned == sizeof(state);
}

bool read_version(HANDLE handle, VSSCOM_VERSION_INFO& version)
{
    DWORD returned = 0;
    return control(handle, static_cast<DWORD>(IOCTL_VSSCOM_GET_VERSION), nullptr, 0, &version,
                   sizeof(version), &returned) &&
           returned == sizeof(version);
}

/// Opens the pipe of the helper service. `error` is the Win32 error when it
/// cannot. A busy pipe is waited for a little: the helper serves a client on
/// each instance and makes the next at once.
HANDLE open_helper(DWORD& error)
{
    VSS_PROFILE_ZONE("ports.vcom.open_helper");
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        // Identification only: the helper needs to know the user and nothing
        // more, and has no reason to act as the user.
        const auto handle = ::CreateFileW(
            VssComHelperPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE)
        {
            error = 0;
            return handle;
        }
        error = ::GetLastError();
        if (error != ERROR_PIPE_BUSY)
            break;
        ::WaitNamedPipeW(VssComHelperPipeName, 1000);
    }
    return INVALID_HANDLE_VALUE;
}

/// What the helper answers when it could not be asked at all.
constexpr DWORD helper_failed = 0xFFFFFFFFu;

/// One request to the helper on a pipe in message mode. Waits for the answer:
/// the helper replies when the work is done, which can take many seconds. The
/// Win32 error it answered, or `helper_failed`.
DWORD helper_ask(HANDLE pipe, ULONG op, ULONG slot, ULONG argument)
{
    VSS_PROFILE_ZONE_NAMED(zone, "ports.vcom.helper_ask");
    VSS_PROFILE_ZONE_VALUE(zone, op);
    VSSCOM_HELPER_REQUEST request{};
    request.Version = VSSCOM_HELPER_VERSION;
    request.Op = op;
    request.Slot = slot;
    request.Argument = argument;
    VSSCOM_HELPER_REPLY reply{};
    DWORD returned = 0;
    if (!::TransactNamedPipe(pipe, &request, sizeof(request), &reply,
                             sizeof(reply), &returned, nullptr) ||
        returned != sizeof(reply) || reply.Version != VSSCOM_HELPER_VERSION)
        return helper_failed;
    return reply.Status;
}

/// The library's error for a Win32 error that the helper answered to a request
/// to change a COM number.
errc errc_of_com_status(DWORD status) noexcept
{
    switch (status)
    {
    case ERROR_SUCCESS:
        return errc::success;
    case ERROR_INVALID_PARAMETER:
        return errc::invalid_argument;
    case ERROR_ALREADY_ASSIGNED:
        return errc::already_exists;
    case ERROR_BUSY:
        return errc::device_busy;
    case ERROR_NOT_FOUND:
        return errc::not_found;
    case ERROR_ACCESS_DENIED:
        return errc::permission_denied;
    case ERROR_NOT_SUPPORTED: // a helper from before COM numbers
        return errc::unsupported;
    case ERROR_TIMEOUT:
        return errc::timeout;
    default:
        return errc::io_error;
    }
}

/// How a port came to be, which decides whether its number can be changed: only
/// a port that something made for this process, and holds for it, has a lease the
/// helper can check.
struct port_origin
{
    /// Made through the control device or the helper, and kept alive by the lease.
    bool leased = false;
    /// The control device made it (and the lease is that handle). Otherwise the
    /// helper did, and the lease is the pipe that asked.
    bool kernel_mode = false;
    ULONG slot = 0;
    /// The device instance id, as the interface enumeration reports it.
    std::wstring instance;
};

/// Asks the helper to give a port the COM number. A port of the user-mode driver
/// belongs to the pipe that made it; a port of the kernel-mode driver belongs to
/// the control handle, and the helper asks the control device who made it, so
/// that request goes on a pipe of its own.
errc ask_com_number(const port_origin& origin, HANDLE lease, unsigned number)
{
    VSS_PROFILE_ZONE("ports.vcom.ask_com_number");
    unique_handle own;
    HANDLE pipe = lease;
    if (origin.kernel_mode)
    {
        DWORD error = 0;
        own.reset(open_helper(error));
        if (own.get() == INVALID_HANDLE_VALUE)
        {
            note_failure("open the helper pipe to set the COM number", error);
            if (error == ERROR_ACCESS_DENIED)
                return errc::permission_denied;
            // No helper is installed: a COM number needs it.
            return error == ERROR_FILE_NOT_FOUND ? errc::unsupported
                                                 : errc::io_error;
        }
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!::SetNamedPipeHandleState(own.get(), &mode, nullptr, nullptr))
            return errc::io_error;
        pipe = own.get();
    }

    const DWORD status = helper_ask(
        pipe,
        origin.kernel_mode ? VSSCOM_HELPER_OP_SET_COM_NUMBER_KM
                           : VSSCOM_HELPER_OP_SET_COM_NUMBER,
        origin.slot, number);
    if (status == helper_failed)
    {
        note_failure("talk to the helper about the COM number",
                     ::GetLastError());
        return errc::io_error;
    }
    if (status != ERROR_SUCCESS)
        note_failure("the helper's answer to the COM number request", status);
    return errc_of_com_status(status);
}

bool same_name(const std::string& a, const std::string& b)
{
    return a.size() == b.size() &&
           ::_strnicmp(a.c_str(), b.c_str(), a.size()) == 0;
}

/// Waits until the port with the instance id has a hub interface, and, when
/// `expected` is not empty, until its COM name is that. Windows takes the
/// interface away while it restarts a port and brings it back with the new name.
std::optional<hub_interface> wait_for_hub(const std::wstring& instance,
                                          const std::wstring& expected,
                                          std::chrono::seconds limit)
{
    VSS_PROFILE_ZONE("ports.vcom.wait_for_hub");
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + limit;
    for (;;)
    {
        for (auto& hub : enumerate_hubs())
        {
            if (hub.port.empty() ||
                ::_wcsicmp(hub.instance.c_str(), instance.c_str()) != 0)
                continue;
            if (!expected.empty() &&
                ::_wcsicmp(hub.port.c_str(), expected.c_str()) != 0)
                continue;
            // hub is an element of the temporary list, so it has to be moved out.
            // cppcheck-suppress returnStdMoveLocal
            return std::move(hub);
        }
        if (clock::now() >= deadline)
            return std::nullopt;
        ::Sleep(100);
    }
}

/// The local time with milliseconds, for the log lines a hung program is
/// diagnosed from.
std::string clock_text()
{
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    ::localtime_s(&local, &seconds);
    char text[40]{};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(millis));
    return text;
}

class driver_transport;
void registry_add(const std::shared_ptr<driver_transport>& transport);
void registry_remove(const driver_transport* transport);

/// The reads and writes that are on the hub handle, so that a change of COM
/// number can hold them back and give them to the new handle. Shared with the
/// completion handlers, which can run after the transport has gone.
struct flight
{
    struct read_slot
    {
        boost::asio::mutable_buffer buffer;
        transport::io_handler handler;
    };
    struct write_slot
    {
        boost::asio::const_buffer buffer;
        transport::io_handler handler;
    };

    std::mutex mutex;
    std::condition_variable idle;
    /// The handle is being swapped: new operations wait in a slot, and an
    /// operation that comes back cancelled goes into its slot.
    bool renaming = false;
    unsigned reads = 0; // operations the handle still has
    unsigned writes = 0;
    std::optional<read_slot> held_read;
    std::optional<write_slot> held_write;
};

/// The hub end of one driver port, as the byte channel under an endpoint.
///
/// What the application writes arrives through reads, and what is written
/// here is what the application reads. From the application's side this end
/// plays the device, so CTS, DSR, DCD and RI are the lines it sets, and DTR,
/// RTS and break are the lines the application drives.
///
/// A second thread waits for the driver to announce a change (the program
/// opened or closed the port, or chose other settings), keeps the answer, and
/// logs each change of "a program has the port open" against the endpoint with
/// the time it happened.
// NOLINTNEXTLINE(misc-multiple-inheritance): shared_from_this is a mixin.
class driver_transport final
    : public transport,
      public std::enable_shared_from_this<driver_transport>
{
public:
    /// `lease` is the control handle that made the port, or the helper's pipe,
    /// or empty for a port that was there already. It is closed after the hub
    /// end, and closing it removes the port.
    driver_transport(const executor_type& executor, HANDLE handle,
                     std::string port, std::string name,
                     unique_handle lease = {}, port_origin origin = {})
        : executor_(executor), handle_(executor, handle), raw_(handle),
          port_(std::move(port)), name_(std::move(name)),
          lease_(std::move(lease)), origin_(std::move(origin)),
          flight_(std::make_shared<flight>())
    {
        refresh_lines();
        start_watch(raw_);
    }

    /// Makes the transport and lists it among those this process holds, which is
    /// how `driver_peer` and `set_driver_com_number` find it.
    static std::shared_ptr<driver_transport> create(
        const executor_type& executor, HANDLE handle, std::string port,
        std::string name, unique_handle lease = {}, port_origin origin = {})
    {
        auto made = std::make_shared<driver_transport>(
            executor, handle, std::move(port), std::move(name),
            std::move(lease), std::move(origin));
        registry_add(made);
        return made;
    }

    ~driver_transport() override
    {
        try
        {
            close();
        }
        catch (...)
        {
            // best effort: a destructor must not throw, and nothing can act on a failed stop
        }
    }

    std::string describe() const override
    {
        std::lock_guard lock(mutex_);
        return "Virtual Serial Studio COM port " + port_ + " (" + name_ + ")";
    }

    std::string public_path() const override
    {
        std::lock_guard lock(mutex_);
        return port_;
    }

    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        start_read(buffer, std::move(handler));
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        start_write(buffer, std::move(handler));
    }

    void close() override
    {
        VSS_PROFILE_ZONE("ports.vcom.close");
        if (!open_.exchange(false))
            return;
        registry_remove(this);

        // A change of number in another thread finishes first: it ends because
        // the transport is no longer open.
        const std::lock_guard rename_lock(rename_mutex_);
        stop_watch();

        std::optional<flight::read_slot> read;
        std::optional<flight::write_slot> write;
        {
            std::lock_guard lock(flight_->mutex);
            flight_->renaming = false;
            read.swap(flight_->held_read);
            write.swap(flight_->held_write);
        }
        abort_held(std::move(read), std::move(write));

        {
            std::lock_guard lock(mutex_);
            notice_ = {};
            raw_ = INVALID_HANDLE_VALUE;
        }
        boost::system::error_code ignored;
        handle_.cancel(ignored);
        handle_.close(ignored);
        // The port was made for this transport and goes with it.
        lease_.reset();
    }

    void cancel() override
    {
        boost::system::error_code ignored;
        handle_.cancel(ignored);
    }

    bool supports_control_lines() const noexcept override { return true; }

    void set_control_line(control_line line, bool state) override
    {
        VSS_PROFILE_ZONE("ports.vcom.set_line");
        // Only the inputs are ours to drive; DTR, RTS and break belong to the
        // application.
        ULONG bit = 0;
        switch (line)
        {
        case control_line::cts: bit = VSSCOM_LINE_CTS; break;
        case control_line::dsr: bit = VSSCOM_LINE_DSR; break;
        case control_line::dcd: bit = VSSCOM_LINE_DCD; break;
        case control_line::ri: bit = VSSCOM_LINE_RI; break;
        default: return;
        }

        std::lock_guard lock(mutex_);
        ULONG next = hub_lines_;
        if (state)
            next |= bit;
        else
            next &= ~bit;
        // While the port is being renumbered the lines are kept and given to
        // the new handle.
        if (!open_.load())
            return;
        if (raw_ != INVALID_HANDLE_VALUE &&
            !control(raw_, static_cast<DWORD>(IOCTL_VSSCOM_HUB_SET_LINES), &next,
                     sizeof(next), nullptr, 0))
            return;
        hub_lines_ = next;
        lines_.set(line, state);
    }

    control_lines lines() const override
    {
        std::lock_guard lock(mutex_);
        return lines_;
    }

    void refresh_lines() override
    {
        VSS_PROFILE_ZONE("ports.vcom.refresh_lines");
        VSSCOM_HUB_STATE state{};
        {
            std::lock_guard lock(mutex_);
            if (!open_.load() || raw_ == INVALID_HANDLE_VALUE ||
                !read_state(raw_, state))
                return;
            lines_.set(control_line::dtr, state.Dtr != 0);
            lines_.set(control_line::rts, state.Rts != 0);
            lines_.set(control_line::break_signal, state.Break != 0);
        }
        apply_state(state);
    }

    void set_notice_sink(notice_sink sink) override
    {
        std::string text;
        {
            std::lock_guard lock(mutex_);
            notice_ = sink;
            if (sink && peer_known_ && peer_.open)
                text = port_ + ": a program already has the port open (process " +
                       std::to_string(peer_.process_id) + "), seen at " +
                       clock_text();
        }
        if (!text.empty())
            sink(std::move(text));
    }

    /// What the driver last said about the program on the port.
    std::optional<driver_peer_info> peer() const
    {
        std::lock_guard lock(mutex_);
        if (!peer_known_)
            return std::nullopt;
        return peer_;
    }

    /// The baud rate and framing the program set, while a program has the
    /// port open.  Before it calls `SetCommState` the driver reports its own
    /// defaults, which are not a choice the program made, so a port nobody has
    /// open reports nothing.
    std::optional<line_format> peer_format() const override
    {
        std::lock_guard lock(mutex_);
        if (!peer_known_ || !peer_.open || peer_.baud_rate == 0)
            return std::nullopt;

        line_format out;
        out.baud_rate = peer_.baud_rate;
        out.data_bits = peer_.data_bits;
        switch (peer_.parity)
        {
        case 0: out.parity = parity_mode::none; break;
        case 1: out.parity = parity_mode::odd; break;
        case 2: out.parity = parity_mode::even; break;
        case 3: out.parity = parity_mode::mark; break;
        case 4: out.parity = parity_mode::space; break;
        default: return std::nullopt;
        }
        switch (peer_.stop_bits)
        {
        case 0: out.stop_bits = stop_bits_mode::one; break;
        case 1: out.stop_bits = stop_bits_mode::one_point_five; break;
        case 2: out.stop_bits = stop_bits_mode::two; break;
        default: return std::nullopt;
        }
        return out;
    }

    bool named(const std::string& com_name) const
    {
        std::lock_guard lock(mutex_);
        return same_name(port_, com_name);
    }

    /// Takes the port back from its program: see `revoke_driver_port`.
    result<std::uint32_t> revoke()
    {
        if (!open_.load())
            return make_error_code(errc::not_open);
        // The request ends at once, so the lock is held across it, as it is
        // for the state read.  The hub end is not swapped while it is held.
        std::lock_guard lock(mutex_);
        if (raw_ == INVALID_HANDLE_VALUE)
            return make_error_code(errc::not_open);
        ULONG process = 0;
        DWORD returned = 0;
        if (!control(raw_, static_cast<DWORD>(IOCTL_VSSCOM_HUB_REVOKE), nullptr, 0,
                     &process, sizeof(process), &returned) ||
            returned != sizeof(process))
            return make_error_code(errc::io_error);
        return static_cast<std::uint32_t>(process);
    }

    /// Changes the COM number: see `set_driver_com_number`.
    result<std::string> renumber(unsigned number)
    {
        VSS_PROFILE_ZONE("ports.vcom.renumber");
        VSS_PROFILE_MESSAGE("ports.vcom.renumber");
        const std::lock_guard rename_lock(rename_mutex_);
        if (!open_.load())
            return make_error_code(errc::not_open);
        if (!origin_.leased)
            return make_error_code(errc::unsupported);

        const std::string wanted = "COM" + std::to_string(number);
        {
            std::lock_guard lock(mutex_);
            if (same_name(port_, wanted))
                return port_;
        }

        // The restart would fail with a program on the port, and so would the
        // user who asked for it, so say so first.
        VSSCOM_HUB_STATE state{};
        {
            std::lock_guard lock(mutex_);
            if (raw_ == INVALID_HANDLE_VALUE || !read_state(raw_, state))
                return make_error_code(errc::io_error);
        }
        if (state.AppOpen != 0)
            return make_error_code(errc::device_busy);

        begin_swap();
        const errc asked = ask_com_number(origin_, lease_.get(), number);

        // Whether it worked or not, the port is back (under the new name or the
        // old one) and the hub end opens again.
        const std::wstring expected =
            asked == errc::success
                ? std::wstring(wanted.begin(), wanted.end())
                : std::wstring();
        if (!finish_swap(expected))
        {
            // Nothing to read from any more: the endpoint hears of it as a
            // failed read.
            fail_held();
            return make_error_code(errc::io_error);
        }
        if (asked != errc::success)
            return make_error_code(asked);
        std::lock_guard lock(mutex_);
        return port_;
    }

private:
    // -- Reads and writes -------------------------------------------------

    void start_read(const boost::asio::mutable_buffer& buffer,
                    io_handler handler)
    {
        VSS_PROFILE_ZONE("ports.vcom.read");
        const std::lock_guard lock(flight_->mutex);
        if (flight_->renaming)
        {
            flight_->held_read = flight::read_slot{buffer, std::move(handler)};
            return;
        }
        ++flight_->reads;
        auto shared = flight_;
        handle_.async_read_some(
            buffer,
            [shared, buffer, handler = std::move(handler)](
                boost::system::error_code ec, std::size_t count) mutable
            {
                {
                    std::unique_lock lock(shared->mutex);
                    --shared->reads;
                    shared->idle.notify_all();
                    // The handle went away for a change of number: the read
                    // goes to the new handle, and the endpoint never knew.
                    if (ec == boost::asio::error::operation_aborted &&
                        shared->renaming)
                    {
                        shared->held_read =
                            flight::read_slot{buffer, std::move(handler)};
                        return;
                    }
                }
                handler(ec, count);
            });
    }

    void start_write(const boost::asio::const_buffer& buffer,
                     io_handler handler)
    {
        VSS_PROFILE_ZONE("ports.vcom.write");
        const std::lock_guard lock(flight_->mutex);
        if (flight_->renaming)
        {
            flight_->held_write = flight::write_slot{buffer, std::move(handler)};
            return;
        }
        ++flight_->writes;
        auto shared = flight_;
        handle_.async_write_some(
            buffer,
            [shared, buffer, handler = std::move(handler)](
                boost::system::error_code ec, std::size_t count) mutable
            {
                {
                    std::unique_lock lock(shared->mutex);
                    --shared->writes;
                    shared->idle.notify_all();
                    if (ec == boost::asio::error::operation_aborted &&
                        shared->renaming)
                    {
                        shared->held_write =
                            flight::write_slot{buffer, std::move(handler)};
                        return;
                    }
                }
                handler(ec, count);
            });
    }

    /// Gives back, as cancelled, the operations a swap was holding.
    void abort_held(std::optional<flight::read_slot> read,
                    std::optional<flight::write_slot> write)
    {
        if (read)
            boost::asio::post(executor_,
                              [handler = std::move(read->handler)]
                              { handler(boost::asio::error::operation_aborted, 0); });
        if (write)
            boost::asio::post(executor_,
                              [handler = std::move(write->handler)]
                              { handler(boost::asio::error::operation_aborted, 0); });
    }

    // -- The state the driver announces ------------------------------------

    void start_watch(HANDLE handle)
    {
        watching_ = true;
        watcher_ = std::thread(
            [this, handle]
            {
                VSS_PROFILE_THREAD("vss.vcom.watch");
                watch(handle);
            });
    }

    void stop_watch()
    {
        if (!watcher_.joinable())
            return;
        watching_ = false;
        // Cancels the wait for a change, and whatever else is on the handle.
        // The caller closes the handle next, or takes the reads and writes
        // back through the flight.
        HANDLE handle;
        {
            std::lock_guard lock(mutex_);
            handle = raw_;
        }
        if (handle != INVALID_HANDLE_VALUE)
            ::CancelIoEx(handle, nullptr);
        watcher_.join();
    }

    void watch(HANDLE handle)
    {
        ULONG known = 0;
        {
            std::lock_guard lock(mutex_);
            known = generation_;
        }
        while (watching_.load())
        {
            VSSCOM_HUB_STATE state{};
            DWORD returned = 0;
            if (!control(handle, static_cast<DWORD>(IOCTL_VSSCOM_HUB_WAIT_STATE),
                         &known, sizeof(known), &state, sizeof(state), &returned) ||
                returned != sizeof(state))
            {
                // Cancelled by a stop, or by a cancel() of the transport, or the
                // port went away. Only a stop ends the watch; the rest is tried
                // again until the transport closes, so that the answer stays
                // current when a cancel was all it was.
                if (!watching_.load())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            known = state.Generation;
            apply_state(state);
        }
    }

    /// Takes in what the driver said, and logs a change of "a program has the
    /// port open".
    void apply_state(const VSSCOM_HUB_STATE& state)
    {
        VSS_PROFILE_ZONE("ports.vcom.apply_state");
        std::string text;
        notice_sink sink;
        {
            std::lock_guard lock(mutex_);
            driver_peer_info next;
            next.open = state.AppOpen != 0;
            next.process_id = state.AppOpen != 0 ? state.AppProcessId : 0;
            next.baud_rate = state.BaudRate;
            next.data_bits = state.DataBits;
            next.parity = state.Parity;
            next.stop_bits = state.StopBits;

            if (peer_known_)
            {
                if (next.open && !peer_.open)
                    text = port_ + ": a program opened the port (process " +
                           std::to_string(next.process_id) + ") at " +
                           clock_text();
                else if (!next.open && peer_.open)
                    text = port_ + ": the program closed the port (process " +
                           std::to_string(peer_.process_id) + ") at " +
                           clock_text();
                else if (next.open && next.process_id != peer_.process_id)
                    text = port_ + ": the port passed from process " +
                           std::to_string(peer_.process_id) + " to process " +
                           std::to_string(next.process_id) + " at " +
                           clock_text();
            }
            peer_ = next;
            peer_known_ = true;
            generation_ = state.Generation;
            sink = notice_;
        }
        if (!text.empty() && sink)
            sink(std::move(text));
    }

    // -- Changing the COM number ------------------------------------------

    /// Holds back the reads and writes, and closes the hub end, so that Windows
    /// can restart the port.
    void begin_swap()
    {
        VSS_PROFILE_ZONE("ports.vcom.begin_swap");
        {
            std::lock_guard lock(flight_->mutex);
            flight_->renaming = true;
        }
        stop_watch();
        {
            std::lock_guard lock(mutex_);
            raw_ = INVALID_HANDLE_VALUE;
        }
        boost::system::error_code ignored;
        handle_.cancel(ignored);
        handle_.close(ignored);

        // The cancelled operations come back on the executor's threads and
        // put themselves into their slots. The new handle must not take a new
        // operation before they have.
        std::unique_lock lock(flight_->mutex);
        flight_->idle.wait_for(lock, std::chrono::seconds(5), [this]
                               { return flight_->reads == 0 && flight_->writes == 0; });
    }

    /// Opens the hub end again, when the port is back with the name `expected`
    /// (or with any name, when it is empty), and gives it the held operations.
    bool finish_swap(const std::wstring& expected)
    {
        VSS_PROFILE_ZONE("ports.vcom.finish_swap");
        auto hub = wait_for_hub(origin_.instance, expected,
                                std::chrono::seconds(30));
        if (!hub && !expected.empty())
        {
            // The change did not take after all: the old name will do.
            hub = wait_for_hub(origin_.instance, std::wstring(),
                               std::chrono::seconds(5));
        }
        if (!hub)
            return false;

        DWORD error = 0;
        const auto handle = open_hub(hub->path, error);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        VSSCOM_VERSION_INFO version{};
        if (!read_version(handle, version) || version.Version != VSSCOM_VERSION)
        {
            ::CloseHandle(handle);
            return false;
        }

        boost::system::error_code ec;
        handle_.assign(handle, ec);
        if (ec)
        {
            ::CloseHandle(handle);
            return false;
        }

        VSSCOM_HUB_STATE state{};
        {
            std::lock_guard lock(mutex_);
            raw_ = handle;
            port_ = to_utf8(hub->port);
            // The driver starts a new hub handle with every input line low.
            if (hub_lines_ != 0)
                (void)control(raw_, static_cast<DWORD>(IOCTL_VSSCOM_HUB_SET_LINES),
                              &hub_lines_, sizeof(hub_lines_), nullptr, 0);
            if (read_state(raw_, state))
            {
                lines_.set(control_line::dtr, state.Dtr != 0);
                lines_.set(control_line::rts, state.Rts != 0);
                lines_.set(control_line::break_signal, state.Break != 0);
            }
        }
        apply_state(state);
        start_watch(handle);

        std::optional<flight::read_slot> read;
        std::optional<flight::write_slot> write;
        {
            std::lock_guard lock(flight_->mutex);
            flight_->renaming = false;
            read.swap(flight_->held_read);
            write.swap(flight_->held_write);
        }
        if (read)
            start_read(read->buffer, std::move(read->handler));
        if (write)
            start_write(write->buffer, std::move(write->handler));
        return true;
    }

    /// The hub end could not be opened again: give the held operations an error
    /// the endpoint treats as a failure, and close.
    void fail_held()
    {
        VSS_PROFILE_ZONE("ports.vcom.fail_held");
        VSS_PROFILE_MESSAGE("ports.vcom.held_failed");
        std::optional<flight::read_slot> read;
        std::optional<flight::write_slot> write;
        {
            std::lock_guard lock(flight_->mutex);
            flight_->renaming = false;
            read.swap(flight_->held_read);
            write.swap(flight_->held_write);
        }
        if (read)
            boost::asio::post(executor_,
                              [handler = std::move(read->handler)]
                              { handler(boost::asio::error::connection_aborted, 0); });
        if (write)
            boost::asio::post(executor_,
                              [handler = std::move(write->handler)]
                              { handler(boost::asio::error::connection_aborted, 0); });
    }

    executor_type executor_;
    boost::asio::windows::stream_handle handle_;
    HANDLE raw_;
    std::string port_;
    std::string name_;
    mutable std::mutex mutex_;
    control_lines lines_;
    ULONG hub_lines_ = 0;
    ULONG generation_ = 0;
    driver_peer_info peer_;
    bool peer_known_ = false;
    notice_sink notice_;
    std::atomic<bool> open_{true};
    std::atomic<bool> watching_{false};
    std::thread watcher_;
    std::mutex rename_mutex_;
    unique_handle lease_;
    port_origin origin_;
    std::shared_ptr<flight> flight_;
};

// -- The transports this process holds ---------------------------------------

std::mutex registry_mutex;
// Weakly: a transport that is closed and let go is not kept alive by the list.
std::vector<std::weak_ptr<driver_transport>> registry;

void registry_add(const std::shared_ptr<driver_transport>& transport)
{
    const std::lock_guard lock(registry_mutex);
    std::erase_if(registry, [](const auto& entry) { return entry.expired(); });
    registry.push_back(transport);
}

// Also called while the transport is being destroyed, when its entry has already
// expired.
void registry_remove(const driver_transport* transport)
{
    const std::lock_guard lock(registry_mutex);
    std::erase_if(registry,
                  [transport](const auto& entry)
                  {
                      const auto held = entry.lock();
                      return !held || held.get() == transport;
                  });
}

/// The transports this process holds, kept alive while the caller looks at them.
std::vector<std::shared_ptr<driver_transport>> registry_snapshot()
{
    std::vector<std::shared_ptr<driver_transport>> out;
    const std::lock_guard lock(registry_mutex);
    for (const auto& entry : registry)
    {
        if (auto held = entry.lock())
            out.push_back(std::move(held));
    }
    return out;
}

/// Waits for the hub end of a port that was just made, and opens it. `lease` is
/// what keeps the port alive (the control handle, or the helper's pipe); it goes
/// to the transport, and closing it removes the port. It is closed on failure.
///
/// Windows installs the driver on the new device before the hub end exists,
/// which takes a second or two. With a COM number to give the port, the helper
/// then changes it and restarts the port before the hub end is opened, so that
/// nothing has the port open.
result<virtual_port_result>
open_made_port(const transport::executor_type& executor,
               const std::string& name, port_origin origin, unique_handle lease,
               unsigned com_number)
{
    VSS_PROFILE_ZONE("ports.vcom.open_made_port");
    auto hub = wait_for_hub(origin.instance, std::wstring(),
                            std::chrono::seconds(30));
    if (!hub)
        return make_error_code(errc::timeout);

    origin.leased = true;
    if (com_number != 0)
    {
        const std::string wanted = "COM" + std::to_string(com_number);
        if (!same_name(to_utf8(hub->port), wanted))
        {
            const errc asked = ask_com_number(origin, lease.get(), com_number);
            if (asked != errc::success)
                return make_error_code(asked);

            hub = wait_for_hub(origin.instance,
                               std::wstring(wanted.begin(), wanted.end()),
                               std::chrono::seconds(30));
            if (!hub)
                return make_error_code(errc::timeout);
        }
    }

    DWORD error = 0;
    const auto handle = open_hub(hub->path, error);
    if (handle == INVALID_HANDLE_VALUE)
    {
        note_failure("open the hub end of the new port", error);
        return make_error_code(errc::io_error);
    }

    VSSCOM_VERSION_INFO version{};
    if (!read_version(handle, version) || version.Version != VSSCOM_VERSION)
    {
        note_failure("read the version of the new port", ::GetLastError());
        ::CloseHandle(handle);
        return make_error_code(errc::unsupported);
    }

    virtual_port_result out;
    out.path = to_utf8(hub->port);
    out.kind = virtual_port_kind::windows_driver;
    out.port = driver_transport::create(executor, handle, out.path, name,
                                        std::move(lease), std::move(origin));
    return out;
}

/// Asks the control device for a port and waits for Windows to build it.
result<virtual_port_result>
create_port_on_demand(const transport::executor_type& executor,
                      const std::string& name, const std::wstring& control_path,
                      unsigned com_number)
{
    VSS_PROFILE_ZONE("ports.vcom.create_on_demand");
    DWORD error = 0;
    unique_handle control(open_control(control_path, error));
    if (control.get() == INVALID_HANDLE_VALUE)
    {
        // The closer ignores an invalid handle, so there is nothing to release.
        note_failure("open the control device", error);
        return make_error_code(error == ERROR_ACCESS_DENIED
                                   ? errc::permission_denied
                                   : errc::io_error);
    }

    VSSCOM_CTL_VERSION_INFO info{};
    if (!control_request(control.get(),
                         static_cast<DWORD>(IOCTL_VSSCOM_CTL_GET_VERSION),
                         nullptr, 0, &info, sizeof(info), error) ||
        info.Version != VSSCOM_CTL_VERSION)
        return make_error_code(errc::unsupported);

    VSSCOM_CTL_CREATE request{};
    VSSCOM_CTL_PORT made{};
    if (!control_request(control.get(),
                         static_cast<DWORD>(IOCTL_VSSCOM_CTL_CREATE_PORT),
                         &request, sizeof(request), &made, sizeof(made),
                         error))
    {
        note_failure("ask the control device for a port", error);
        // No slot left on the machine, or this user holds the most they may.
        if (error == ERROR_NO_SYSTEM_RESOURCES ||
            error == ERROR_NOT_ENOUGH_QUOTA)
            return make_error_code(errc::port_exhausted);
        return make_error_code(error == ERROR_ACCESS_DENIED
                                   ? errc::permission_denied
                                   : errc::io_error);
    }

    // From here, leaving closes the control handle, which removes the port.
    wchar_t suffix[16]{};
    std::swprintf(suffix, 16, VSSCOM_PORT_INSTANCE_FORMAT,
                  static_cast<unsigned long>(made.Slot));
    port_origin origin;
    origin.kernel_mode = true;
    origin.slot = made.Slot;
    origin.instance = std::wstring(VSSCOM_PORT_HARDWARE_ID) + L"\\" + suffix;
    return open_made_port(executor, name, std::move(origin),
                          std::move(control), com_number);
}

/// Asks the helper service for a port of the user-mode driver. The helper
/// answers when the driver has started on it. The port lives as long as the pipe
/// handle, so the handle goes to the transport like the control handle does.
/// errc::unsupported when there is no helper, or it speaks another version.
result<virtual_port_result>
create_port_via_helper(const transport::executor_type& executor,
                       const std::string& name, unsigned com_number)
{
    VSS_PROFILE_ZONE("ports.vcom.create_via_helper");
    DWORD error = 0;
    unique_handle pipe(open_helper(error));
    if (pipe.get() == INVALID_HANDLE_VALUE)
    {
        note_failure("open the helper pipe", error);
        if (error == ERROR_ACCESS_DENIED)
            return make_error_code(errc::permission_denied);
        return make_error_code(error == ERROR_FILE_NOT_FOUND
                                   ? errc::unsupported
                                   : errc::io_error);
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!::SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
    {
        note_failure("set the helper pipe to message mode", ::GetLastError());
        return make_error_code(errc::io_error);
    }

    VSSCOM_HELPER_REQUEST request{};
    request.Version = VSSCOM_HELPER_VERSION;
    request.Op = VSSCOM_HELPER_OP_CREATE;
    VSSCOM_HELPER_REPLY reply{};
    DWORD returned = 0;
    // Waits while the helper makes the device and Windows starts the driver.
    if (!::TransactNamedPipe(pipe.get(), &request, sizeof(request), &reply,
                             sizeof(reply), &returned, nullptr) ||
        returned != sizeof(reply) || reply.Version != VSSCOM_HELPER_VERSION)
    {
        note_failure("talk to the helper", ::GetLastError());
        return make_error_code(errc::io_error);
    }

    if (reply.Status != ERROR_SUCCESS)
        note_failure("the helper's answer to the request for a port",
                     reply.Status);
    switch (reply.Status)
    {
    case ERROR_SUCCESS:
        break;
    case ERROR_NO_SYSTEM_RESOURCES:
    case ERROR_NOT_ENOUGH_QUOTA:
        return make_error_code(errc::port_exhausted);
    case ERROR_ACCESS_DENIED:
        return make_error_code(errc::permission_denied);
    case ERROR_NOT_SUPPORTED:
    case ERROR_DEV_NOT_EXIST: // the helper is there and the driver is not
        return make_error_code(errc::unsupported);
    case ERROR_TIMEOUT:
        return make_error_code(errc::timeout);
    default:
        return make_error_code(errc::io_error);
    }

    wchar_t suffix[16]{};
    std::swprintf(suffix, 16, VssComUmPortInstanceFormat,
                  static_cast<unsigned long>(reply.Slot));
    port_origin origin;
    origin.kernel_mode = false;
    origin.slot = reply.Slot;
    origin.instance = std::wstring(VssComUmPortInstancePrefix) + suffix;
    return open_made_port(executor, name, std::move(origin), std::move(pipe),
                          com_number);
}

} // namespace

namespace
{

/// True when the helper service has its pipe. A pipe that is busy is there.
bool helper_present()
{
    VSS_PROFILE_ZONE("ports.vcom.helper_present");
    return ::WaitNamedPipeW(VssComHelperPipeName, 1) != FALSE ||
           ::GetLastError() != ERROR_FILE_NOT_FOUND;
}


/// True when the key exists in the driver store's list of hardware IDs. The key
/// is readable by every user. `unknown` when it could not be asked.
driver_fact store_lists(const wchar_t* path)
{
    HKEY key = nullptr;
    const LONG opened =
        ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key);
    if (opened == ERROR_SUCCESS)
    {
        ::RegCloseKey(key);
        return driver_fact::yes;
    }
    return opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND
               ? driver_fact::no
               : driver_fact::unknown;
}

/// The driver package is installed: the control device or a port is there, or
/// the driver store lists a hardware ID of either driver.
driver_fact package_installed(bool control_device)
{
    if (control_device || !enumerate_hubs().empty())
        return driver_fact::yes;

    const driver_fact user_mode =
        store_lists(L"SYSTEM\\DriverDatabase\\DeviceIds\\VSSCOMUM\\PORT");
    const driver_fact kernel_mode =
        store_lists(L"SYSTEM\\DriverDatabase\\DeviceIds\\VSSCOM\\PORT");
    if (user_mode == driver_fact::yes || kernel_mode == driver_fact::yes)
        return driver_fact::yes;
    if (user_mode == driver_fact::no && kernel_mode == driver_fact::no)
        return driver_fact::no;
    return driver_fact::unknown;
}

/// Asks the service control manager about the helper. Any user may ask.
helper_service_state helper_service(DWORD& code)
{
    const SC_HANDLE manager =
        ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr)
    {
        code = ::GetLastError();
        return helper_service_state::unknown;
    }
    const SC_HANDLE service =
        ::OpenServiceW(manager, VssComHelperServiceName, SERVICE_QUERY_STATUS);
    if (service == nullptr)
    {
        const DWORD error = ::GetLastError();
        ::CloseServiceHandle(manager);
        if (error == ERROR_SERVICE_DOES_NOT_EXIST)
            return helper_service_state::not_installed;
        code = error;
        return helper_service_state::unknown;
    }

    SERVICE_STATUS status{};
    const bool asked = ::QueryServiceStatus(service, &status) != FALSE;
    if (!asked)
        code = ::GetLastError();
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    if (!asked)
        return helper_service_state::unknown;
    return status.dwCurrentState == SERVICE_RUNNING
               ? helper_service_state::running
               : helper_service_state::stopped;
}

/// Sends the helper a request that changes nothing: a COM number of 0 is outside
/// the range, so a helper that knows COM numbers answers ERROR_INVALID_PARAMETER
/// before it looks at anything, and one from before them answers
/// ERROR_NOT_SUPPORTED. Either way the helper has identified the caller, which
/// is the other thing worth knowing.
helper_probe probe_helper(DWORD& code)
{
    DWORD error = 0;
    unique_handle pipe(open_helper(error));
    if (pipe.get() == INVALID_HANDLE_VALUE)
    {
        code = error;
        if (error == ERROR_FILE_NOT_FOUND)
            return helper_probe::no_pipe;
        return error == ERROR_ACCESS_DENIED ? helper_probe::access_denied
                                            : helper_probe::failed;
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!::SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
    {
        code = ::GetLastError();
        return helper_probe::failed;
    }

    VSSCOM_HELPER_REQUEST request{};
    request.Version = VSSCOM_HELPER_VERSION;
    request.Op = VSSCOM_HELPER_OP_SET_COM_NUMBER;
    VSSCOM_HELPER_REPLY reply{};
    DWORD returned = 0;
    if (!::TransactNamedPipe(pipe.get(), &request, sizeof(request), &reply,
                             sizeof(reply), &returned, nullptr) ||
        returned != sizeof(reply))
    {
        code = ::GetLastError();
        return helper_probe::failed;
    }
    if (reply.Version != VSSCOM_HELPER_VERSION)
    {
        code = ERROR_NOT_SUPPORTED;
        return helper_probe::incompatible;
    }
    code = reply.Status == ERROR_INVALID_PARAMETER ? 0 : reply.Status;
    switch (reply.Status)
    {
    case ERROR_INVALID_PARAMETER:
    // The helper is working and this user is at their limit: not a set-up problem.
    case ERROR_NOT_ENOUGH_QUOTA:
        return helper_probe::ok;
    case ERROR_ACCESS_DENIED:
        return helper_probe::access_denied;
    case ERROR_NOT_SUPPORTED:
        return helper_probe::incompatible;
    default:
        break;
    }
    return helper_probe::failed;
}

} // namespace

bool platform_driver_available()
{
    VSS_PROFILE_ZONE("ports.vcom.platform_available");
    return !find_control_path().empty() || helper_present() ||
           !enumerate_hubs().empty();
}

std::string platform_driver_version()
{
    VSS_PROFILE_ZONE("ports.vcom.platform_version");
    // Both devices ship together; the control device speaks version 1 of its
    // own interface alongside version 3 of the ports.
    const std::wstring control_path = find_control_path();
    if (!control_path.empty())
    {
        DWORD error = 0;
        unique_handle control(open_control(control_path, error));
        VSSCOM_CTL_VERSION_INFO info{};
        if (control.get() != INVALID_HANDLE_VALUE &&
            control_request(control.get(),
                            static_cast<DWORD>(IOCTL_VSSCOM_CTL_GET_VERSION),
                            nullptr, 0, &info, sizeof(info), error) &&
            info.Version == VSSCOM_CTL_VERSION)
            return std::to_string(VSSCOM_VERSION);
    }

    for (const auto& hub : enumerate_hubs())
    {
        DWORD error = 0;
        const auto handle = open_hub(hub.path, error);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        VSSCOM_VERSION_INFO version{};
        const bool ok = read_version(handle, version);
        ::CloseHandle(handle);
        if (ok)
            return std::to_string(version.Version);
    }
    return {};
}

result<virtual_port_result>
platform_create_driver_port(const transport::executor_type& executor,
                            const std::string& name, unsigned com_number)
{
    VSS_PROFILE_ZONE("ports.vcom.platform_create_port");
    // A number is the helper service's to give. Without it, say so before a
    // port is made that would only be thrown away.
    if (com_number != 0 && !helper_present())
    {
        // A port that already has the number is still all right, below.
        if (find_control_path().empty() && enumerate_hubs().empty())
            return make_error_code(errc::unsupported);
    }

    // With the control device installed, make a port of our own: it is ours
    // alone and goes when the transport does. An older driver without one
    // (errc::unsupported) falls back to the ports that exist.
    const std::wstring control_path = find_control_path();
    if (!control_path.empty() && (com_number == 0 || helper_present()))
    {
        auto made =
            create_port_on_demand(executor, name, control_path, com_number);
        if (made || made.error() != make_error_code(errc::unsupported))
            return made;
    }

    // The user-mode driver has no control device: its helper service makes the
    // port. No helper, or no user-mode driver behind it, falls through as well.
    auto helped = create_port_via_helper(executor, name, com_number);
    if (helped || helped.error() != make_error_code(errc::unsupported))
        return helped;

    const auto hubs = enumerate_hubs();
    if (hubs.empty())
        return make_error_code(errc::unsupported);

    // A port is free when nobody has its hub end and no application has the
    // COM port open. Anything else belongs to somebody. With a number asked for,
    // only a port that already has it will do: the number of a port that already
    // existed is not ours to change.
    const std::string wanted =
        com_number != 0 ? "COM" + std::to_string(com_number) : std::string();
    bool numbered = wanted.empty();
    errc failure = errc::device_busy;
    for (const auto& hub : hubs)
    {
        if (!wanted.empty() && !same_name(to_utf8(hub.port), wanted))
            continue;
        numbered = true;

        DWORD error = 0;
        const auto handle = open_hub(hub.path, error);
        if (handle == INVALID_HANDLE_VALUE)
        {
            if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)
                failure = errc::io_error;
            continue;
        }

        VSSCOM_VERSION_INFO version{};
        VSSCOM_HUB_STATE state{};
        if (!read_version(handle, version) ||
            version.Version != VSSCOM_VERSION)
        {
            // A driver of another version: the hub speaks version 3.
            ::CloseHandle(handle);
            failure = errc::unsupported;
            continue;
        }
        if (!read_state(handle, state) || state.AppOpen != 0)
        {
            ::CloseHandle(handle);
            continue;
        }

        virtual_port_result out;
        out.path = to_utf8(hub.port);
        out.kind = virtual_port_kind::windows_driver;
        out.port = driver_transport::create(executor, handle, out.path, name);
        return out;
    }

    return make_error_code(numbered ? failure : errc::not_found);
}

result<std::string> platform_set_driver_com_number(transport& port,
                                                   unsigned com_number)
{
    VSS_PROFILE_ZONE("ports.vcom.platform_set_com_number");
    // Looked up among the transports of this process, which are all ours, so that
    // nothing depends on run-time type information.
    for (const auto& candidate : registry_snapshot())
    {
        if (static_cast<transport*>(candidate.get()) == &port)
            return candidate->renumber(com_number);
    }
    return make_error_code(errc::invalid_argument);
}

result<std::uint32_t> platform_revoke_driver_port(transport& port)
{
    VSS_PROFILE_ZONE("ports.vcom.platform_revoke");
    for (const auto& candidate : registry_snapshot())
    {
        if (static_cast<transport*>(candidate.get()) == &port)
            return candidate->revoke();
    }
    return make_error_code(errc::invalid_argument);
}

std::optional<driver_peer_info> platform_driver_peer(const std::string& com_name)
{
    VSS_PROFILE_ZONE("ports.vcom.platform_peer");
    for (const auto& candidate : registry_snapshot())
    {
        if (candidate->named(com_name))
            return candidate->peer();
    }
    return std::nullopt;
}

std::optional<driver_setup_observation> platform_observe_driver_setup()
{
    VSS_PROFILE_ZONE("ports.vcom.observe_setup");
    driver_setup_observation seen;
    seen.control_device = !find_control_path().empty();
    seen.driver_installed = package_installed(seen.control_device);

    DWORD service_code = 0;
    seen.service = helper_service(service_code);

    DWORD pipe_code = 0;
    seen.pipe = probe_helper(pipe_code);
    // The pipe says more about a refusal than the service does.
    seen.code = pipe_code != 0 ? pipe_code : service_code;
    return seen;
}

driver_failure platform_last_driver_failure()
{
    const std::lock_guard lock(failure_mutex);
    return failure_record;
}

} // namespace vss::detail
