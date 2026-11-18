//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/debug/studio_provider.hpp>

#include <vss/ports/driver_setup.hpp>
#include <vss/ports/serial_enumeration.hpp>
#include <vss/ports/vcom_driver.hpp>

#include "json_support.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <chrono>
#include <set>

namespace vss::debug
{
namespace
{

namespace json = boost::json;

/// How many bytes of one traffic record go in a reply.
constexpr std::size_t traffic_bytes_most = 256;
constexpr std::size_t traffic_records_default = 100;
constexpr std::size_t traffic_records_most = 500;
constexpr std::size_t chain_most = 200;

std::int64_t age_ms(time_point when, time_point now)
{
    if (when == time_point{})
        return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - when)
        .count();
}

std::string hex(const byte_buffer& data, std::size_t most)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    const std::size_t count = std::min(data.size(), most);
    for (std::size_t index = 0; index < count; ++index)
    {
        out += digits[data[index] >> 4];
        out += digits[data[index] & 0x0f];
    }
    return out;
}

json::object error_object(const error_code& code)
{
    json::object out;
    out["code"] = code.value();
    out["category"] = std::string(code.category().name());
    out["message"] = code.message();
    return out;
}

json::object settings_object(const serial_settings& settings)
{
    json::object out;
    out["baud_rate"] = settings.baud_rate;
    out["data_bits"] = settings.data_bits;
    out["parity"] = to_string(settings.parity);
    out["stop_bits"] = to_string(settings.stop_bits);
    out["flow_control"] = to_string(settings.flow_control);
    out["exclusive"] = settings.exclusive;
    return out;
}

const char* peer_text(peer_open_state state) noexcept
{
    switch (state)
    {
    case peer_open_state::unknown:
        return "unknown";
    case peer_open_state::not_open:
        return "not_open";
    case peer_open_state::open:
        return "open";
    }
    return "unknown";
}

std::optional<std::int64_t> number(const json::object& object,
                                   std::string_view key)
{
    if (const auto* found = object.if_contains(key))
    {
        if (found->is_int64())
            return found->as_int64();
        if (found->is_uint64())
            return static_cast<std::int64_t>(found->as_uint64());
    }
    return std::nullopt;
}

std::string text(const json::object& object, std::string_view key)
{
    if (const auto* found = object.if_contains(key);
        found != nullptr && found->is_string())
        return std::string(found->as_string());
    return {};
}

/// A port named by its number or by its name.
std::shared_ptr<endpoint> find_port(studio& target, const json::object& object,
                                    std::string_view key)
{
    if (const auto id = number(object, key); id && *id > 0)
        return target.engine().find_endpoint(static_cast<endpoint_id>(*id));
    const std::string name = text(object, key);
    if (name.empty())
        return nullptr;
    return target.engine().find_endpoint(name);
}

std::optional<parity_mode> parity_from(std::string_view name)
{
    for (const parity_mode value :
         {parity_mode::none, parity_mode::odd, parity_mode::even,
          parity_mode::mark, parity_mode::space})
    {
        if (name == to_string(value))
            return value;
    }
    return std::nullopt;
}

} // namespace

studio_provider::studio_provider(studio& target, trace::recorder* source)
    : studio_(&target),
      recorder_(source != nullptr ? source : &trace::recorder::global())
{
}

std::string studio_provider::snapshot_json() const
{
    const auto now = studio_->engine().now();
    const auto infos = studio_->endpoint_infos();
    const auto peers = studio_->peer_states();
    const auto drivers = studio_->driver_ports();
    const auto lost = studio_->lost_ports();
    const std::set<endpoint_id> lost_ids(lost.begin(), lost.end());

    json::array endpoints;
    for (const endpoint_info& info : infos)
    {
        json::object item;
        item["id"] = info.id;
        item["name"] = info.name;
        item["kind"] = to_string(info.kind);
        item["state"] = to_string(info.state);
        item["open"] = info.open;
        if (!info.path.empty())
            item["path"] = info.path;
        const std::string advertised = studio_->advertised_path(info.id);
        if (!advertised.empty() && advertised != info.path)
            item["advertised"] = advertised;
        item["settings"] = settings_object(
            studio_->port_settings(info.id).value_or(info.serial));
        item["supports_control_lines"] = info.supports_control_lines;
        if (info.peer_format)
            item["peer_format"] = to_label(*info.peer_format);
        if (info.clients)
            item["clients"] = *info.clients;

        json::object counters;
        counters["bytes_rx"] = info.stats.bytes_rx;
        counters["bytes_tx"] = info.stats.bytes_tx;
        counters["read_ops"] = info.stats.read_ops;
        counters["write_ops"] = info.stats.write_ops;
        counters["errors"] = info.stats.errors;
        counters["dropped"] = info.stats.dropped;
        counters["breaks_rx"] = info.stats.breaks_rx;
        counters["breaks_tx"] = info.stats.breaks_tx;
        counters["last_activity_ms_ago"] =
            age_ms(info.stats.last_activity, now);
        item["counters"] = std::move(counters);

        if (const auto stopped = studio_->port_stopped(info.id))
            item["run_state"] =
                *stopped ? "stopped"
                         : (studio_->port_paused(info.id).value_or(false)
                                ? "paused"
                                : "running");
        item["lost"] = lost_ids.contains(info.id);
        if (const auto peer = peers.find(info.id); peer != peers.end())
            item["peer"] = peer_text(peer->second);
        if (const auto driver = drivers.find(info.id); driver != drivers.end())
            item["driver_port"] = driver->second;
        if (const auto connection = studio_->connection(info.id))
        {
            json::object network;
            network["uri"] = connection->uri;
            network["retrying"] = connection->retrying;
            network["attempts"] = connection->attempts;
            network["last_error"] = connection->last_error;
            network["last_error_code"] = connection->last_error_code;
            network["next_try_in_ms"] = connection->next_try_in.count();
            item["connection"] = std::move(network);
        }
        if (const auto found = studio_->engine().find_endpoint(info.id))
        {
            const auto queue = found->write_queue();
            json::object waiting;
            waiting["pending_writes"] = queue.pending;
            waiting["pending_bytes"] = queue.bytes;
            waiting["writing"] = queue.writing;
            item["write_queue"] = std::move(waiting);
        }
        if (const auto failure = recorder_->last_failure(info.id))
            item["last_failure"] = entry_object(*failure);
        endpoints.push_back(std::move(item));
    }

    json::array links;
    for (const link_info& info : studio_->link_infos())
    {
        json::object item;
        item["id"] = info.id;
        item["mode"] = to_string(info.mode);
        json::array sources;
        for (const endpoint_id id : info.sources)
            sources.push_back(id);
        json::array targets;
        for (const endpoint_id id : info.targets)
            targets.push_back(id);
        item["sources"] = std::move(sources);
        item["targets"] = std::move(targets);
        item["owner"] = info.owner;
        json::object options;
        options["delay_ms"] = info.options.delay_ms;
        options["jitter_ms"] = info.options.jitter_ms;
        options["emulate_modem_lines"] = info.options.emulate_modem_lines;
        options["policy"] = to_string(info.options.policy);
        options["direction"] = to_string(info.options.direction);
        item["options"] = std::move(options);
        json::object counters;
        counters["bytes"] = info.stats.bytes;
        counters["messages"] = info.stats.messages;
        counters["dropped"] = info.stats.dropped;
        counters["collisions"] = info.stats.collisions;
        counters["breaks"] = info.stats.breaks;
        counters["paced_releases"] = info.stats.paced_releases;
        counters["late_releases"] = info.stats.late_releases;
        counters["max_late_us"] = info.stats.max_late_us;
        item["counters"] = std::move(counters);
        links.push_back(std::move(item));
    }

    json::object hub;
    hub["running"] = studio_->engine().running();
    hub["endpoints"] = infos.size();
    hub["dispatch_queue_depth"] = studio_->engine().dispatch_queue_depth();

    json::object recorder;
    recorder["last_seq"] = recorder_->last_seq();
    recorder["overwritten"] = recorder_->overwritten();

    json::object out;
    out["hub"] = std::move(hub);
    out["endpoints"] = std::move(endpoints);
    out["links"] = std::move(links);
    out["recorder"] = std::move(recorder);
    out["driver_last_failure"] = [&]
    {
        const driver_failure failure = last_driver_failure();
        json::object item;
        item["step"] = failure.step;
        item["code"] = failure.code;
        return item;
    }();
    return json::serialize(out);
}

op_result studio_provider::snapshot()
{
    return op_result::done(snapshot_json());
}

op_result studio_provider::com_numbers()
{
    const auto evidence = read_com_number_evidence();
    if (!evidence)
        return op_result::fail(
            "unsupported", "This computer has no COM name database to read.");

    const auto holders =
        com_number_holders().value_or(std::vector<com_number_holder>{});
    const auto drivers = studio_->driver_ports();
    const auto reserved = studio_->reserved_com_numbers();

    std::set<unsigned> numbers;
    numbers.insert(evidence->database.begin(), evidence->database.end());
    numbers.insert(evidence->resolving.begin(), evidence->resolving.end());
    for (const auto& entry : evidence->entries)
        numbers.insert(entry.number);

    json::array rows;
    for (const unsigned number : numbers)
    {
        json::object row;
        row["number"] = number;
        row["name"] = "COM" + std::to_string(number);
        row["in_database"] =
            std::ranges::count(evidence->database, number) != 0;
        row["resolves"] = std::ranges::count(evidence->resolving, number) != 0;
        json::array nodes;
        for (const auto& entry : evidence->entries)
        {
            if (entry.number != number)
                continue;
            json::object node;
            node["instance_id"] = entry.instance_id;
            node["description"] = entry.description;
            node["present"] = entry.present;
            node["studio_port"] = is_studio_port_instance_id(entry.instance_id);
            nodes.push_back(std::move(node));
        }
        row["device_nodes"] = std::move(nodes);
        for (const auto& holder : holders)
        {
            if (holder.number != number)
                continue;
            static constexpr const char* kinds[] = {
                "present_device", "studio_port_gone", "other_port_gone",
                "name_only", "database_only"};
            row["verdict"] = kinds[static_cast<int>(holder.kind)];
            row["reusable_by_studio"] = holder.reusable_by_studio();
        }
        const std::string name = "COM" + std::to_string(number);
        for (const auto& driver : drivers)
        {
            if (driver.second == name)
                row["this_studio_port"] = driver.first;
        }
        rows.push_back(std::move(row));
    }

    json::object out;
    out["numbers"] = std::move(rows);
    json::array keeps;
    if (reserved)
    {
        for (const unsigned number : *reserved)
            keeps.push_back(number);
        out["reserved_by_studio_rule"] = std::move(keeps);
    }
    return op_result::done(json::serialize(out));
}

op_result studio_provider::driver()
{
    json::object out;
    const driver_failure failure = last_driver_failure();
    json::object last;
    last["step"] = failure.step;
    last["code"] = failure.code;
    out["last_failure"] = std::move(last);

    json::array ports;
    for (const auto& item : studio_->driver_ports())
    {
        json::object port;
        port["endpoint"] = item.first;
        port["com_name"] = item.second;
        if (const auto holder = studio_->driver_holder(item.first))
        {
            json::object peer;
            peer["open"] = holder->open;
            peer["process_id"] = holder->process_id;
            peer["baud_rate"] = holder->baud_rate;
            peer["data_bits"] = holder->data_bits;
            peer["parity"] = holder->parity;
            peer["stop_bits"] = holder->stop_bits;
            port["peer"] = std::move(peer);
        }
        ports.push_back(std::move(port));
    }
    out["ports"] = std::move(ports);

    json::array lost;
    for (const endpoint_id id : studio_->lost_ports())
        lost.push_back(id);
    out["lost_ports"] = std::move(lost);

    // What the recorder knows about the driver and the helper.
    trace::filter what;
    what.failures_only = true;
    what.limit = 50;
    json::array failures;
    for (const auto& item : recorder_->read(what))
    {
        const std::string_view call = item.call_text();
        if (call.find("helper") != std::string_view::npos ||
            call.find("control device") != std::string_view::npos ||
            call.find("hub end") != std::string_view::npos ||
            call.find("COM number") != std::string_view::npos)
            failures.push_back(entry_object(item));
    }
    out["recent_failures"] = std::move(failures);
    return op_result::done(json::serialize(out));
}

op_result studio_provider::traffic(const std::string& args_json)
{
    boost::system::error_code ec;
    const json::value given = json::parse(args_json, ec);
    std::set<endpoint_id> wanted;
    std::size_t limit = traffic_records_default;
    if (!ec && given.is_object())
    {
        const auto& object = given.as_object();
        if (const auto* list = object.if_contains("endpoints");
            list != nullptr && list->is_array())
        {
            for (const auto& item : list->as_array())
                if (item.is_number())
                    wanted.insert(item.to_number<endpoint_id>());
        }
        if (const auto count = number(object, "limit"); count && *count > 0)
            limit = std::min<std::size_t>(static_cast<std::size_t>(*count),
                                          traffic_records_most);
    }
    json::array records;
    const auto now = studio_->engine().now();
    for (const auto& item : studio_->engine().traffic(0))
    {
        if (!wanted.empty() && !wanted.contains(item.endpoint))
            continue;
        json::object row;
        row["endpoint"] = item.endpoint;
        row["name"] = item.name;
        row["direction"] = to_string(item.direction);
        row["ms_ago"] = age_ms(item.when, now);
        if (item.data)
        {
            row["size"] = item.data->size();
            row["hex"] = hex(*item.data, traffic_bytes_most);
        }
        records.push_back(std::move(row));
    }
    // The newest, in time order.
    while (records.size() > limit)
        records.erase(records.begin());
    json::object out;
    out["records"] = std::move(records);
    out["bytes_per_record_most"] = traffic_bytes_most;
    return op_result::done(json::serialize(out));
}

op_result studio_provider::run(const std::string& args_json)
{
    boost::system::error_code ec;
    const json::value given = json::parse(args_json, ec);
    if (ec || !given.is_object())
        return op_result::fail("invalid_argument",
                               "The arguments are not an object.");
    const auto& object = given.as_object();
    const std::string operation = text(object, "op");

    const std::uint64_t before = recorder_->last_seq();
    const auto started = std::chrono::steady_clock::now();
    error_code failure;
    json::object extra;

    const auto need_port = [&]() -> std::shared_ptr<endpoint>
    {
        auto port = find_port(*studio_, object, "port");
        if (!port)
            failure = make_error_code(errc::not_found);
        return port;
    };

    if (operation == "add_monitor")
    {
        const std::string from = text(object, "from");
        const std::string to = text(object, "to");
        if (from.empty() || to.empty())
            return op_result::fail("invalid_argument",
                                   "add_monitor needs \"from\" and \"to\".");
        if (const auto made = studio_->monitor(from, to); !made)
            failure = made.error();
        else
            extra["link"] = *made;
    }
    else if (operation == "restart_port" || operation == "reopen" ||
             operation == "stop_port" || operation == "start_port")
    {
        if (const auto port = need_port())
        {
            const endpoint_id id = port->id();
            const json::value* settings = object.if_contains("settings");
            if (operation == "restart_port" && settings != nullptr &&
                settings->is_object())
            {
                serial_settings next =
                    studio_->port_settings(id).value_or(port->info().serial);
                const auto& given_settings = settings->as_object();
                if (const auto baud = number(given_settings, "baud_rate"))
                    next.baud_rate = static_cast<unsigned>(*baud);
                if (const auto bits = number(given_settings, "data_bits"))
                    next.data_bits = static_cast<unsigned>(*bits);
                if (const auto parity =
                        parity_from(text(given_settings, "parity")))
                    next.parity = *parity;
                if (const auto done = studio_->reconfigure_port(id, next);
                    !done)
                    failure = done.error();
            }
            else
            {
                const auto done =
                    operation == "stop_port"    ? studio_->stop_port(id)
                    : operation == "start_port" ? studio_->start_port(id)
                                                : studio_->reopen(id);
                if (!done)
                    failure = done.error();
            }
        }
    }
    else if (operation == "set_com_number")
    {
        const auto wanted = number(object, "number");
        if (!wanted || *wanted < 1 || *wanted > 255)
            return op_result::fail("invalid_argument",
                                   "set_com_number needs \"number\" 1 to 255.");
        if (const auto port = need_port())
        {
            if (const auto done = studio_->set_com_number(
                    port->id(), static_cast<unsigned>(*wanted));
                !done)
                failure = done.error();
            else
                extra["com_name"] = *done;
        }
    }
    else if (operation == "check_listen_free")
    {
        const std::string uri = text(object, "uri");
        if (uri.empty())
            return op_result::fail("invalid_argument",
                                   "check_listen_free needs \"uri\".");
        if (const auto done = studio_->check_listen_free(uri); !done)
            failure = done.error();
    }
    else
    {
        return op_result::fail(
            "invalid_argument",
            "The operations are add_monitor, restart_port, reopen, "
            "stop_port, start_port, set_com_number and check_listen_free.");
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;

    trace::filter what;
    what.after_seq = before;
    what.limit = chain_most;
    json::array chain;
    for (const auto& item : recorder_->read(what))
        chain.push_back(entry_object(item));

    json::object out;
    out["op"] = operation;
    out["ok"] = !failure;
    if (failure)
        out["error"] = error_object(failure);
    out["duration_ms"] =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    out["result"] = std::move(extra);
    // Every call recorded while the operation ran, which on a busy studio
    // can include another thread's.
    out["calls"] = std::move(chain);
    return op_result::done(json::serialize(out));
}

} // namespace vss::debug
