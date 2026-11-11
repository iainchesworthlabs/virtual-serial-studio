//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/profile.hpp>

#include <vss/studio/studio.hpp>

#include <vss/ports/listen_transport.hpp>
#include <vss/ports/serial_transport.hpp>
#include <vss/ports/vcom_driver.hpp>

#include "reconnector.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <filesystem>
#include <future>
#include <utility>

namespace vss
{
namespace
{

std::string label_for_uri(const std::string& uri)
{
    if (const auto kind = classify_uri(uri))
    {
        std::string base = to_string(*kind);
        const auto scheme = uri.find("://");
        if (scheme != std::string::npos)
        {
            std::string target = uri.substr(scheme + 3);
            const auto query = target.find('?');
            if (query != std::string::npos)
                target.erase(query);
            for (char& character : target)
            {
                if (character == ':' || character == '/' || character == '\\' ||
                    character == '.')
                    character = '-';
            }
            if (!target.empty())
                base += "-" + target;
        }
        return base;
    }
    return "port";
}

/// The transport a URI names: a listening endpoint for `tcp-listen://` and
/// `udp-listen://`, otherwise a serial port or a network client. `settings`
/// is null when the URI alone describes the port.
std::shared_ptr<transport>
open_transport(const transport::executor_type& executor, const std::string& uri,
               const serial_settings* settings, error_code& ec)
{
    if (is_listen_uri(uri))
    {
        const auto address = parse_listen_uri(uri);
        if (!address)
        {
            ec = address.error();
            return nullptr;
        }
        auto listening =
            std::make_shared<listen_transport>(executor, *address, ec);
        if (ec)
            return nullptr;
        return listening;
    }

    auto serial =
        settings != nullptr
            ? std::make_shared<serial_transport>(executor, uri, *settings, ec)
            : std::make_shared<serial_transport>(executor, uri, ec);
    if (ec)
        return nullptr;
    return serial;
}

/// A port name as a link name: the characters a link may hold, at most 64.
std::string link_name(const std::string& name)
{
    std::string result;
    for (const char c : name)
    {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        result +=
            (letter || digit || c == '.' || c == '_' || c == '-') ? c : '-';
    }
    if (result.size() > 64)
        result.resize(64);
    if (result.empty() || result == "." || result == "..")
        result = "port";
    return result;
}

endpoint_kind kind_for_record(const std::string& kind)
{
    const auto mapped = config::to_endpoint_kind(kind);
    return mapped ? *mapped : endpoint_kind::physical;
}

config::port_entry record_to_entry(const std::string& name,
                                   const std::string& kind,
                                   const std::string& uri,
                                   const std::string& virtual_kind,
                                   const serial_settings& serial, bool capture)
{
    config::port_entry entry;
    entry.name = name;
    entry.kind = kind;
    entry.uri = uri;
    entry.virtual_kind = virtual_kind;
    entry.serial = serial;
    entry.capture = capture;
    return entry;
}

} // namespace

studio::studio(const studio_options& options)
    : options_(options), hub_(std::make_shared<vss::hub>(options_.hub))
{
    VSS_PROFILE_ZONE("studio.create");
    if (options_.start_hub)
        hub_->start();

    // Tries at reconnecting run on a thread of their own; what they publish
    // and what they open goes through the studio, which joins that thread
    // before anything else is torn down.
    reconnector_ = std::make_shared<reconnector>(
        [this](endpoint_id id) { return reconnect_attempt(id); },
        [this](endpoint_id id, std::string text)
        {
            const auto found = hub_->find_endpoint(id);
            if (!found)
                return;
            endpoint_event event;
            event.kind = event_kind::log_message;
            event.endpoint = id;
            event.name = found->name();
            event.message = std::move(text);
            event.when = clock_type::now();
            hub_->publish(std::move(event));
        });

    // A connection that is lost shows as the endpoint stopping.  The sink holds
    // the reconnector and the hub weakly, not the studio, so a delivery that is
    // already running when the studio goes is harmless.
    reconnect_subscription_ = hub_->subscribe(
        event_filter{std::nullopt, event_kind::stopped},
        [core = std::weak_ptr<reconnector>(reconnector_),
         weak_hub = std::weak_ptr<vss::hub>(hub_)](const endpoint_event& event)
        {
            const auto retry = core.lock();
            const auto engine = weak_hub.lock();
            if (!retry || !engine)
                return;
            const auto found = engine->find_endpoint(event.endpoint);
            if (!found)
                return;
            // Events for the states on the way (opening) are not a loss.
            const auto state = found->state();
            if (state == endpoint_state::failed ||
                state == endpoint_state::closed)
                retry->lost(event.endpoint);
        });
}

studio::~studio()
{
    VSS_PROFILE_ZONE("studio.destroy");
    if (hub_ && reconnect_subscription_ != 0)
        hub_->unsubscribe(reconnect_subscription_);
    // Ends the tries first: one may be in the middle of opening a transport.
    if (reconnector_)
    {
        reconnector_->set_policy({});
        reconnector_.reset();
    }

    if (hub_)
    {

        hub_->stop();
        hub_->join();
    }

    // The ports are closed, so no application can use the names any more.
    for (const auto& [id, record] : records_)
    {
        if (record.link)
            remove_stable_link(*record.link);
    }
}

void studio::start()
{
    VSS_PROFILE_ZONE("studio.start");
    hub_->start();
}

void studio::stop()
{
    VSS_PROFILE_ZONE("studio.stop");
    hub_->stop();
    hub_->join();
}

endpoint_options studio::default_options() const
{
    endpoint_options options;
    options.capture_traffic = options_.capture;
    return options;
}

std::string studio::unique_name(std::string_view base) const
{
    std::string candidate(base);
    int suffix = 2;
    while (hub_->find_endpoint(candidate))
    {
        candidate = std::string(base) + "-" + std::to_string(suffix);
        ++suffix;
    }
    return candidate;
}

bool studio::path_in_use(const std::string& path) const
{
    if (path.empty())
        return false;
    std::lock_guard lock(record_mutex_);
    return claimed_paths_.find(path) != claimed_paths_.end();
}

void studio::claim_paths(const std::string& target,
                         const std::string& advertised)
{
    std::lock_guard lock(record_mutex_);
    if (!target.empty())
        claimed_paths_.insert(target);
    if (!advertised.empty())
        claimed_paths_.insert(advertised);
}

result<void> studio::link_pty(port_record& record)
{
    VSS_PROFILE_ZONE("studio.link_pty");
    if (!options_.stable_links ||
        record.virtual_kind != to_string(virtual_port_kind::pty))
        return {};

    std::filesystem::path directory(options_.stable_dir);
    if (options_.stable_dir.empty())
    {
        const auto resolved = resolve_link_directory(process_environment());
        if (!resolved)
            return resolved.error();
        directory = *resolved;
    }

    const auto made =
        make_stable_link(directory, link_name(record.name), record.target);
    if (!made)
        return made.error();
    record.link = *made;
    record.advertised = made->path;
    return {};
}

result<std::shared_ptr<endpoint>>
studio::register_port(port_record record, std::shared_ptr<transport> transport,
                      const endpoint_options& options)
{
    VSS_PROFILE_ZONE("studio.register_port");
    if (!transport)
        return make_error_code(errc::invalid_argument);

    const auto linked = link_pty(record);
    if (!linked)
        return linked.error();

    auto created = hub_->add_endpoint(record.name, kind_for_record(record.kind),
                                      std::move(transport), options);
    if (!created)
    {
        if (record.link)
            remove_stable_link(*record.link);
        return created.error();
    }

    record.id = (*created)->id();
    if (record.advertised.empty())
        record.advertised = (*created)->public_path();
    claim_paths(record.target, record.advertised);
    {
        std::lock_guard lock(record_mutex_);
        records_.emplace(record.id, record);
    }
    if (reconnects(record) && reconnector_)
        reconnector_->allow(record.id);
    return *created;
}

result<std::shared_ptr<endpoint>>
studio::create_virtual_port(const std::string& name, virtual_port_kind kind,
                            const endpoint_options& options, unsigned com_number)
{
    VSS_PROFILE_ZONE("studio.create_virtual_port");
    if (kind == virtual_port_kind::pty && !supports(virtual_port_kind::pty))
        return create_auto_virtual_port(name, options, com_number);

    const std::string final_name = unique_name(name);

    auto created = vss::create_virtual_port(hub_->executor(), kind, final_name,
                                            com_number);
    if (!created)
        return created.error();

    port_record record;
    record.name = final_name;
    record.kind = "virtual";
    record.virtual_kind = to_string(kind);
    record.target = created->path;
    record.advertised = created->path;
    record.capture = options.capture_traffic;
    if (kind == virtual_port_kind::windows_driver)
    {
        record.com_number = com_number;
        record.driver = created->port;
    }

    return register_port(std::move(record), std::move(created->port), options);
}

result<std::shared_ptr<endpoint>>
studio::create_auto_virtual_port(const std::string& name,
                                 const endpoint_options& options, unsigned com_number)
{
    VSS_PROFILE_ZONE("studio.create_auto_virtual_port");
    const std::string final_name = unique_name(name);

    // A COM port from our own driver is the goal state: no third-party
    // emulator, and the port behaves like hardware.  Until the driver ships
    // driver_available() is false and this block is skipped.
    if (supports(virtual_port_kind::windows_driver))
    {
        auto created = vss::create_virtual_port(
            hub_->executor(), virtual_port_kind::windows_driver, final_name,
            com_number);
        if (created)
        {
            port_record record;
            record.name = final_name;
            record.kind = "virtual";
            record.virtual_kind = "windows-driver";
            record.target = created->path;
            record.advertised = created->path;
            record.capture = options.capture_traffic;
            record.com_number = com_number;
            record.driver = created->port;

            return register_port(std::move(record), std::move(created->port),
                                 options);
        }

        // A number was asked for and cannot be had: another port, or com0com,
        // or a pseudo terminal would not be what the topology says, and the
        // number is the point. Without a number any port will do.
        if (com_number != 0)
            return created.error();
    }

    // Where com0com does not exist the installation reports no pairs and the
    // loop below does nothing.
    const auto installation = detect_com0com();
    for (const auto& pair : installation.pairs)
    {
        // A null-modem pair can be claimed by at most one endpoint: wiring
        // both ends into the same hub is a physical loop.
        if (path_in_use(pair.a) || path_in_use(pair.b))
            continue;

        const std::pair<std::string, std::string> ends[] = {{pair.a, pair.b},
                                                            {pair.b, pair.a}};
        for (const auto& [attach, advertise] : ends)
        {
            auto opened = attach_serial(
                final_name, "serial://" + attach + "?baud=115200", options);
            if (!opened)
                continue;

            claim_paths(attach, advertise);
            std::lock_guard lock(record_mutex_);
            auto it = records_.find((*opened)->id());
            if (it != records_.end())
            {
                it->second.kind = "virtual";
                it->second.virtual_kind = "auto";
                it->second.advertised = advertise;
            }
            return opened;
        }
    }

    // Beyond Windows the best virtual port is a pseudo terminal, which is a
    // real tty an application can open; loopback is only the last resort.
    auto created = create_best_virtual_port(hub_->executor(), final_name);
    if (!created)
        return created.error();

    port_record record;
    record.name = final_name;
    record.kind = "virtual";
    record.virtual_kind = to_string(created->kind);
    record.target = created->path;
    record.advertised = created->path;
    record.capture = options.capture_traffic;

    return register_port(std::move(record), std::move(created->port), options);
}

result<std::shared_ptr<endpoint>>
studio::attach_serial(const std::string& name, const std::string& uri,
                      const endpoint_options& options)
{
    VSS_PROFILE_ZONE_NAMED(zone, "studio.attach_serial");
    if (zone.active())
        zone.text(uri);
    const std::string final_name = unique_name(name);

    boost::system::error_code ec;
    auto transport = open_transport(hub_->executor(), uri, nullptr, ec);
    if (ec)
        return ec;

    port_record record;
    record.name = final_name;
    record.uri = uri;
    record.capture = options.capture_traffic;
    record.serial = options.serial;
    if (const auto kind = classify_uri(uri))
        record.kind = to_string(*kind);
    else
        record.kind = "physical";
    record.target = transport->public_path();
    record.advertised = transport->public_path();

    return register_port(std::move(record), std::move(transport), options);
}

result<std::shared_ptr<endpoint>>
studio::attach_serial(const std::string& name, const std::string& uri,
                      const serial_settings& settings, endpoint_options options)
{
    VSS_PROFILE_ZONE_NAMED(zone, "studio.attach_serial");
    if (zone.active())
        zone.text(uri);
    if (const error_code invalid = validate(settings))
        return invalid;

    options.serial = settings;
    const std::string final_name = unique_name(name);

    boost::system::error_code ec;
    auto transport = open_transport(hub_->executor(), uri, &settings, ec);
    if (ec)
        return ec;

    port_record record;
    record.name = final_name;
    record.uri = uri;
    record.capture = options.capture_traffic;
    record.serial = settings;
    record.settings_given = true;
    if (const auto kind = classify_uri(uri))
        record.kind = to_string(*kind);
    else
        record.kind = "physical";
    record.target = transport->public_path();
    record.advertised = transport->public_path();

    return register_port(std::move(record), std::move(transport), options);
}

result<std::shared_ptr<endpoint>>
studio::create_mock_port(const std::string& name,
                         const serial_settings& settings,
                         const std::string& target)
{
    VSS_PROFILE_ZONE("studio.create_mock_port");
    const std::string final_name = unique_name(name);

    boost::system::error_code ec;
    auto transport =
        serial_transport::make_mock(hub_->executor(), settings, target, ec);
    if (ec)
        return ec;
    if (!transport)
        return make_error_code(errc::internal);

    endpoint_options options = default_options();
    options.serial = settings;

    port_record record;
    record.name = final_name;
    record.kind = "mock";
    record.advertised = transport->public_path();
    record.serial = settings;
    record.capture = options.capture_traffic;
    record.fixture = transport;
    record.mock_target = target;

    return register_port(std::move(record), std::move(transport), options);
}

result<void> studio::inject_fault(endpoint_id id, fault_kind fault)
{
    VSS_PROFILE_ZONE("studio.inject_fault");
    std::shared_ptr<serial_transport> fixture;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        fixture = it->second.fixture.lock();
    }
    if (!fixture || fixture->mock() == nullptr)
        return make_error_code(errc::unsupported);

    const auto target = hub_->find_endpoint(id);
    if (!target)
        return make_error_code(errc::not_found);

    // The fixture is not thread safe, and the endpoint reads from it on its
    // strand, so the injection runs there too.  The lambda holds the
    // transport, which keeps the fixture alive until it has run.
    boost::asio::post(target->get_executor(),
                      [fixture = std::move(fixture), fault]
                      {
                          auto* mock = fixture->mock();
                          switch (fault)
                          {
                          case fault_kind::parity:
                              mock->inject_parity_error();
                              break;
                          case fault_kind::framing:
                              mock->inject_framing_error();
                              break;
                          case fault_kind::overrun:
                              mock->inject_overrun();
                              break;
                          case fault_kind::break_condition:
                              mock->inject_break();
                              break;
                          case fault_kind::hangup:
                              mock->inject_hangup();
                              break;
                          case fault_kind::disconnect:
                              mock->inject_disconnect();
                              break;
                          }
                      });
    return {};
}

result<std::shared_ptr<serial_transport>>
studio::fixture_for(endpoint_id id) const
{
    std::shared_ptr<serial_transport> fixture;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        fixture = it->second.fixture.lock();
    }
    if (!fixture || fixture->mock() == nullptr)
        return make_error_code(errc::unsupported);
    return fixture;
}

namespace
{

/// Puts bytes in a fixture's receive path.  The fixture is not thread safe and
/// the endpoint reads from it on its strand, so this runs there; the lambda
/// holds the transport, which keeps the fixture alive until it has run.
void post_inject(const std::shared_ptr<endpoint>& target,
                 std::shared_ptr<serial_transport> fixture, byte_buffer data)
{
    boost::asio::post(target->get_executor(),
                      [fixture = std::move(fixture), data = std::move(data)]
                      {
                          if (auto* mock = fixture->mock(); mock != nullptr)
                              mock->inject_read(data.data(), data.size());
                      });
}

} // namespace

result<void> studio::inject_data(endpoint_id id, bytes_view data)
{
    VSS_PROFILE_ZONE("studio.inject_data");
    const auto fixture = fixture_for(id);
    if (!fixture)
        return fixture.error();
    const auto target = hub_->find_endpoint(id);
    if (!target)
        return make_error_code(errc::not_found);
    post_inject(target, *fixture, byte_buffer(data.begin(), data.end()));
    return {};
}

result<void> studio::set_fixture_echo(endpoint_id id, bool on)
{
    VSS_PROFILE_ZONE("studio.set_fixture_echo");
    const auto fixture = fixture_for(id);
    if (!fixture)
        return fixture.error();
    const auto target = hub_->find_endpoint(id);
    if (!target)
        return make_error_code(errc::not_found);

    subscriber_id previous = 0;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        previous = it->second.echo;
        if (on == (previous != 0))
            return {};
        if (!on)
            it->second.echo = 0;
    }
    if (!on)
    {
        hub_->unsubscribe(previous);
        return {};
    }

    event_filter filter;
    filter.endpoint = id;
    filter.kind = event_kind::data;
    // What the hub writes to the fixture is a `tx` event on it.  The sink holds
    // the endpoint and the transport, never the studio.
    const auto subscription = hub_->subscribe(
        filter,
        [target, transport = *fixture](const endpoint_event& event)
        {
            if (!event.data || event.direction != data_direction::tx)
                return;
            post_inject(target, transport,
                        byte_buffer(event.data->begin(), event.data->end()));
        });
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it != records_.end())
        {
            it->second.echo = subscription;
            return {};
        }
    }
    hub_->unsubscribe(subscription);
    return make_error_code(errc::not_found);
}

bool studio::fixture_echo(endpoint_id id) const
{
    std::lock_guard lock(record_mutex_);
    const auto it = records_.find(id);
    return it != records_.end() && it->second.echo != 0;
}

std::optional<serial_settings> studio::port_settings(endpoint_id id) const
{
    std::lock_guard lock(record_mutex_);
    const auto it = records_.find(id);
    if (it == records_.end())
        return std::nullopt;
    return it->second.serial;
}

result<void> studio::reconfigure_port(endpoint_id id,
                                      const serial_settings& settings)
{
    VSS_PROFILE_ZONE("studio.reconfigure_port");
    if (const auto invalid = validate(settings))
        return invalid;

    port_record record;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        record = it->second;
    }
    const auto found = hub_->find_endpoint(id);
    if (!found)
        return make_error_code(errc::not_found);

    const auto store = [this, id, &settings](bool given)
    {
        std::lock_guard lock(record_mutex_);
        if (const auto it = records_.find(id); it != records_.end())
        {
            it->second.serial = settings;
            if (given)
                it->second.settings_given = true;
        }
    };

    if (record.kind == "mock")
    {
        boost::system::error_code ec;
        auto fresh = serial_transport::make_mock(hub_->executor(), settings,
                                                 record.mock_target, ec);
        if (ec)
            return ec;
        if (!fresh)
            return make_error_code(errc::internal);

        // The echo holds the old fixture; it is made again on the new one.
        const bool echo = record.echo != 0;
        if (echo)
            static_cast<void>(set_fixture_echo(id, false));
        const std::weak_ptr<serial_transport> fixture = fresh;
        if (const auto reopened = found->reopen(std::move(fresh)); !reopened)
        {
            if (echo)
                static_cast<void>(set_fixture_echo(id, true));
            return reopened.error();
        }
        {
            std::lock_guard lock(record_mutex_);
            if (const auto it = records_.find(id); it != records_.end())
                it->second.fixture = fixture;
        }
        store(false);
        if (echo)
            static_cast<void>(set_fixture_echo(id, true));
        return {};
    }

    if (record.uri.empty())
    {
        // A virtual port or a loopback: the program on the other side sets
        // the line itself, so only the saved values change.
        store(false);
        return {};
    }

    const std::string_view uri = record.uri;
    if (is_listen_uri(record.uri) || uri.starts_with("tcp://") ||
        uri.starts_with("udp://"))
        return make_error_code(errc::unsupported);

    boost::system::error_code ec;
    auto fresh = open_transport(hub_->executor(), record.uri, &settings, ec);
    if (ec)
        return ec;
    if (!fresh)
        return make_error_code(errc::internal);
    if (const auto reopened = found->reopen(std::move(fresh)); !reopened)
        return reopened.error();
    store(true);
    return {};
}

result<void> studio::remove_port(endpoint_id id)
{
    VSS_PROFILE_ZONE("studio.remove_port");
    std::string target;
    std::string advertised;
    subscriber_id echo = 0;
    {
        std::lock_guard lock(record_mutex_);
        auto it = records_.find(id);
        if (it != records_.end())
        {
            target = it->second.target;
            advertised = it->second.advertised;
            echo = it->second.echo;
        }
    }

    if (echo != 0)
        hub_->unsubscribe(echo);
    const auto removed = hub_->remove_endpoint(id);
    if (!removed)
        return removed.error();
    if (reconnector_)
        reconnector_->disallow(id);

    std::lock_guard lock(record_mutex_);
    if (const auto it = records_.find(id); it != records_.end())
    {
        if (const auto& link = it->second.link; link.has_value())
            remove_stable_link(*link);
    }
    records_.erase(id);
    if (!target.empty())
        claimed_paths_.erase(target);
    if (!advertised.empty() && advertised != target)
        claimed_paths_.erase(advertised);
    return {};
}

result<void> studio::remove_port(std::string_view name)
{
    VSS_PROFILE_ZONE("studio.remove_port_by_name");
    const auto endpoint = hub_->find_endpoint(name);
    if (!endpoint)
        return make_error_code(errc::not_found);
    return remove_port(endpoint->id());
}

result<void> studio::rename_port(endpoint_id id, const std::string& name)
{
    VSS_PROFILE_ZONE("studio.rename_port");
    if (name.empty())
        return make_error_code(errc::invalid_argument);

    std::optional<stable_link> old_link;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        if (it->second.name == name)
            return {};
        old_link = it->second.link;
    }

    // A pseudo terminal is advertised by a link named after the port, so
    // the link moves with the name.  Make the new one first: if the hub
    // refuses the name, the old link is untouched.
    std::optional<stable_link> new_link;
    if (old_link)
    {
        const auto made = make_stable_link(
            std::filesystem::path(old_link->path).parent_path(),
            link_name(name), old_link->target);
        if (!made)
            return made.error();
        if (made->path != old_link->path)
            new_link = *made;
    }

    const auto renamed = hub_->rename_endpoint(id, name);
    if (!renamed)
    {
        if (new_link)
            remove_stable_link(*new_link);
        return renamed.error();
    }

    std::lock_guard lock(record_mutex_);
    const auto it = records_.find(id);
    if (it == records_.end())
    {
        // The port was removed while the name changed.
        if (new_link)
            remove_stable_link(*new_link);
        return {};
    }
    it->second.name = name;
    if (new_link && old_link)
    {
        claimed_paths_.erase(it->second.advertised);
        it->second.link = *new_link;
        it->second.advertised = new_link->path;
        claimed_paths_.insert(new_link->path);
        remove_stable_link(*old_link);
    }
    return {};
}

result<std::string> studio::set_com_number(endpoint_id id, unsigned com_number)
{
    VSS_PROFILE_ZONE("studio.set_com_number");
    std::shared_ptr<transport> driver;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        driver = it->second.driver.lock();
    }
    if (!driver)
        return make_error_code(errc::unsupported);

    // The port restarts, which takes a second or two: no lock is held.
    auto renumbered = set_driver_com_number(*driver, com_number);
    if (!renumbered)
        return renumbered.error();

    std::lock_guard lock(record_mutex_);
    if (const auto it = records_.find(id); it != records_.end())
    {
        auto& record = it->second;
        claimed_paths_.erase(record.target);
        if (record.advertised != record.target)
            claimed_paths_.erase(record.advertised);
        record.target = *renumbered;
        record.advertised = *renumbered;
        record.com_number = com_number;
        claimed_paths_.insert(*renumbered);
    }
    return renumbered;
}

std::unordered_map<endpoint_id, std::string> studio::driver_ports() const
{
    VSS_PROFILE_ZONE("studio.driver_ports");
    std::unordered_map<endpoint_id, std::string> out;
    std::lock_guard lock(record_mutex_);
    for (const auto& [id, record] : records_)
    {
        if (!record.driver.expired())
            out.emplace(id, record.target);
    }
    return out;
}

std::optional<driver_peer_info> studio::driver_holder(endpoint_id id) const
{
    VSS_PROFILE_ZONE("studio.driver_holder");
    std::string com_name;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return std::nullopt;
        if (!it->second.driver.expired())
            com_name = it->second.target;
    }
    // An empty name means there is no live driver port to ask about.
    if (com_name.empty())
        return std::nullopt;
    return driver_peer(com_name);
}

result<std::uint32_t> studio::revoke_port(endpoint_id id)
{
    VSS_PROFILE_ZONE("studio.revoke_port");
    std::shared_ptr<transport> driver;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        driver = it->second.driver.lock();
    }
    if (!driver)
        return make_error_code(errc::unsupported);
    return revoke_driver_port(*driver);
}

std::unordered_map<endpoint_id, peer_open_state> studio::peer_states() const
{
    VSS_PROFILE_ZONE("studio.peer_states");
    std::vector<endpoint_id> ids;
    std::vector<std::string> paths;
    {
        std::lock_guard lock(record_mutex_);
        for (const auto& [id, record] : records_)
        {
            if (record.kind == "virtual" &&
                (record.virtual_kind == to_string(virtual_port_kind::pty) ||
                 record.virtual_kind ==
                     to_string(virtual_port_kind::windows_driver)) &&
                !record.target.empty())
            {
                ids.push_back(id);
                paths.push_back(record.target);
            }
        }
    }

    std::unordered_map<endpoint_id, peer_open_state> out;
    const std::vector<peer_open_state> states = peer_open(paths);
    for (std::size_t index = 0; index < ids.size() && index < states.size();
         ++index)
    {
        if (states[index] != peer_open_state::unknown)
            out.emplace(ids[index], states[index]);
    }
    return out;
}

bool studio::reconnects(const port_record& record)
{
    // A client that connects out.  A listener waits for others, a serial
    // device does not come back by trying again, and an in-process port has
    // no connection to lose.
    return (record.kind == "tcp" || record.kind == "rfc2217") &&
           !record.uri.empty() && !is_listen_uri(record.uri);
}

result<void> studio::reopen(endpoint_id id)
{
    VSS_PROFILE_ZONE("studio.reopen");
    port_record record;
    {
        std::lock_guard lock(record_mutex_);
        const auto it = records_.find(id);
        if (it == records_.end())
            return make_error_code(errc::not_found);
        record = it->second;
    }
    const auto found = hub_->find_endpoint(id);
    if (!found)
        return make_error_code(errc::not_found);

    if (record.kind == "virtual" &&
        record.virtual_kind == to_string(virtual_port_kind::windows_driver))
    {
        if (!supports(virtual_port_kind::windows_driver))
            return make_error_code(errc::unsupported);

        // The dead port goes first, so its slot and its COM name are free for
        // the new one.
        if (const auto old = record.driver.lock())
            old->close();
        auto created = vss::create_virtual_port(
            hub_->executor(), virtual_port_kind::windows_driver, record.name,
            record.com_number);
        if (!created)
            return created.error();
        const std::string path = created->path;
        const std::weak_ptr<transport> driver = created->port;
        if (const auto reopened = found->reopen(std::move(created->port));
            !reopened)
            return reopened.error();

        std::lock_guard lock(record_mutex_);
        if (const auto it = records_.find(id); it != records_.end())
        {
            claimed_paths_.erase(it->second.target);
            claimed_paths_.erase(it->second.advertised);
            it->second.target = path;
            it->second.advertised = path;
            it->second.driver = driver;
            claimed_paths_.insert(path);
        }
        return {};
    }

    if (record.kind == "mock" || record.uri.empty())
        return make_error_code(errc::unsupported);

    // A listener holds its address until it is closed, and the new one wants
    // the same address, so the old one goes first.
    if (is_listen_uri(record.uri))
    {
        // Closed on the endpoint's strand, and waited for, so the address is
        // free when the new one binds.
        auto closed = std::make_shared<std::promise<void>>();
        auto done = closed->get_future();
        found->shutdown([closed] { closed->set_value(); });
        done.wait();
    }

    boost::system::error_code ec;
    auto fresh =
        open_transport(hub_->executor(), record.uri,
                       record.settings_given ? &record.serial : nullptr, ec);
    if (ec)
        return ec;
    return found->reopen(std::move(fresh));
}

result<void> studio::reconnect_attempt(endpoint_id id)
{
    VSS_PROFILE_ZONE("studio.reconnect_attempt");
    if (!hub_->running())
        return make_error_code(errc::unsupported);
    const auto found = hub_->find_endpoint(id);
    if (!found)
        return make_error_code(errc::not_found);

    // Somebody opened it already, or it has not stopped after all.
    const auto state = found->state();
    if (state != endpoint_state::failed && state != endpoint_state::closed)
        return {};
    return reopen(id);
}

void studio::set_reconnect_policy(const reconnect_policy& policy)
{
    reconnector_->set_policy(policy);
}

reconnect_policy studio::current_reconnect_policy() const
{
    return reconnector_->policy();
}

std::vector<endpoint_id> studio::lost_ports() const
{
    VSS_PROFILE_ZONE("studio.lost_ports");
    std::vector<endpoint_id> candidates;
    {
        std::lock_guard lock(record_mutex_);
        for (const auto& [id, record] : records_)
        {
            if (record.kind == "virtual" &&
                record.virtual_kind ==
                    to_string(virtual_port_kind::windows_driver))
                candidates.push_back(id);
        }
    }

    std::vector<endpoint_id> out;
    for (const endpoint_id id : candidates)
    {
        const auto found = hub_->find_endpoint(id);
        if (!found)
            continue;
        const auto state = found->state();
        if (state == endpoint_state::failed || state == endpoint_state::closed)
            out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

studio::recreate_report studio::recreate_lost_ports()
{
    VSS_PROFILE_ZONE("studio.recreate_lost_ports");
    recreate_report report;
    for (const endpoint_id id : lost_ports())
    {
        const auto reopened = reopen(id);
        if (reopened)
            report.recreated.push_back(id);
        else
            report.failed.emplace_back(id, reopened.error());
    }
    return report;
}

std::vector<format_mismatch> studio::format_mismatches() const
{
    VSS_PROFILE_ZONE("studio.format_mismatches");
    return find_format_mismatches(endpoint_infos(), link_infos());
}

std::string studio::advertised_path(endpoint_id id) const
{
    std::lock_guard lock(record_mutex_);
    auto it = records_.find(id);
    if (it == records_.end())
        return {};
    return it->second.advertised;
}

std::shared_ptr<endpoint> studio::find(std::string_view name) const
{
    return hub_->find_endpoint(name);
}

std::vector<endpoint_info> studio::endpoint_infos() const
{
    VSS_PROFILE_ZONE("studio.endpoint_infos");
    std::vector<endpoint_info> out;
    for (const auto& endpoint : hub_->endpoints())
    {
        endpoint_info info = endpoint->info();
        info.path = advertised_path(info.id);
        if (info.path.empty())
            info.path = endpoint->public_path();
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<link_info> studio::link_infos() const
{
    VSS_PROFILE_ZONE("studio.link_infos");
    return hub_->links();
}

std::vector<endpoint_id>
studio::resolve_all(const std::vector<std::string>& names, error_code& ec) const
{
    VSS_PROFILE_ZONE("studio.resolve_all");
    std::vector<endpoint_id> ids;
    ids.reserve(names.size());
    for (const auto& name : names)
    {
        const auto endpoint = hub_->find_endpoint(name);
        if (!endpoint)
        {
            ec = make_error_code(errc::not_found);
            return {};
        }
        ids.push_back(endpoint->id());
    }
    return ids;
}

result<link_id> studio::connect(std::string_view a, std::string_view b)
{
    VSS_PROFILE_ZONE("studio.connect");
    const auto first = hub_->find_endpoint(a);
    const auto second = hub_->find_endpoint(b);
    if (!first || !second)
        return make_error_code(errc::not_found);
    return hub_->connect(first->id(), second->id());
}

result<link_id> studio::split(std::string_view source,
                              const std::vector<std::string>& targets)
{
    VSS_PROFILE_ZONE("studio.split");
    const auto origin = hub_->find_endpoint(source);
    if (!origin)
        return make_error_code(errc::not_found);

    error_code ec;
    const auto ids = resolve_all(targets, ec);
    if (ec)
        return ec;
    return hub_->split(origin->id(), ids);
}

result<link_id> studio::join(const std::vector<std::string>& sources,
                             std::string_view target)
{
    VSS_PROFILE_ZONE("studio.join");
    const auto destination = hub_->find_endpoint(target);
    if (!destination)
        return make_error_code(errc::not_found);

    error_code ec;
    const auto ids = resolve_all(sources, ec);
    if (ec)
        return ec;
    return hub_->join(ids, destination->id());
}

result<link_id> studio::redirect(std::string_view from, std::string_view to)
{
    VSS_PROFILE_ZONE("studio.redirect");
    const auto first = hub_->find_endpoint(from);
    const auto second = hub_->find_endpoint(to);
    if (!first || !second)
        return make_error_code(errc::not_found);
    return hub_->redirect(first->id(), second->id());
}

result<link_id> studio::monitor(std::string_view from, std::string_view to)
{
    VSS_PROFILE_ZONE("studio.monitor");
    const auto first = hub_->find_endpoint(from);
    const auto second = hub_->find_endpoint(to);
    if (!first || !second)
        return make_error_code(errc::not_found);
    return hub_->monitor(first->id(), second->id());
}

result<link_id> studio::bus(const std::vector<std::string>& members,
                            link_options options)
{
    VSS_PROFILE_ZONE("studio.bus");
    error_code ec;
    const auto ids = resolve_all(members, ec);
    if (ec)
        return ec;
    return hub_->bus(ids, options);
}

result<void> studio::disconnect(link_id id)
{
    VSS_PROFILE_ZONE("studio.disconnect");
    return hub_->disconnect(id);
}

result<void> studio::add_target(link_id id, std::string_view target)
{
    VSS_PROFILE_ZONE("studio.add_target");
    const auto port = hub_->find_endpoint(target);
    if (!port)
        return make_error_code(errc::not_found);
    return hub_->add_target(id, port->id());
}

result<void> studio::remove_target(link_id id, std::string_view target)
{
    VSS_PROFILE_ZONE("studio.remove_target");
    const auto port = hub_->find_endpoint(target);
    if (!port)
        return make_error_code(errc::not_found);
    return hub_->remove_target(id, port->id());
}

result<studio::share_result> studio::share_serial(const std::string& uri,
                                                  int listeners)
{
    VSS_PROFILE_ZONE("studio.share_serial");
    if (listeners < 1 || listeners > 64)
        return make_error_code(errc::invalid_argument);

    auto source = attach_serial(label_for_uri(uri), uri);
    if (!source)
        return source.error();

    std::vector<endpoint_id> created;
    for (int index = 0; index < listeners; ++index)
    {
        auto listener = create_auto_virtual_port(
            "listener-" + std::to_string(index + 1), default_options());
        if (!listener)
        {
            for (const endpoint_id id : created)
                static_cast<void>(remove_port(id));
            static_cast<void>(remove_port((*source)->id()));
            return listener.error();
        }
        created.push_back((*listener)->id());
    }

    const auto link = hub_->split((*source)->id(), created);
    if (!link)
    {
        for (const endpoint_id id : created)
            static_cast<void>(remove_port(id));
        static_cast<void>(remove_port((*source)->id()));
        return link.error();
    }

    share_result out;
    out.source = (*source)->id();
    out.listeners = std::move(created);
    out.link = *link;
    return out;
}

result<link_id> studio::join_ports(const std::vector<std::string>& source_uris,
                                   const std::string& target_uri)
{
    VSS_PROFILE_ZONE("studio.join_ports");
    if (source_uris.empty())
        return make_error_code(errc::invalid_argument);

    std::vector<std::string> sources;
    for (std::size_t index = 0; index < source_uris.size(); ++index)
    {
        auto attached = attach_serial("publisher-" + std::to_string(index + 1),
                                      source_uris[index]);
        if (!attached)
            return attached.error();
        sources.push_back((*attached)->name());
    }

    auto target = attach_serial("merged", target_uri);
    if (!target)
        return target.error();

    return join(sources, (*target)->name());
}

result<link_id> studio::bridge(const std::string& from_uri,
                               const std::string& to_uri)
{
    VSS_PROFILE_ZONE("studio.bridge");
    auto from = attach_serial("bridge-a", from_uri);
    if (!from)
        return from.error();
    auto to = attach_serial("bridge-b", to_uri);
    if (!to)
    {
        static_cast<void>(remove_port((*from)->id()));
        return to.error();
    }

    const auto link = hub_->connect((*from)->id(), (*to)->id());
    if (!link)
    {
        static_cast<void>(remove_port((*from)->id()));
        static_cast<void>(remove_port((*to)->id()));
        return link.error();
    }
    return *link;
}

result<void> studio::apply(const config::document& value,
                           bool rollback_on_error)
{
    VSS_PROFILE_ZONE("studio.apply");
    VSS_PROFILE_MESSAGE("studio.apply_config");
    if (const error_code invalid = config::validate(value))
        return invalid;

    // Track the ports made so far, and the id each document name got, so
    // links can refer to ports by name and a failure can undo the ports.
    std::vector<endpoint_id> created;
    std::unordered_map<std::string, endpoint_id> ids;

    const auto rollback = [&]()
    {
        if (!rollback_on_error)
            return;
        for (const endpoint_id id : created)
            static_cast<void>(remove_port(id));
    };

    // Ports come first because links need their ids.  Each port is created
    // by the call that matches its kind; the document was validated above.
    for (const auto& port : value.ports)
    {
        endpoint_options options = default_options();
        options.serial = port.serial;
        options.capture_traffic = port.capture;

        result<std::shared_ptr<endpoint>> added =
            make_error_code(errc::config_error);

        if (port.kind == "virtual")
        {
            if (port.virtual_kind == "auto")
                added = create_auto_virtual_port(port.name, options,
                                                 port.com_number);
            else if (port.virtual_kind == "loopback")
                added = create_virtual_port(
                    port.name, virtual_port_kind::loopback, options);
            else if (port.virtual_kind == "pty")
                added = create_virtual_port(port.name, virtual_port_kind::pty,
                                            options);
            else if (port.virtual_kind == "named-pipe")
                added = create_virtual_port(
                    port.name, virtual_port_kind::named_pipe, options);
            else if (port.virtual_kind == "windows-driver")
                added = create_virtual_port(port.name,
                                            virtual_port_kind::windows_driver,
                                            options, port.com_number);
        }
        else if (port.kind == "mock")
        {
            added = create_mock_port(port.name, port.serial, port.mock_target);
        }
        else
        {
            added = attach_serial(port.name, port.uri, port.serial, options);
        }

        // Stop at the first port that cannot be made, removing the earlier
        // ones when the caller asked for a rollback.
        if (!added)
        {
            rollback();
            return added.error();
        }

        created.push_back((*added)->id());
        ids.emplace(port.name, (*added)->id());
    }

    // Links: turn the names into ids.  at() cannot throw because validate()
    // checked that every name belongs to a port of the document.
    for (const auto& link : value.links)
    {
        std::vector<endpoint_id> sources;
        std::vector<endpoint_id> targets;
        std::vector<endpoint_id> members;
        sources.reserve(link.sources.size());
        targets.reserve(link.targets.size());
        members.reserve(link.members.size());
        for (const auto& name : link.sources)
            sources.push_back(ids.at(name));
        for (const auto& name : link.targets)
            targets.push_back(ids.at(name));
        for (const auto& name : link.members)
            members.push_back(ids.at(name));

        // Create the link through the hub call for its mode.  The index
        // [0] is safe because validate() requires one port on that side.
        link_options options;
        options.emulate_modem_lines = link.emulate_modem_lines;
        options.policy = link.policy;
        options.delay_ms = link.delay_ms;
        options.jitter_ms = link.jitter_ms;

        result<link_id> added = make_error_code(errc::config_error);
        if (link.mode == "bus")
            added = hub_->bus(members, options);
        else if (link.mode == "duplex")
            added = hub_->connect(sources[0], targets[0], options);
        else if (link.mode == "split")
            added = hub_->split(sources[0], targets, options);
        else if (link.mode == "join")
            added = hub_->join(sources, targets[0], options);
        else if (link.mode == "redirect")
            added = hub_->redirect(sources[0], targets[0], options);
        else if (link.mode == "monitor")
        {
            // A monitor is a one-way link without modem lines; going through
            // redirect lets it carry a delay.
            options.emulate_modem_lines = false;
            added = hub_->redirect(sources[0], targets[0], options);
        }

        if (!added)
        {
            rollback();
            return added.error();
        }
    }

    return {};
}

config::document studio::snapshot() const
{
    VSS_PROFILE_ZONE("studio.snapshot");
    config::document document;
    document.name = "studio";

    std::unordered_map<endpoint_id, std::string> names;
    {
        std::lock_guard lock(record_mutex_);
        for (const auto& entry : records_)
        {
            const auto& record = entry.second;
            auto entry_of_port = record_to_entry(
                record.name, record.kind, record.uri, record.virtual_kind,
                record.serial, record.capture);
            entry_of_port.com_number = record.com_number;
            document.ports.push_back(std::move(entry_of_port));
            names.emplace(record.id, record.name);
        }
    }

    for (const auto& link : hub_->links())
    {
        config::link_entry entry;
        switch (link.mode)
        {
        case link_mode::duplex:
            entry.mode = link.sources.size() == 1 && link.targets.size() == 1
                             ? "duplex"
                             : "split";
            break;
        case link_mode::one_way:
            // Many sources are a join whether or not it carries modem lines;
            // a tap on a bus is one without.
            if (link.sources.size() > 1)
                entry.mode = "join";
            else if (!link.options.emulate_modem_lines)
                entry.mode = "monitor";
            else
                entry.mode = "redirect";
            break;
        case link_mode::bus:
            entry.mode = "bus";
            break;
        }

        const bool bus = link.mode == link_mode::bus;
        for (const endpoint_id id : link.sources)
        {
            auto it = names.find(id);
            if (it == names.end())
                continue;
            (bus ? entry.members : entry.sources).push_back(it->second);
        }
        for (const endpoint_id id : link.targets)
        {
            auto it = names.find(id);
            if (it != names.end())
                entry.targets.push_back(it->second);
        }

        if (bus ? entry.members.size() < 2
                : entry.sources.empty() || entry.targets.empty())
            continue;

        entry.emulate_modem_lines = link.options.emulate_modem_lines;
        entry.policy = link.options.policy;
        entry.delay_ms = link.options.delay_ms;
        entry.jitter_ms = link.options.jitter_ms;
        document.links.push_back(std::move(entry));
    }

    return document;
}

com0com_installation studio::detect_com0com() const
{
    VSS_PROFILE_ZONE("studio.detect_com0com");
    return vss::detect_com0com();
}

std::vector<serial_device_info> studio::serial_devices() const
{
    VSS_PROFILE_ZONE("studio.serial_devices");
    return enumerate_serial_devices();
}

std::optional<std::vector<unsigned>> studio::reserved_com_numbers() const
{
    VSS_PROFILE_ZONE("studio.reserved_com_numbers");
    return vss::reserved_com_numbers();
}

} // namespace vss
