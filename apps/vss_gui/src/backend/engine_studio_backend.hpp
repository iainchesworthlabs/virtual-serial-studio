//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef VSS_GUI_BACKEND_ENGINE_STUDIO_BACKEND_HPP
#define VSS_GUI_BACKEND_ENGINE_STUDIO_BACKEND_HPP

#include "backend/studio_backend.hpp"

#include <memory>
#include <vector>

class QTimer;

namespace vss
{
class studio;
struct endpoint_info;
struct link_info;
} // namespace vss

namespace vss::gui
{

class EventBridge;

/// The real adapter over `vss::studio`.
///
/// The studio must outlive the backend and the backend must outlive the
/// engine's event deliveries, which the application's construction order
/// already guarantees: the studio is created before the main window.
class EngineStudioBackend final : public StudioBackend
{
public:
    explicit EngineStudioBackend(vss::studio& studio,
                                 QObject* parent = nullptr);
    ~EngineStudioBackend() override;

    capabilities caps() const override;

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
    vss::result<void>
    set_port_settings(vss::endpoint_id id,
                      const vss::serial_settings& settings) override;
    vss::result<void> rename_port(vss::endpoint_id id,
                                  const QString& name) override;
    std::vector<vss::endpoint_id> lost_ports() const override;
    vss::result<std::size_t> recreate_lost_ports() override;

    std::vector<port_view> ports() const override { return ports_; }
    std::vector<link_view> links() const override { return links_; }
    std::vector<device_view> devices() const override;
    std::vector<traffic_view> traffic(int limit) const override;
    setup_facts facts() const override;
    driver_check check_driver() const override;
    std::chrono::milliseconds probe_cache_age() const override
    {
        return std::chrono::seconds(5);
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
    vss::result<vss::link_id>
    bus(const std::vector<vss::endpoint_id>& members) override;
    vss::result<void> disconnect(vss::link_id id) override;
    vss::result<void> set_owner(vss::link_id id,
                                vss::endpoint_id owner) override;
    vss::result<void> release_owner(vss::link_id id) override;

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
    std::optional<line_wiring> link_wiring(vss::link_id id) const override;
    vss::result<void> set_link_wiring(vss::link_id id,
                                      const line_wiring& wiring) override;
    vss::result<void> inject_fault(vss::endpoint_id id,
                                   vss::fault_kind fault) override;
    vss::result<void> set_echo(vss::endpoint_id id, bool on) override;
    bool echo(vss::endpoint_id id) const override;

    vss::config::document snapshot() const override;
    vss::result<void> apply(const vss::config::document& document,
                            bool rollback_on_error) override;

    vss::result<share_view> share_serial(const QString& uri,
                                         int listeners) override;
    vss::result<vss::link_id> bridge(const QString& from_uri,
                                     const QString& to_uri) override;
    vss::result<void> create_com0com_pair() override;

    bool hub_running() const override;
    void refresh() override;
    void set_refresh_interval(int milliseconds) override;

private:
    /// Makes a virtual port, asking the Windows driver for a COM number that
    /// is not a legacy port (see `com_numbers.hpp`) where it has one to give.
    vss::result<std::shared_ptr<vss::endpoint>>
    create_port_with_com_number(const QString& name,
                                vss::virtual_port_kind kind,
                                const vss::endpoint_options& options);

    void rebuild();
    /// Looks at the engine soon, once for however many batches arrive until
    /// then.  The slices of a flood would otherwise each pay for a look.
    void schedule_rebuild();

    vss::studio& studio_;
    std::unique_ptr<EventBridge> bridge_;
    /// Looks for a program that opened a pseudo terminal; null where the
    /// platform cannot tell.
    QTimer* peer_timer_ = nullptr;
    bool rebuild_scheduled_ = false;
    std::vector<port_view> ports_;
    std::vector<link_view> links_;
};

} // namespace vss::gui

#endif // VSS_GUI_BACKEND_ENGINE_STUDIO_BACKEND_HPP
