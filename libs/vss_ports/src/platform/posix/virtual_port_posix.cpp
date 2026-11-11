//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>

#include "../virtual_port_platform.hpp"
#include "pty_slave_path.hpp"

#include <vss/ports/native_lines.hpp>

#include <boost/asio/posix/stream_descriptor.hpp>

#include <atomic>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdlib.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace vss::detail
{
namespace
{

/// Marks a descriptor so that a program started with `exec` does not get it.
bool close_on_exec(int descriptor) noexcept
{
    const int flags = ::fcntl(descriptor, F_GETFD);
    return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

/// The baud rate a `speed_t` stands for, or 0 for one this table lacks.  Only
/// the rates Linux and macOS both name are here; a faster rate is reported as
/// unknown rather than guessed.  (macOS uses the rate itself as the speed, so
/// the cases are distinct constants on both.)
unsigned baud_from_speed(speed_t speed) noexcept
{
    struct entry
    {
        speed_t speed;
        unsigned baud;
    };
    static const entry table[] = {
        {B50, 50},       {B75, 75},       {B110, 110},     {B134, 134},
        {B150, 150},     {B200, 200},     {B300, 300},     {B600, 600},
        {B1200, 1200},   {B1800, 1800},   {B2400, 2400},   {B4800, 4800},
        {B9600, 9600},   {B19200, 19200}, {B38400, 38400}, {B57600, 57600},
        {B115200, 115200}, {B230400, 230400}};
    for (const auto& item : table)
    {
        if (item.speed == speed)
            return item.baud;
    }
    return 0;
}

/// The baud rate and framing in `settings`, or empty when its rate is not one
/// of those above.  `framing_kept` says whether this system lets a program
/// change the framing of a pseudo terminal (see #pty_keeps_framing); when it
/// does not, only the baud rate is reported.  Sticky (mark and space) parity
/// is a Linux-only flag that is not read here, so it shows as odd or even.
std::optional<line_format> format_from_termios(const struct termios& settings,
                                               bool framing_kept)
{
    line_format out;
    out.baud_rate = baud_from_speed(::cfgetospeed(&settings));
    if (out.baud_rate == 0)
        return std::nullopt;
    out.framing_known = framing_kept;
    if (!framing_kept)
        return out;

    switch (settings.c_cflag & CSIZE)
    {
    case CS5: out.data_bits = 5; break;
    case CS6: out.data_bits = 6; break;
    case CS7: out.data_bits = 7; break;
    default: out.data_bits = 8; break;
    }

    if ((settings.c_cflag & PARENB) == 0)
        out.parity = parity_mode::none;
    else
        out.parity = (settings.c_cflag & PARODD) != 0 ? parity_mode::odd
                                                      : parity_mode::even;

    out.stop_bits = (settings.c_cflag & CSTOPB) != 0 ? stop_bits_mode::two
                                                     : stop_bits_mode::one;
    return out;
}

/// Whether a program's choice of data bits and parity survives on this
/// system's pseudo terminals.  Linux forces the slave to 8 data bits and no
/// parity whatever a program asks for; macOS keeps what is set.  Asked of the
/// system itself, on the slave of a port nobody has opened yet, rather than
/// assumed from its name; the slave is put back as it was.
bool pty_keeps_framing(int slave)
{
    struct termios original{};
    if (slave < 0 || ::tcgetattr(slave, &original) != 0)
        return false;

    struct termios probe = original;
    probe.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARODD);
    probe.c_cflag |= CS7 | PARENB;
    bool kept = false;
    if (::tcsetattr(slave, TCSANOW, &probe) == 0)
    {
        struct termios now{};
        if (::tcgetattr(slave, &now) == 0)
            kept = (now.c_cflag & CSIZE) == CS7 && (now.c_cflag & PARENB) != 0;
    }
    ::tcsetattr(slave, TCSANOW, &original);
    return kept;
}

/// A pseudo terminal pair.  The master is ours; the slave path is handed to
/// the rest of the world.
class pty_transport final : public transport
{
public:
    pty_transport(const executor_type& executor, int master, int slave,
                  std::string path, std::string name, bool framing_kept)
        : executor_(executor), descriptor_(executor, master), slave_fd_(slave),
          path_(std::move(path)), name_(std::move(name)),
          framing_kept_(framing_kept)
    {
        boost::system::error_code ignored;
        descriptor_.non_blocking(true, ignored);
        descriptor_.native_non_blocking(true, ignored);
        lines_.set(control_line::dtr, true);
        lines_.set(control_line::rts, true);
        lines_.set(control_line::cts, true);
        lines_.set(control_line::dsr, true);
        lines_.set(control_line::dcd, true);
        refresh_lines();
        // What the slave looks like before any program has touched it.
        initial_format_ = read_format();
    }

    ~pty_transport() override { close(); }

    std::string describe() const override
    {
        return "pseudo terminal " + path_ + " (" + name_ + ")";
    }

    std::string public_path() const override { return path_; }

    bool is_open() const override { return open_.load(); }

    void async_read_some(boost::asio::mutable_buffer buffer,
                         io_handler handler) override
    {
        descriptor_.async_read_some(buffer, std::move(handler));
    }

    void async_write_some(boost::asio::const_buffer buffer,
                          io_handler handler) override
    {
        descriptor_.async_write_some(buffer, std::move(handler));
    }

    void close() override
    {
        if (!open_.exchange(false))
            return;
        boost::system::error_code ignored;
        descriptor_.cancel(ignored);
        descriptor_.close(ignored);
        if (slave_fd_ >= 0)
        {
            ::close(slave_fd_);
            slave_fd_ = -1;
        }
    }

    void cancel() override
    {
        boost::system::error_code ignored;
        descriptor_.cancel(ignored);
    }

    bool supports_control_lines() const noexcept override { return true; }

    void set_control_line(control_line line, bool state) override
    {
        if (const auto written =
                native::write_line(descriptor_.native_handle(), line, state);
            !written)
            return;
        lines_.set(line, state);
    }

    control_lines lines() const override { return lines_; }

    void refresh_lines() override
    {
        const auto status =
            native::read_modem_status(descriptor_.native_handle());
        if (!status)
            return;
        lines_.set(control_line::cts, status->cts);
        lines_.set(control_line::dsr, status->dsr);
        lines_.set(control_line::dcd, status->dcd);
        lines_.set(control_line::ri, status->ri);
    }

    /// The baud rate and framing a program has set on the slave with
    /// `tcsetattr`, read back through the master (a pseudo terminal pair
    /// shares one set of terminal settings).  The slave is made raw at 38400
    /// 8N1 and Studio itself holds it open, so only a setting that differs
    /// from that start is a program's choice; a program that sets exactly 38400
    /// 8N1 is not told apart from none.  After the program closes the port the
    /// last setting stays, because Studio's own descriptor keeps the slave
    /// open.  Linux forces the slave to 8N1 whatever a program asks for, so
    /// there only the baud rate is reported (`framing_known` is false).
    std::optional<line_format> peer_format() const override
    {
        const auto now = read_format();
        if (!now || now == initial_format_)
            return std::nullopt;
        return now;
    }

private:
    std::optional<line_format> read_format() const
    {
        if (!open_.load())
            return std::nullopt;
        struct termios settings{};
        if (::tcgetattr(descriptor_.native_handle(), &settings) != 0)
            return std::nullopt;
        return format_from_termios(settings, framing_kept_);
    }

    executor_type executor_;
    // Mutable: asking for the native handle is not a const call in Asio.
    mutable boost::asio::posix::stream_descriptor descriptor_;
    int slave_fd_ = -1;
    std::string path_;
    std::string name_;
    control_lines lines_;
    std::atomic<bool> open_{true};
    std::optional<line_format> initial_format_;
    bool framing_kept_ = false;
};

} // namespace

result<virtual_port_result>
platform_create_pty(const transport::executor_type& executor,
                    const std::string& name)
{
    VSS_PROFILE_ZONE("ports.virtual.create_pty");
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return make_error_code(errc::io_error);

    // A program this process starts (the serial group step, a tool whose
    // output is read) must not inherit the port: it would hold it open for as
    // long as it runs, also after Studio has closed it.  macOS refuses
    // O_CLOEXEC in posix_openpt, so the flag is set afterwards on both systems.
    if (!close_on_exec(master))
    {
        ::close(master);
        return make_error_code(errc::io_error);
    }

    if (::grantpt(master) != 0 || ::unlockpt(master) != 0)
    {
        ::close(master);
        return make_error_code(errc::io_error);
    }

    std::string slave_path;
    if (!platform_pty_slave_path(master, slave_path))
    {
        ::close(master);
        return make_error_code(errc::io_error);
    }

    // Keep the slave open so the master never reports end-of-file while no
    // application has connected yet, and put it in raw mode so the line
    // discipline does not translate or echo anything.
    const int slave =
        ::open(slave_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (slave >= 0)
    {
        struct termios settings{};
        if (::tcgetattr(slave, &settings) == 0)
        {
            ::cfmakeraw(&settings);
            settings.c_cflag |= (CLOCAL | CREAD);
            ::tcsetattr(slave, TCSANOW, &settings);
        }
    }
    const bool framing_kept = pty_keeps_framing(slave);

    auto transport_port = std::make_shared<pty_transport>(
        executor, master, slave, slave_path, name, framing_kept);

    virtual_port_result out;
    out.port = std::move(transport_port);
    out.path = slave_path;
    out.kind = virtual_port_kind::pty;
    return out;
}

bool platform_supports_pty() noexcept
{
    return true;
}

} // namespace vss::detail
