//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_BACKEND_FAKE_STUDIO_BACKEND_HPP
#define VSS_GUI_BACKEND_FAKE_STUDIO_BACKEND_HPP

#include "backend/studio_backend.hpp"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace vss::gui
{

/// In-memory `StudioBackend` for dialog and model tests.
///
/// It mirrors the real backend's contract, including unique port names,
/// owner validation, event signals and the tri-state peer value.  Failures
/// can be injected per call with `fail_next`, so error paths are testable
/// without touching the engine.
class FakeStudioBackend final : public StudioBackend
{
public:
    explicit FakeStudioBackend(QObject* parent = nullptr);

    void set_capabilities(const capabilities& value) { caps_ = value; }
    void set_peer_state(vss::endpoint_id id, peer_state state);
    /// COM numbers that another device has, so a driver port cannot take them.
    void set_taken_com_numbers(QSet<unsigned> numbers)
    {
        taken_com_numbers_ = std::move(numbers);
    }
    /// COM numbers that another device has but that `reserved_com_numbers`
    /// does not report (the database could not be read in full): a driver port
    /// still cannot take them.
    void set_hidden_com_numbers(QSet<unsigned> numbers)
    {
        hidden_com_numbers_ = std::move(numbers);
    }
    /// The backend cannot say which COM numbers Windows keeps (the window
    /// then does not offer numbers).
    void set_reservations_unknown(bool unknown)
    {
        reservations_unknown_ = unknown;
    }
    /// The program that has a driver port open, as the driver would name it.
    void set_holder(vss::endpoint_id id, std::optional<holder_view> holder);
    /// The releases asked for so far, in order.
    const std::vector<std::pair<vss::endpoint_id, release_method>>&
    releases() const noexcept
    {
        return releases_;
    }
    /// Marks ports as lost, as a restart of the Windows driver would.
    void set_lost_ports(std::vector<vss::endpoint_id> ids);
    std::vector<vss::endpoint_id> lost_ports() const override;
    vss::result<std::size_t> recreate_lost_ports() override;
    void set_devices(std::vector<device_view> devices);
    void set_facts(const setup_facts& facts) { facts_ = facts; }
    /// How long the setup report may be reused (see
    /// `StudioBackend::probe_cache_age`); zero, the default, asks every time.
    void set_probe_cache_age(std::chrono::milliseconds age)
    {
        probe_cache_age_ = age;
    }
    /// Windows-style free driver pairs for `share_serial`; -1 keeps the
    /// pseudo-terminal behaviour.  Matches the share plan's free-pair count
    /// in tests that exercise the read-only warning and studio-only rows.
    void set_free_pairs(int count) { free_pairs_ = count; }
    /// Makes `share_serial` fail while creating this listener, after the
    /// source and earlier listeners exist, so the rollback path is exercised.
    void fail_share_listener(int index, vss::error_code error)
    {
        share_failure_ = {index, error};
    }
    /// Making a virtual port takes this long, like Windows installing a
    /// device.  Set it before the work starts: a worker thread reads it.
    void set_create_delay(std::chrono::milliseconds delay)
    {
        create_delay_ = delay;
    }
    /// The next fallible call returns this error and consumes it.
    void fail_next(vss::error_code error)
    {
        next_error_ = error;
        calls_before_error_ = 0;
    }
    /// The fallible call after `calls` more fallible calls returns this
    /// error and consumes it, so a test can fail the second step of an
    /// operation that makes several backend calls.
    void fail_after(int calls, vss::error_code error)
    {
        next_error_ = error;
        calls_before_error_ = calls;
    }

    /// How the next benchmark ends, for tests.  `lost` bytes never arrive and
    /// the run takes about `duration` in ten steps, so a test can cancel it.
    /// Set these before a run starts: the run reads them from a worker thread
    /// and the fake does not look at any port.
    void set_benchmark_outcome(std::uint64_t lost,
                               std::chrono::milliseconds duration)
    {
        benchmark_lost_ = lost;
        benchmark_duration_ = duration;
    }
    /// The options of the most recent benchmark, or none.
    const std::optional<vss::benchmark_options>& last_benchmark() const
    {
        return benchmark_seen_;
    }

    const std::vector<QByteArray>& sent() const noexcept { return sent_; }
    /// The port each entry of `sent()` went to, in the same order.
    const std::vector<vss::endpoint_id>& sent_to() const noexcept
    {
        return sent_to_;
    }
    /// The faults injected so far, in order.
    const std::vector<vss::fault_kind>& injected() const noexcept
    {
        return injected_;
    }
    int refresh_interval_ms() const noexcept { return refresh_interval_ms_; }

    capabilities caps() const override { return caps_; }

    vss::result<vss::endpoint_id>
    create_virtual_port(const QString& name, vss::virtual_port_kind kind,
                        bool capture) override;
    vss::result<vss::endpoint_id> create_auto_port(const QString& name,
                                                   bool capture) override;
    vss::result<vss::endpoint_id>
    create_mock_port(const QString& name, const vss::serial_settings& settings,
                     const QString& target) override;
    vss::result<vss::endpoint_id>
    attach_serial(const QString& name, const QString& uri,
                  const vss::serial_settings& settings, bool capture) override;
    vss::result<void> remove_port(vss::endpoint_id id) override;
    vss::result<vss::endpoint_id>
    create_driver_port(const QString& name, bool capture,
                       unsigned com_number) override;
    vss::result<QString> set_com_number(vss::endpoint_id id,
                                        unsigned com_number) override;
    std::optional<holder_view> port_holder(vss::endpoint_id id) const override;
    vss::result<void> release_port(vss::endpoint_id id,
                                   release_method method) override;
    vss::result<void> rename_port(vss::endpoint_id id,
                                  const QString& name) override;
    vss::result<void>
    set_port_settings(vss::endpoint_id id,
                      const vss::serial_settings& settings) override;

    std::vector<port_view> ports() const override
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        return ports_;
    }
    std::vector<link_view> links() const override
    {
        const std::lock_guard<std::recursive_mutex> lock(mutex_);
        return links_;
    }
    std::vector<device_view> devices() const override { return devices_; }
    std::optional<std::vector<unsigned>> reserved_com_numbers() const override
    {
        if (reservations_unknown_)
            return std::nullopt;
        std::vector<unsigned> numbers(taken_com_numbers_.begin(),
                                      taken_com_numbers_.end());
        return numbers;
    }
    std::vector<traffic_view> traffic(int limit) const override;
    setup_facts facts() const override { return facts_; }
    std::chrono::milliseconds probe_cache_age() const override
    {
        return probe_cache_age_;
    }
    std::optional<port_view> port(vss::endpoint_id id) const override;
    std::optional<link_view> link(vss::link_id id) const override;

    vss::result<vss::link_id>
    connect_ports(vss::endpoint_id first, vss::endpoint_id second,
                  const vss::link_options& options) override;
    vss::result<vss::link_id>
    split(vss::endpoint_id source, const std::vector<vss::endpoint_id>& targets,
          const vss::link_options& options) override;
    vss::result<vss::link_id> join(const std::vector<vss::endpoint_id>& sources,
                                   vss::endpoint_id target,
                                   const vss::link_options& options) override;
    vss::result<vss::link_id>
    redirect(vss::endpoint_id from, vss::endpoint_id to,
             const vss::link_options& options) override;
    vss::result<vss::link_id> monitor(vss::endpoint_id from,
                                      vss::endpoint_id to) override;
    vss::result<vss::link_id> bus(const std::vector<vss::endpoint_id>& members,
                                  const vss::link_options& options) override;
    vss::result<void> disconnect(vss::link_id id) override;
    vss::result<void> add_target(vss::link_id link,
                                 vss::endpoint_id target) override;
    vss::result<void> remove_target(vss::link_id link,
                                    vss::endpoint_id target) override;
    vss::result<void> add_source(vss::link_id link,
                                 vss::endpoint_id source) override;
    vss::result<void> remove_source(vss::link_id link,
                                    vss::endpoint_id source) override;
    vss::result<void> add_member(vss::link_id link,
                                 vss::endpoint_id member) override;
    vss::result<void> remove_member(vss::link_id link,
                                    vss::endpoint_id member) override;
    vss::result<void>
    set_link_options(vss::link_id link,
                     const vss::link_options& options) override;
    vss::result<void> set_owner(vss::link_id id,
                                vss::endpoint_id owner) override;
    vss::result<void> release_owner(vss::link_id id) override;

    std::optional<line_wiring> link_wiring(vss::link_id id) const override;
    vss::result<void> set_link_wiring(vss::link_id id,
                                      const line_wiring& wiring) override;

    vss::result<void> send(vss::endpoint_id id,
                           const QByteArray& data) override;
    vss::result<void> set_line(vss::endpoint_id id, vss::control_line line,
                               bool state) override;
    void refresh_lines(vss::endpoint_id id) override;

    vss::result<vss::benchmark_result>
    run_benchmark(vss::endpoint_id from, vss::endpoint_id to,
                  const vss::benchmark_options& options,
                  const vss::benchmark_progress_handler& progress,
                  const std::atomic<bool>* cancel) override;
    vss::result<void> set_echo(vss::endpoint_id id, bool on) override;
    bool echo(vss::endpoint_id id) const override;
    vss::result<void> inject_fault(vss::endpoint_id id,
                                   vss::fault_kind fault) override;

    vss::config::document snapshot() const override;
    vss::result<void> apply(const vss::config::document& document,
                            bool rollback_on_error) override;

    vss::result<share_view> share_serial(const QString& uri,
                                         int listeners) override;
    vss::result<vss::link_id> bridge(const QString& from_uri,
                                     const QString& to_uri) override;
    vss::result<void> create_com0com_pair() override;

    bool hub_running() const override { return true; }
    void refresh() override;
    void set_refresh_interval(int milliseconds) override;

private:
    QString unique_name(const QString& base) const;
    std::optional<vss::error_code> take_error();
    port_view* find_port(vss::endpoint_id id);
    link_view* find_link(vss::link_id id);
    vss::link_id add_link(link_shape shape, vss::link_mode mode,
                          const std::vector<vss::endpoint_id>& sources,
                          const std::vector<vss::endpoint_id>& targets,
                          const vss::link_options& options);

    capabilities caps_;
    std::uint64_t benchmark_lost_ = 0;
    std::chrono::milliseconds benchmark_duration_{0};
    std::optional<vss::benchmark_options> benchmark_seen_;
    std::vector<port_view> ports_;
    std::vector<vss::endpoint_id> lost_;
    std::vector<link_view> links_;
    std::vector<device_view> devices_;
    std::vector<traffic_view> traffic_;
    setup_facts facts_;
    std::chrono::milliseconds probe_cache_age_{};
    std::vector<QByteArray> sent_;
    std::vector<vss::endpoint_id> sent_to_;
    std::vector<vss::endpoint_id> echoing_;
    std::vector<vss::fault_kind> injected_;
    QHash<vss::link_id, line_wiring> wiring_;
    std::chrono::milliseconds create_delay_{0};
    /// Guards the ports and links: a job on a worker thread changes them while
    /// the GUI thread reads them.  Recursive, as the mutators call each other.
    mutable std::recursive_mutex mutex_;
    std::optional<vss::error_code> next_error_;
    int calls_before_error_ = 0;
    vss::endpoint_id next_port_ = 1;
    vss::link_id next_link_ = 1;
    int refresh_interval_ms_ = 250;
    int free_pairs_ = -1;
    int next_com_number_ = 7;
    QSet<unsigned> taken_com_numbers_;
    QSet<unsigned> hidden_com_numbers_;
    bool reservations_unknown_ = false;
    QHash<vss::endpoint_id, holder_view> holders_;
    std::vector<std::pair<vss::endpoint_id, release_method>> releases_;
    std::optional<std::pair<int, vss::error_code>> share_failure_;
};

} // namespace vss::gui

#endif // VSS_GUI_BACKEND_FAKE_STUDIO_BACKEND_HPP
