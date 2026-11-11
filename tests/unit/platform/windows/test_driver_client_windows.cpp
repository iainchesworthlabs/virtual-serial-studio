//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The studio's side of the vsscom driver. A hosted runner has no driver, so
// most of these cases pin the protocol the guest scripts and the driver share,
// and what the client reports when the driver is absent. One case runs the
// client against the real driver; it is skipped unless VSS_TEST_DRIVER=1, which
// the Windows guest script sets.
//

#include <vss/ports/loopback_transport.hpp>
#include <vss/ports/vcom_driver.hpp>
#include <vss/studio/studio.hpp>

#include "test_framework.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <winioctl.h>
#include <devpropdef.h>
#include <ntddser.h>

#include <vsscom/public.h>
#include <vsscom_helper/protocol.h>

using namespace vss;

TEST_CASE("driver client: the control codes are the ones the guest scripts use")
{
    // test-driver.ps1 spells these out; a change here must change it too.
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_GET_VERSION), 0x80002000ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_HUB_GET_STATE), 0x80002004ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_HUB_WAIT_STATE), 0x80002008ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_HUB_SET_LINES), 0x8000200Cul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_HUB_REVOKE), 0x80002010ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_HUB_LINE_ERROR),
               0x80002014ul);
    REQUIRE_EQ(static_cast<int>(VSSCOM_VERSION), 4);
}

TEST_CASE("driver client: a line error has the layout and the bits the driver reads")
{
    // Three 32-bit words, and the bits of SERIAL_ERROR_* in ntddser.h, which
    // the driver hands to ClearCommError as they are.
    REQUIRE_EQ(sizeof(VSSCOM_HUB_LINE_ERROR), 12u);
    REQUIRE_EQ(offsetof(VSSCOM_HUB_LINE_ERROR, Errors), 0u);
    REQUIRE_EQ(offsetof(VSSCOM_HUB_LINE_ERROR, HasByte), 4u);
    REQUIRE_EQ(offsetof(VSSCOM_HUB_LINE_ERROR, Byte), 8u);
    REQUIRE_EQ(static_cast<unsigned long>(VSSCOM_LINE_ERROR_BREAK),
               static_cast<unsigned long>(SERIAL_ERROR_BREAK));
    REQUIRE_EQ(static_cast<unsigned long>(VSSCOM_LINE_ERROR_FRAMING),
               static_cast<unsigned long>(SERIAL_ERROR_FRAMING));
    REQUIRE_EQ(static_cast<unsigned long>(VSSCOM_LINE_ERROR_OVERRUN),
               static_cast<unsigned long>(SERIAL_ERROR_OVERRUN));
    REQUIRE_EQ(static_cast<unsigned long>(VSSCOM_LINE_ERROR_QUEUE_OVERRUN),
               static_cast<unsigned long>(SERIAL_ERROR_QUEUEOVERRUN));
    REQUIRE_EQ(static_cast<unsigned long>(VSSCOM_LINE_ERROR_PARITY),
               static_cast<unsigned long>(SERIAL_ERROR_PARITY));
}

TEST_CASE("driver client: a line error for a transport that is not a driver port is refused")
{
    boost::asio::io_context io;
    auto loopback = loopback_transport::create(io.get_executor(), "x");
    const auto raised =
        raise_driver_line_error(*loopback, driver_line_error::parity);
    REQUIRE(!raised.has_value());
    REQUIRE_EQ(raised.error(), make_error_code(errc::invalid_argument));
}

TEST_CASE("driver client: the hub state has the layout the driver fills")
{
    // Twelve 32-bit words, in the order the guest script reads them.
    REQUIRE_EQ(sizeof(VSSCOM_HUB_STATE), std::size_t{48});
    REQUIRE_EQ(offsetof(VSSCOM_HUB_STATE, Generation), std::size_t{0});
    REQUIRE_EQ(offsetof(VSSCOM_HUB_STATE, AppOpen), std::size_t{4});
    REQUIRE_EQ(offsetof(VSSCOM_HUB_STATE, Dtr), std::size_t{24});
    REQUIRE_EQ(offsetof(VSSCOM_HUB_STATE, Lines), std::size_t{36});
    REQUIRE_EQ(offsetof(VSSCOM_HUB_STATE, TxQueued), std::size_t{40});
    REQUIRE_EQ(offsetof(VSSCOM_HUB_STATE, AppProcessId), std::size_t{44});
    REQUIRE_EQ(sizeof(VSSCOM_VERSION_INFO), std::size_t{8});

    // The line bits are the modem status bits Windows uses.
    REQUIRE_EQ(static_cast<unsigned>(VSSCOM_LINE_MASK), 0xF0u);
    REQUIRE_EQ(static_cast<unsigned>(VSSCOM_LINE_CTS), static_cast<unsigned>(MS_CTS_ON));
    REQUIRE_EQ(static_cast<unsigned>(VSSCOM_LINE_DSR), static_cast<unsigned>(MS_DSR_ON));
    REQUIRE_EQ(static_cast<unsigned>(VSSCOM_LINE_RI), static_cast<unsigned>(MS_RING_ON));
    REQUIRE_EQ(static_cast<unsigned>(VSSCOM_LINE_DCD), static_cast<unsigned>(MS_RLSD_ON));
}

TEST_CASE("driver client: the control device has the codes and layout the guest scripts use")
{
    // test-driver.ps1 spells these out in CtlIo; a change here must change it too.
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_CTL_GET_VERSION), 0x80002040ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_CTL_CREATE_PORT), 0x80002044ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_CTL_DESTROY_PORT), 0x80002048ul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_CTL_LIST_PORTS), 0x8000204Cul);
    REQUIRE_EQ(static_cast<unsigned long>(IOCTL_VSSCOM_CTL_GET_OWNER), 0x80002050ul);
    REQUIRE_EQ(sizeof(VSSCOM_CTL_OWNER), std::size_t{8 + VSSCOM_CTL_MAX_SID_BYTES});
    REQUIRE_EQ(offsetof(VSSCOM_CTL_OWNER, Sid), std::size_t{8});

    REQUIRE_EQ(sizeof(VSSCOM_CTL_VERSION_INFO), std::size_t{8});
    REQUIRE_EQ(sizeof(VSSCOM_CTL_CREATE), std::size_t{8});
    REQUIRE_EQ(sizeof(VSSCOM_CTL_PORT), std::size_t{4});
    REQUIRE_EQ(sizeof(VSSCOM_CTL_LIST_ENTRY), std::size_t{8});
    REQUIRE_EQ(offsetof(VSSCOM_CTL_LIST, Ports), std::size_t{4});
    REQUIRE_EQ(sizeof(VSSCOM_CTL_LIST),
               std::size_t{4 + 8 * VSSCOM_CTL_MAX_PORTS});
    REQUIRE_EQ(static_cast<int>(VSSCOM_CTL_MAX_PORTS), 32);
    REQUIRE_EQ(static_cast<int>(VSSCOM_CTL_MAX_PORTS_PER_USER), 16);
    REQUIRE_EQ(static_cast<int>(VSSCOM_CTL_VERSION), 1);
}

TEST_CASE("driver client: the instance id of a port made on demand is predictable")
{
    // The client looks for this device instance id after CREATE_PORT.
    wchar_t suffix[16]{};
    std::swprintf(suffix, 16, VSSCOM_PORT_INSTANCE_FORMAT, 10ul);
    REQUIRE(std::wstring(suffix) == L"000A");
    REQUIRE(std::wstring(VSSCOM_PORT_HARDWARE_ID) == L"VSSCOM\\PORT");
}

namespace
{

using namespace std::chrono_literals;

/// Reads once from a transport and waits for the answer on this thread.
std::size_t transport_read(transport& port, void* data, std::size_t size)
{
    using answer = std::pair<boost::system::error_code, std::size_t>;
    auto promise = std::make_shared<std::promise<answer>>();
    auto future = promise->get_future();
    port.async_read_some(boost::asio::buffer(data, size),
                         [promise](boost::system::error_code ec, std::size_t n)
                         { promise->set_value({ec, n}); });
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    const answer got = future.get();
    REQUIRE(!got.first);
    return got.second;
}

void transport_write(transport& port, const char* text)
{
    using answer = std::pair<boost::system::error_code, std::size_t>;
    auto promise = std::make_shared<std::promise<answer>>();
    auto future = promise->get_future();
    port.async_write_some(boost::asio::buffer(text, std::strlen(text)),
                          [promise](boost::system::error_code ec, std::size_t n)
                          { promise->set_value({ec, n}); });
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    const answer got = future.get();
    REQUIRE(!got.first);
    REQUIRE_EQ(got.second, std::strlen(text));
}

HANDLE open_com(const std::string& name)
{
    return ::CreateFileA(("\\\\.\\" + name).c_str(), GENERIC_READ | GENERIC_WRITE,
                         0, nullptr, OPEN_EXISTING, 0, nullptr);
}

/// Waits until the COM name cannot be opened any more.
bool wait_gone(const std::string& name)
{
    const auto until = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < until)
    {
        const auto handle = open_com(name);
        if (handle == INVALID_HANDLE_VALUE)
            return true;
        ::CloseHandle(handle);
        std::this_thread::sleep_for(250ms);
    }
    return false;
}

/// Ends the requests a program has waiting on `handle` when the case has not
/// finished with it in `limit`.  A wait for an event that never comes then
/// fails the case (`ERROR_OPERATION_ABORTED`) instead of holding it for ever.
class io_deadline
{
public:
    io_deadline(HANDLE handle, std::chrono::seconds limit)
        : watcher_([this, handle, limit] { watch(handle, limit); })
    {
    }

    io_deadline(const io_deadline&) = delete;
    io_deadline& operator=(const io_deadline&) = delete;

    ~io_deadline()
    {
        {
            const std::lock_guard lock(mutex_);
            finished_ = true;
        }
        changed_.notify_one();
        watcher_.join();
    }

private:
    void watch(HANDLE handle, std::chrono::seconds limit)
    {
        std::unique_lock lock(mutex_);
        const auto until = std::chrono::steady_clock::now() + limit;
        // After the limit every request the case starts is ended as well, so
        // the case reaches a failing check whichever call it was in.
        while (!finished_)
        {
            if (changed_.wait_until(lock, until) == std::cv_status::timeout)
            {
                ::CancelIoEx(handle, nullptr);
                changed_.wait_for(lock, 100ms);
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    bool finished_ = false;
    std::thread watcher_;
};

} // namespace

// The one case that needs the driver. It runs in a Windows guest with the
// driver installed (drivers/windows/test/test-client.ps1 sets the variable) and
// is skipped everywhere else.
TEST_CASE("driver client: against the real driver, a port is made, carries bytes and goes")
{
    char enabled[8]{};
    const DWORD length = ::GetEnvironmentVariableA("VSS_TEST_DRIVER", enabled,
                                                   sizeof(enabled));
    if (length != 1 || enabled[0] != '1')
        VSS_SKIP("needs the driver: set VSS_TEST_DRIVER=1 in a guest that has it");

    REQUIRE(driver_available());
    REQUIRE(!driver_version().empty());

    boost::asio::io_context io;
    auto guard = boost::asio::make_work_guard(io);
    std::thread runner([&io] { io.run(); });
    auto stop = [&]
    {
        guard.reset();
        io.stop();
        runner.join();
    };

    try
    {
        auto first = create_driver_port(io.get_executor(), "guest test one");
        REQUIRE(first.has_value());
        const std::string name = first->path;
        REQUIRE(name.rfind("COM", 0) == 0);
        REQUIRE(first->kind == virtual_port_kind::windows_driver);
        transport& hub = *first->port;
        REQUIRE(hub.is_open());

        // A second request makes another port, and the two do not meet.
        auto second = create_driver_port(io.get_executor(), "guest test two");
        REQUIRE(second.has_value());
        REQUIRE(second->path != name);

        const auto app = open_com(name);
        REQUIRE(app != INVALID_HANDLE_VALUE);
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 100;
        timeouts.ReadTotalTimeoutConstant = 5000;
        REQUIRE(::SetCommTimeouts(app, &timeouts) != FALSE);

        // Application to hub.
        DWORD done = 0;
        REQUIRE(::WriteFile(app, "hello hub", 9, &done, nullptr) != FALSE);
        REQUIRE_EQ(done, DWORD{9});
        char buffer[64]{};
        std::size_t got = 0;
        while (got < 9)
            got += transport_read(hub, buffer + got, sizeof(buffer) - got);
        REQUIRE(std::string(buffer, got) == "hello hub");

        // Hub to application.
        transport_write(hub, "hello app");
        char reply[64]{};
        REQUIRE(::ReadFile(app, reply, 9, &done, nullptr) != FALSE);
        REQUIRE_EQ(done, DWORD{9});
        REQUIRE(std::string(reply, done) == "hello app");

        // The lines the application drives reach the hub, and the hub's reach
        // the application.
        REQUIRE(hub.supports_control_lines());
        REQUIRE(::EscapeCommFunction(app, SETDTR) != FALSE);
        hub.refresh_lines();
        REQUIRE(hub.lines().dtr());
        REQUIRE(!hub.lines().rts());
        hub.set_control_line(control_line::cts, true);
        DWORD status = 0;
        REQUIRE(::GetCommModemStatus(app, &status) != FALSE);
        REQUIRE((status & MS_CTS_ON) != 0);

        // Closing the transport takes its port away.
        ::CloseHandle(app);
        hub.close();
        REQUIRE(wait_gone(name));

        // The other port is untouched.
        REQUIRE(second->port->is_open());
        const std::string other = second->path;
        second->port->close();
        REQUIRE(wait_gone(other));
    }
    catch (...)
    {
        stop();
        throw;
    }
    stop();
}

// Needs the driver too (test-client.ps1, test C3): a line error raised through
// the transport reaches a program as it would on hardware.
TEST_CASE("driver client: against the real driver, a line error reaches the program")
{
    char enabled[8]{};
    const DWORD length = ::GetEnvironmentVariableA("VSS_TEST_DRIVER", enabled,
                                                   sizeof(enabled));
    if (length != 1 || enabled[0] != '1')
        VSS_SKIP("needs the driver: set VSS_TEST_DRIVER=1 in a guest that has it");

    boost::asio::io_context io;
    auto guard = boost::asio::make_work_guard(io);
    std::thread runner([&io] { io.run(); });
    auto stop = [&]
    {
        guard.reset();
        io.stop();
        runner.join();
    };

    try
    {
        auto made = create_driver_port(io.get_executor(), "guest line errors");
        REQUIRE(made.has_value());
        const std::string name = made->path;
        transport& hub = *made->port;

        // With no program on the port the error is dropped, and the call still
        // succeeds.
        REQUIRE(raise_driver_line_error(hub, driver_line_error::overrun));

        const auto app = open_com(name);
        REQUIRE(app != INVALID_HANDLE_VALUE);
        COMMTIMEOUTS timeouts{};
        timeouts.ReadIntervalTimeout = 100;
        timeouts.ReadTotalTimeoutConstant = 5000;
        REQUIRE(::SetCommTimeouts(app, &timeouts) != FALSE);
        REQUIRE(::SetCommMask(app, EV_ERR | EV_BREAK) != FALSE);

        // The waits below have no timeout of their own: an event that is lost
        // must fail the case, not hold it.
        std::optional<io_deadline> deadline;
        deadline.emplace(app, 60s);

        DWORD errors = 0xFFFFFFFFul;
        REQUIRE(::ClearCommError(app, &errors, nullptr) != FALSE);
        REQUIRE_EQ(errors, DWORD{0});

        // A parity error between bytes: the call returns before the driver has
        // raised it, so the event is what says it has arrived.
        transport_write(hub, "ab");
        REQUIRE(raise_driver_line_error(hub, driver_line_error::parity,
                                        std::uint8_t{'x'}));
        transport_write(hub, "cd");
        DWORD events = 0;
        REQUIRE(::WaitCommEvent(app, &events, nullptr) != FALSE);
        REQUIRE_EQ(events, DWORD{EV_ERR});
        REQUIRE(::ClearCommError(app, &errors, nullptr) != FALSE);
        REQUIRE_EQ(errors, DWORD{CE_RXPARITY});
        REQUIRE(::ClearCommError(app, &errors, nullptr) != FALSE);
        REQUIRE_EQ(errors, DWORD{0});
        char text[16]{};
        DWORD done = 0;
        REQUIRE(::ReadFile(app, text, 5, &done, nullptr) != FALSE);
        REQUIRE(std::string(text, done) == "abxcd");

        // Each kind, as the name ClearCommError gives it.
        const std::pair<driver_line_error, DWORD> kinds[] = {
            {driver_line_error::framing, CE_FRAME},
            {driver_line_error::overrun, CE_OVERRUN},
            {driver_line_error::break_condition, CE_BREAK},
        };
        for (const auto& [kind, reported] : kinds)
        {
            REQUIRE(raise_driver_line_error(hub, kind));
            REQUIRE(::WaitCommEvent(app, &events, nullptr) != FALSE);
            const DWORD expected = kind == driver_line_error::break_condition
                                       ? DWORD{EV_BREAK}
                                       : DWORD{EV_ERR};
            REQUIRE_EQ(events, expected);
            REQUIRE(::ClearCommError(app, &errors, nullptr) != FALSE);
            REQUIRE_EQ(errors, reported);
        }

        // A full receive queue raises no event; a break after it says both
        // have arrived, because requests keep their order.
        REQUIRE(raise_driver_line_error(hub, driver_line_error::queue_overrun));
        REQUIRE(raise_driver_line_error(hub, driver_line_error::break_condition));
        REQUIRE(::WaitCommEvent(app, &events, nullptr) != FALSE);
        REQUIRE_EQ(events, DWORD{EV_BREAK});
        REQUIRE(::ClearCommError(app, &errors, nullptr) != FALSE);
        REQUIRE_EQ(errors, DWORD{CE_RXOVER | CE_BREAK});

        deadline.reset();
        ::CloseHandle(app);

        // A closed transport refuses.
        hub.close();
        REQUIRE(!raise_driver_line_error(hub, driver_line_error::parity)
                     .has_value());
        REQUIRE(wait_gone(name));
    }
    catch (...)
    {
        stop();
        throw;
    }
    stop();
}

// The studio's hang-up and disconnect on a driver port, as a program on the
// port sees them. test-client.ps1 runs it as C4.
TEST_CASE("driver client: against the real driver, a hang-up and a disconnect reach the program")
{
    char enabled[8]{};
    const DWORD length = ::GetEnvironmentVariableA("VSS_TEST_DRIVER", enabled,
                                                   sizeof(enabled));
    if (length != 1 || enabled[0] != '1')
        VSS_SKIP("needs the driver: set VSS_TEST_DRIVER=1 in a guest that has it");

    studio app;
    const auto made =
        app.create_virtual_port("guest faults", virtual_port_kind::windows_driver);
    REQUIRE(made.has_value());
    const std::shared_ptr<endpoint> port = *made;
    const endpoint_id id = port->id();
    const auto names = app.driver_ports();
    REQUIRE(names.contains(id));
    const std::string name = names.at(id);

    // With no program on the port both succeed and change nothing.
    REQUIRE(app.inject_fault(id, fault_kind::hangup));
    REQUIRE(app.inject_fault(id, fault_kind::disconnect));
    REQUIRE_EQ(port->state(), endpoint_state::open);

    const HANDLE program = open_com(name);
    REQUIRE(program != INVALID_HANDLE_VALUE);
    HANDLE next = INVALID_HANDLE_VALUE;
    {
        // The waits below have no timeout of their own.
        const io_deadline deadline(program, 60s);

        // The carrier and the ready line are up, as with a modem on line.
        REQUIRE(!port->set_line(control_line::dcd, true));
        REQUIRE(!port->set_line(control_line::dsr, true));
        REQUIRE(!port->set_line(control_line::cts, true));
        DWORD status = 0;
        REQUIRE(::GetCommModemStatus(program, &status) != FALSE);
        REQUIRE_EQ(status & (MS_RLSD_ON | MS_DSR_ON | MS_CTS_ON),
                   DWORD{MS_RLSD_ON | MS_DSR_ON | MS_CTS_ON});
        // Setting the mask drops the events of the lines going up.
        REQUIRE(::SetCommMask(program, EV_RLSD | EV_DSR | EV_CTS) != FALSE);

        // A hang-up drops the carrier and the ready line and nothing else.
        REQUIRE(app.inject_fault(id, fault_kind::hangup));
        REQUIRE(::GetCommModemStatus(program, &status) != FALSE);
        REQUIRE_EQ(status & (MS_RLSD_ON | MS_DSR_ON | MS_CTS_ON),
                   DWORD{MS_CTS_ON});
        // The two lines drop one after the other, so the program is told in
        // one event or in two.
        DWORD seen = 0;
        for (int round = 0;
             round < 2 && seen != DWORD{EV_RLSD | EV_DSR}; ++round)
        {
            DWORD events = 0;
            REQUIRE(::WaitCommEvent(program, &events, nullptr) != FALSE);
            seen |= events;
        }
        REQUIRE_EQ(seen, DWORD{EV_RLSD | EV_DSR});
        REQUIRE(!port->lines().dcd());
        REQUIRE(!port->lines().dsr());
        REQUIRE_EQ(port->state(), endpoint_state::open);

        // A disconnect: the program's handle stops working, as that of an
        // unplugged device does, and the port stays for the next program.
        REQUIRE(app.inject_fault(id, fault_kind::disconnect));
        REQUIRE(::GetCommModemStatus(program, &status) == FALSE);
        char byte = 0;
        DWORD done = 0;
        REQUIRE(::WriteFile(program, &byte, 1, &done, nullptr) == FALSE);
        REQUIRE_EQ(port->state(), endpoint_state::open);

        next = open_com(name);
        REQUIRE(next != INVALID_HANDLE_VALUE);
        REQUIRE(::GetCommModemStatus(next, &status) != FALSE);
    }
    ::CloseHandle(next);
    ::CloseHandle(program);

    REQUIRE(app.remove_port(id));
    REQUIRE(wait_gone(name));
}

TEST_CASE("driver client: without the driver it says so and takes nothing")
{
    if (driver_available())
        return; // a machine with the driver installed: covered in the guests

    boost::asio::io_context io;
    REQUIRE(driver_version().empty());
    REQUIRE_EQ(create_driver_port(io.get_executor(), "x"),
               make_error_code(errc::unsupported));
}

namespace
{

/// A COM number nothing resolves, from `first` up.
unsigned free_com_number(unsigned first)
{
    for (unsigned number = first; number <= 255; ++number)
    {
        char target[256];
        if (::QueryDosDeviceA(("COM" + std::to_string(number)).c_str(), target,
                              sizeof(target)) == 0)
            return number;
    }
    return 0;
}

bool helper_service_present()
{
    return ::WaitNamedPipeW(VssComHelperPipeName, 1) != FALSE ||
           ::GetLastError() != ERROR_FILE_NOT_FOUND;
}

} // namespace

// Needs the driver and the helper service in a guest, like the case above, and
// changes the COM numbers of ports it makes. test-client.ps1 runs it as C2.
TEST_CASE("driver client: COM numbers with the helper service")
{
    char enabled[8]{};
    const DWORD length = ::GetEnvironmentVariableA("VSS_TEST_DRIVER", enabled,
                                                   sizeof(enabled));
    if (length != 1 || enabled[0] != '1')
        VSS_SKIP("needs the driver: set VSS_TEST_DRIVER=1 in a guest that has it");
    if (!helper_service_present())
        VSS_SKIP("needs the helper service (install-driver.ps1 -Helper)");

    const unsigned first = free_com_number(100);
    const unsigned second = free_com_number(first + 1);
    REQUIRE(first != 0 && second != 0);

    boost::asio::io_context io;
    auto guard = boost::asio::make_work_guard(io);
    std::thread runner([&io] { io.run(); });
    auto stop = [&]
    {
        guard.reset();
        io.stop();
        runner.join();
    };

    try
    {
        // A port made with a number has it.
        auto made = create_driver_port(io.get_executor(), "numbered", first);
        REQUIRE(made.has_value());
        const std::string name1 = "COM" + std::to_string(first);
        const std::string name2 = "COM" + std::to_string(second);
        REQUIRE(made->path == name1);
        transport& hub = *made->port;
        REQUIRE(hub.public_path() == name1);

        // A number another device has is refused, and no port is left over.
        auto taken = create_driver_port(io.get_executor(), "taken", first);
        REQUIRE(!taken.has_value());
        REQUIRE_EQ(taken.error(), make_error_code(errc::already_exists));

        // The driver names the program that opens the port, and the transport
        // logs it with the time.
        std::mutex notes_mutex;
        std::vector<std::string> notes;
        hub.set_notice_sink(
            [&](std::string text)
            {
                const std::lock_guard lock(notes_mutex);
                notes.push_back(std::move(text));
            });
        const auto noted = [&](const std::string& what)
        {
            const auto until = std::chrono::steady_clock::now() + 10s;
            while (std::chrono::steady_clock::now() < until)
            {
                {
                    const std::lock_guard lock(notes_mutex);
                    for (const auto& note : notes)
                    {
                        if (note.find(what) != std::string::npos)
                            return true;
                    }
                }
                std::this_thread::sleep_for(50ms);
            }
            return false;
        };

        {
            const auto idle = driver_peer(name1);
            REQUIRE(idle.has_value());
            REQUIRE(!idle->open);
            REQUIRE_EQ(idle->process_id, std::uint32_t{0});
        }
        HANDLE app = open_com(name1);
        REQUIRE(app != INVALID_HANDLE_VALUE);
        REQUIRE(noted("a program opened the port"));
        {
            const auto busy = driver_peer(name1);
            REQUIRE(busy.has_value());
            REQUIRE(busy->open);
            REQUIRE_EQ(busy->process_id, static_cast<std::uint32_t>(::GetCurrentProcessId()));
        }

        // With a program on the port the number cannot change.
        REQUIRE_EQ(set_driver_com_number(hub, second),
                   make_error_code(errc::device_busy));
        ::CloseHandle(app);
        REQUIRE(noted("the program closed the port"));
        REQUIRE(!vss_test::require_value(driver_peer(name1)).open);

        // A read that is waiting survives the change.
        std::promise<std::pair<boost::system::error_code, std::size_t>> answered;
        auto future = answered.get_future();
        char buffer[16]{};
        hub.async_read_some(boost::asio::buffer(buffer, sizeof(buffer)),
                            [&](boost::system::error_code ec, std::size_t n)
                            { answered.set_value({ec, n}); });

        const auto renamed = set_driver_com_number(hub, second);
        REQUIRE(renamed.has_value());
        REQUIRE(*renamed == name2);
        REQUIRE(hub.public_path() == name2);
        REQUIRE(driver_peer(name2).has_value());
        REQUIRE(!driver_peer(name1).has_value());

        // The old name is gone, the new one opens, and the read is still ours.
        const auto old_name = open_com(name1);
        REQUIRE(old_name == INVALID_HANDLE_VALUE);
        app = open_com(name2);
        REQUIRE(app != INVALID_HANDLE_VALUE);
        DWORD done = 0;
        REQUIRE(::WriteFile(app, "after", 5, &done, nullptr) != FALSE);
        REQUIRE(future.wait_for(10s) == std::future_status::ready);
        const auto got = future.get();
        REQUIRE(!got.first);
        REQUIRE(std::string(buffer, got.second) == "after");
        ::CloseHandle(app);

        // The number it left is free for the next port.
        auto reused = create_driver_port(io.get_executor(), "reused", first);
        REQUIRE(reused.has_value());
        REQUIRE(reused->path == name1);

        // A number outside the range is refused before anything happens.
        REQUIRE_EQ(set_driver_com_number(hub, 0),
                   make_error_code(errc::invalid_argument));

        reused->port->close();
        hub.close();
        REQUIRE(wait_gone(name2));
        REQUIRE(wait_gone(name1));
    }
    catch (...)
    {
        stop();
        throw;
    }
    stop();
}
