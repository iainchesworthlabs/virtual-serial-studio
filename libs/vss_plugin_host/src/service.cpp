//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/service.hpp>

#include <vss/core/error.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
#include <vss/plugin/host/extension.hpp>
#include <vss/plugin/host/instances.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/sandbox.hpp>
#include <vss/plugin/host/start_plan.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vss::plugin::host
{

namespace
{

/// How long `shut_down` gives the plugins, and how often it looks.
constexpr std::chrono::milliseconds shut_down_limit{5000};
constexpr std::chrono::milliseconds shut_down_interval{25};

/// One plugin as a scan found it: what to show, and how to start it when
/// it may run.
struct scanned_plugin
{
    service_plugin shown;
    std::optional<start_plan> plan;
};

struct scan_result
{
    std::vector<scanned_plugin> plugins;
    std::vector<std::string> notes;
    std::vector<approved_key> approved_keys;
};

/// A change to the trust store, made before a scan.
using store_change = std::function<void(trust_store&, const inspect_context&,
                                        std::vector<std::string>&)>;

} // namespace

class plugin_service::impl : public std::enable_shared_from_this<impl>
{
public:
    impl(const boost::asio::any_io_executor& executor,
         boost::asio::any_io_executor slow, service_options options)
        : strand_(boost::asio::make_strand(executor)), slow_(std::move(slow)),
          options_(std::move(options)), shut_down_timer_(strand_)
    {
        publish_now();
    }

    template <class Work> void post(Work work)
    {
        boost::asio::post(strand_,
                          [self = shared_from_this(),
                           work = std::move(work)]() mutable { work(*self); });
    }

    std::shared_ptr<const service_snapshot> snapshot() const
    {
        const std::lock_guard lock(snapshot_mutex_);
        return snapshot_;
    }

    // On the strand.

    void set_change_handler(change_handler handler)
    {
        change_handler_ = std::move(handler);
    }

    /// Starts a scan, after `change` to the trust store when one is given.
    /// Scans do not overlap: one asked for while another runs follows it.
    void scan(store_change change)
    {
        if (closed_)
            return;
        if (scanning_)
        {
            waiting_changes_.push_back(std::move(change));
            return;
        }
        scanning_ = true;
        publish_soon();
        boost::asio::post(
            slow_,
            [self = shared_from_this(), change = std::move(change),
             options = options_]() mutable
            {
                scan_result found = read_plugins(options, change);
                self->post([found = std::move(found)](impl& me) mutable
                           { me.take(std::move(found)); });
            });
    }

    /// Runs `work` now, or after the scan that is under way.
    void when_scanned(std::function<void(impl&)> work)
    {
        if (scanning_)
            after_scan_.push_back(std::move(work));
        else
            work(*this);
    }

    const service_plugin* find_shown(std::string_view plugin_id) const
    {
        // The copy that counts is the one without a problem of discovery;
        // failing that, the first.
        const service_plugin* first = nullptr;
        for (const service_plugin& plugin : shown_)
        {
            if (plugin.id != plugin_id)
                continue;
            if (plugin.level.has_value())
                return &plugin;
            if (first == nullptr)
                first = &plugin;
        }
        return first;
    }

    void set_enabled(const std::string& plugin_id, bool enabled)
    {
        const service_plugin* const found = find_shown(plugin_id);
        if (found == nullptr)
            return;
        // The decision is about this exact copy.
        plugin_decision decision;
        decision.plugin_id = found->id;
        decision.hash = found->hash;
        decision.key_id = found->key_id;
        decision.level = found->level.value_or(trust_level::unsigned_plugin);
        decision.enabled = enabled;
        for (const permission_request& asked : found->asked)
            decision.granted.push_back(asked.name);
        const bool official = found->level == trust_level::official;
        scan(
            [decision, official,
             enabled](trust_store& store, const inspect_context& context,
                      std::vector<std::string>& notes) mutable
            {
                const plugin_decision* const old = store.find(
                    decision.plugin_id, decision.hash, decision.key_id);
                if (old != nullptr)
                {
                    decision = *old;
                    decision.enabled = enabled;
                }
                else if (!official)
                {
                    // Nobody decided about this copy.  Only an official
                    // plugin can be switched without a decision.
                    notes.emplace_back(
                        "The plugin waits for your decision first.");
                    return;
                }
                keep(store, std::move(decision), context, notes);
            });
    }

    void decide(plugin_decision decision)
    {
        scan([decision = std::move(decision)](
                 trust_store& store, const inspect_context& context,
                 std::vector<std::string>& notes) mutable
             { keep(store, std::move(decision), context, notes); });
    }

    void forget(const std::string& plugin_id)
    {
        const service_plugin* const found = find_shown(plugin_id);
        if (found == nullptr)
            return;
        scan([id = found->id, hash = found->hash, key_id = found->key_id](
                 trust_store& store, const inspect_context&,
                 std::vector<std::string>&)
             { static_cast<void>(store.forget(id, hash, key_id)); });
    }

    void set_instances(const std::string& plugin_id,
                       std::vector<instance_spec> wanted)
    {
        const auto found = running_.find(plugin_id);
        if (found != running_.end())
            found->second.instances->want(wanted);
        wanted_[plugin_id] = std::move(wanted);
        publish_soon();
    }

    void approve_key(approved_key key)
    {
        scan(
            [key = std::move(key)](trust_store& store,
                                   const inspect_context& context,
                                   std::vector<std::string>& notes) mutable
            {
                key.approved_on = std::chrono::year_month_day{context.today};
                if (const auto stored = store.approve_key(std::move(key));
                    !stored)
                    notes.emplace_back("The key could not be approved.");
            });
    }

    void forget_key(const std::string& key_id)
    {
        scan([key_id](trust_store& store, const inspect_context&,
                      std::vector<std::string>&)
             { static_cast<void>(store.forget_key(key_id)); });
    }

    /// Puts a decision in the store, dated today, and says so when the
    /// store will not take it.
    static void keep(trust_store& store, plugin_decision decision,
                     const inspect_context& context,
                     std::vector<std::string>& notes)
    {
        decision.decided_on = std::chrono::year_month_day{context.today};
        if (const auto stored = store.decide(std::move(decision)); !stored)
            notes.emplace_back(
                "Your decision about a plugin could not be kept.");
    }

    std::shared_ptr<plugin_runner> runner(std::string_view plugin_id) const
    {
        const auto found = running_.find(std::string(plugin_id));
        return found == running_.end() ? nullptr : found->second.runner;
    }

    void shut_down(std::function<void()> done)
    {
        closed_ = true;
        waiting_changes_.clear();
        done_ = std::move(done);
        for (auto& [id, entry] : running_)
            entry.runner->stop("Studio is closing");
        shut_down_deadline_ =
            std::chrono::steady_clock::now() + shut_down_limit;
        wait_for_stops();
    }

private:
    struct running_plugin
    {
        std::shared_ptr<plugin_runner> runner;
        std::shared_ptr<plugin_instances> instances;
        /// The extension points offered to this plugin.  Shared so that a
        /// copy of the entry is the same plugin.
        std::shared_ptr<std::vector<std::unique_ptr<extension_handler>>>
            handlers;
        /// The start of the plugin that the instances were last told is
        /// ready; 0 when they were told it has gone.
        std::uint64_t ready_start = 0;
        plugin_hash hash;
        std::vector<permission> granted;
    };

    /// On the worker thread: everything that reads a disk.
    static scan_result read_plugins(const service_options& options,
                                    const store_change& change)
    {
        scan_result result;
        inspect_context context =
            context_for_this_computer(options.computer, &result.notes);
        context.sandbox = available_sandbox().level();
        context.capabilities = options.plan.capabilities;
        // With nobody to ask, there is no licence: a plugin that needs
        // one does not run.
        context.licence =
            options.licence ? options.licence() : license::no_licence();
        context.licence_known = true;
        if (options.change_context_for_tests)
            options.change_context_for_tests(context);
        if (change)
        {
            trust_store store =
                context.trust.has_value() ? *context.trust : trust_store{};
            change(store, context, result.notes);
            if (options.computer.trust_file.empty())
            {
                result.notes.emplace_back(
                    "There is nowhere to keep your decisions about plugins.");
            }
            else if (const auto saved =
                         save_trust_store(options.computer.trust_file, store);
                     !saved)
            {
                result.notes.emplace_back(
                    "Your decision about a plugin could not be saved.");
            }
            else
            {
                context.trust = std::move(store);
                context.user_keys.clear();
                for (const approved_key& key : context.trust->keys())
                    context.user_keys.push_back(
                        approved_signer{key.key_id, key.public_key});
            }
        }

        plan_context plan = options.plan;
        if (context.rules.has_value())
            plan.min_sandbox = context.rules->min_sandbox;

        const discovery found = discover_plugins(options.roots);
        for (const found_plugin& plugin : found.plugins)
        {
            scanned_plugin scanned;
            service_plugin& shown = scanned.shown;
            shown.directory = plugin.directory;
            shown.scope = plugin.scope;
            shown.hash = plugin.hash;
            shown.declared = plugin.declared;
            if (plugin.declared.has_value())
            {
                shown.id = plugin.declared->id;
                shown.name = plugin.declared->name;
                shown.version = to_string(plugin.declared->version);
                shown.author = plugin.declared->author.name;
                shown.asked = plugin.declared->permissions;
            }
            if (plugin.problem)
            {
                // Discovery already knows it cannot be used: a manifest
                // that cannot be read, a copy another copy stands before.
                shown.problem = plugin.problem;
                shown.detail = plugin.detail;
                result.plugins.push_back(std::move(scanned));
                continue;
            }

            inspect_context own = context;
            own.install = plugin.scope == plugin_scope::for_user
                              ? install_kind::for_one_user
                              : install_kind::for_everyone;
            const inspection checked = inspect_plugin(plugin.directory, own);
            shown.hash = checked.hash;
            shown.level = checked.level;
            shown.key_id = checked.key_id;
            const plugin_decision* decision = nullptr;
            if (context.trust.has_value() && checked.declared.has_value())
            {
                decision = context.trust->find(checked.declared->id,
                                               checked.hash, checked.key_id);
            }
            shown.decided = decision != nullptr;
            if (decision != nullptr)
            {
                shown.decision = *decision;
            }
            else if (context.trust.has_value() && checked.declared.has_value())
            {
                const plugin_decision* const earlier = context.trust->previous(
                    checked.declared->id, checked.hash, checked.key_id);
                if (earlier != nullptr)
                    shown.earlier_decision = *earlier;
            }

            std::string detail;
            auto planned = plan_start(checked, decision, plan, &detail);
            if (!planned)
            {
                shown.problem = planned.error();
                shown.detail = std::move(detail);
            }
            else
            {
                shown.enabled = planned->enabled;
                shown.granted = planned->granted;
                shown.withheld = planned->withheld;
                if (planned->enabled)
                {
                    std::error_code ignored;
                    std::filesystem::create_directories(
                        planned->options.process.working_directory, ignored);
                    scanned.plan = std::move(*planned);
                }
            }
            result.plugins.push_back(std::move(scanned));
        }
        if (context.trust.has_value())
            result.approved_keys = context.trust->keys();
        for (const root_problem& root : found.roots)
        {
            if (root.problem && !root.detail.empty())
                result.notes.push_back(root.detail);
        }
        return result;
    }

    /// Back on the strand: what runs is made to match what may run.
    void take(scan_result found)
    {
        scanning_ = false;
        ++scans_;
        notes_ = std::move(found.notes);
        approved_keys_ = std::move(found.approved_keys);
        shown_.clear();
        std::map<std::string, running_plugin> keep;
        for (scanned_plugin& scanned : found.plugins)
        {
            const std::string& id = scanned.shown.id;
            const auto old = running_.find(id);
            const bool runs = scanned.plan.has_value() && !closed_ &&
                              !id.empty() && keep.find(id) == keep.end();
            if (runs && old != running_.end() &&
                old->second.hash == scanned.shown.hash &&
                old->second.granted == scanned.shown.granted)
            {
                // The same copy still runs, with the same permissions.
                keep.insert(*old);
                running_.erase(old);
            }
            else if (runs)
            {
                running_plugin entry;
                const std::filesystem::path data_directory =
                    scanned.plan->options.process.working_directory;
                entry.hash = scanned.shown.hash;
                entry.granted = scanned.shown.granted;
                entry.runner =
                    plugin_runner::create(boost::asio::any_io_executor(strand_),
                                          std::move(scanned.plan->options));
                entry.instances = std::make_shared<plugin_instances>(
                    scanned.shown.declared.has_value()
                        ? scanned.shown.declared->extension_points
                        : std::vector<extension_point>{},
                    [runner =
                         entry.runner](api_message message,
                                       plugin_instances::answer_handler handler)
                    {
                        runner->request(std::move(message), std::move(handler));
                    });
                entry.instances->on_change(
                    [weak = weak_from_this()]()
                    {
                        if (const auto self = weak.lock())
                            self->publish_soon();
                    });
                if (const auto wanted = wanted_.find(id);
                    wanted != wanted_.end())
                    entry.instances->want(wanted->second);
                entry.handlers = std::make_shared<
                    std::vector<std::unique_ptr<extension_handler>>>();
                if (options_.extensions)
                {
                    extension_context context;
                    context.plugin_id = id;
                    context.plugin_name = scanned.shown.name;
                    context.granted = scanned.shown.granted;
                    context.data_directory = data_directory;
                    context.instances = entry.instances;
                    context.executor = boost::asio::any_io_executor(strand_);
                    context.link.answer =
                        [runner = entry.runner](std::uint64_t re,
                                                api_message message)
                    { runner->answer(re, std::move(message)); };
                    context.link.notify =
                        [runner = entry.runner](api_message message)
                    { runner->notify(std::move(message)); };
                    context.link.request =
                        [runner =
                             entry.runner](api_message message,
                                           plugin_link::answer_handler handler,
                                           std::chrono::milliseconds timeout)
                    {
                        runner->request(std::move(message), std::move(handler),
                                        timeout);
                    };
                    *entry.handlers = options_.extensions(context);
                }
                entry.runner->on_message(
                    [weak = weak_from_this(), id](const api_message& message)
                    {
                        if (const auto self = weak.lock())
                            self->plugin_message(id, message);
                    });
                entry.runner->on_change(
                    [weak = weak_from_this(), id]()
                    {
                        if (const auto self = weak.lock())
                            self->post([id](impl& me)
                                       { me.runner_changed(id); });
                    });
                entry.runner->start();
                keep.emplace(id, std::move(entry));
            }
            shown_.push_back(std::move(scanned.shown));
        }
        // What is left ran and may no longer: gone, changed or switched
        // off.
        for (auto& [id, entry] : running_)
        {
            entry.runner->stop("the plugin is no longer to run");
            if (entry.ready_start != 0)
            {
                entry.instances->plugin_gone();
                for (const auto& handler : *entry.handlers)
                    handler->plugin_gone();
            }
            leaving_.push_back(std::move(entry.runner));
        }
        running_ = std::move(keep);
        publish_now();

        auto later = std::move(after_scan_);
        after_scan_.clear();
        for (auto& work : later)
            work(*this);
        publish_soon();

        if (!waiting_changes_.empty() && !closed_)
        {
            // One scan serves every change that waited.
            auto changes = std::move(waiting_changes_);
            waiting_changes_.clear();
            scan(
                [changes = std::move(changes)](trust_store& store,
                                               const inspect_context& context,
                                               std::vector<std::string>& notes)
                {
                    for (const store_change& change : changes)
                    {
                        if (change)
                            change(store, context, notes);
                    }
                });
        }
    }

    /// A message of an extension point came from a plugin.  The first
    /// handler that takes it answers it; a message nobody takes is one
    /// this host does not know.
    void plugin_message(const std::string& id, const api_message& message)
    {
        const auto found = running_.find(id);
        if (found == running_.end())
            return;
        // The plugin is told it is ready before its first message is
        // taken, whichever of the two the service hears of first.
        runner_changed(id);
        const auto handlers = found->second.handlers;
        for (const auto& handler : *handlers)
        {
            if (handler->take(message))
                return;
        }
        found->second.runner->unknown(message);
    }

    /// A runner's snapshot changed: its instances are told when the
    /// plugin became ready and when it went.
    void runner_changed(const std::string& id)
    {
        const auto found = running_.find(id);
        if (found != running_.end())
        {
            running_plugin& entry = found->second;
            const auto now = entry.runner->snapshot();
            const bool ready = now->state == run_state::ready;
            if (entry.ready_start != 0 &&
                (!ready || now->starts != entry.ready_start))
            {
                entry.ready_start = 0;
                entry.instances->plugin_gone();
                for (const auto& handler : *entry.handlers)
                    handler->plugin_gone();
            }
            if (ready && entry.ready_start == 0)
            {
                entry.ready_start = now->starts;
                for (const auto& handler : *entry.handlers)
                    handler->plugin_ready();
                entry.instances->plugin_ready();
            }
        }
        publish_soon();
    }

    void wait_for_stops()
    {
        const bool all_gone = std::ranges::all_of(
            running_,
            [](const auto& entry)
            {
                return entry.second.runner->snapshot()->state ==
                       run_state::stopped;
            });
        if (all_gone || std::chrono::steady_clock::now() >= shut_down_deadline_)
        {
            for (auto& [id, entry] : running_)
                entry.runner->abandon();
            for (auto& runner : leaving_)
                runner->abandon();
            running_.clear();
            leaving_.clear();
            publish_soon();
            if (done_)
            {
                auto done = std::move(done_);
                done_ = nullptr;
                done();
            }
            return;
        }
        shut_down_timer_.expires_after(shut_down_interval);
        shut_down_timer_.async_wait(
            [self = shared_from_this()](const error_code& cancelled)
            {
                if (!cancelled)
                    self->wait_for_stops();
            });
    }

    void publish_soon()
    {
        if (publish_waiting_)
            return;
        publish_waiting_ = true;
        boost::asio::post(strand_,
                          [self = shared_from_this()]()
                          {
                              self->publish_waiting_ = false;
                              self->forget_stopped();
                              self->publish_now();
                              if (self->change_handler_)
                                  self->change_handler_();
                          });
    }

    /// Runners that were told to stop are let go once they have.
    void forget_stopped()
    {
        std::erase_if(
            leaving_, [](const auto& runner)
            { return runner->snapshot()->state == run_state::stopped; });
    }

    void publish_now()
    {
        auto next = std::make_shared<service_snapshot>();
        next->plugins = shown_;
        for (service_plugin& plugin : next->plugins)
        {
            // A copy that another stands before shares its id and not
            // its run.
            const auto found = running_.find(plugin.id);
            if (found != running_.end() && !plugin.problem &&
                found->second.hash == plugin.hash)
            {
                plugin.run = found->second.runner->snapshot();
                plugin.instances = found->second.instances->views();
            }
        }
        next->notes = notes_;
        next->approved_keys = approved_keys_;
        next->scanning = scanning_;
        next->scans = scans_;
        next->revision = ++revision_;
        const std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(next);
    }

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::any_io_executor slow_;
    service_options options_;
    change_handler change_handler_;

    bool scanning_ = false;
    std::uint64_t scans_ = 0;
    bool closed_ = false;
    /// Calls that came while a scan ran.  They need what the scan finds.
    std::vector<std::function<void(impl&)>> after_scan_;
    std::vector<store_change> waiting_changes_;
    std::vector<service_plugin> shown_;
    std::vector<std::string> notes_;
    std::vector<approved_key> approved_keys_;
    /// The instances wanted of each plugin, by its id.
    std::map<std::string, std::vector<instance_spec>> wanted_;
    std::map<std::string, running_plugin> running_;
    /// Runners that were told to stop and have not yet.
    std::vector<std::shared_ptr<plugin_runner>> leaving_;

    boost::asio::steady_timer shut_down_timer_;
    std::chrono::steady_clock::time_point shut_down_deadline_{};
    std::function<void()> done_;

    std::uint64_t revision_ = 0;
    bool publish_waiting_ = false;
    mutable std::mutex snapshot_mutex_;
    std::shared_ptr<const service_snapshot> snapshot_;
};

std::shared_ptr<plugin_service>
plugin_service::create(const boost::asio::any_io_executor& executor,
                       const boost::asio::any_io_executor& slow,
                       service_options options)
{
    return std::shared_ptr<plugin_service>(new plugin_service(
        std::make_shared<impl>(executor, slow, std::move(options))));
}

plugin_service::plugin_service(std::shared_ptr<impl> state)
    : impl_(std::move(state))
{
}

plugin_service::~plugin_service() = default;

void plugin_service::on_change(change_handler handler)
{
    impl_->post([handler = std::move(handler)](impl& self) mutable
                { self.set_change_handler(std::move(handler)); });
}

void plugin_service::rescan()
{
    impl_->post([](impl& self) { self.scan(store_change{}); });
}

void plugin_service::set_enabled(std::string plugin_id, bool enabled)
{
    impl_->post(
        [plugin_id = std::move(plugin_id), enabled](impl& self) mutable
        {
            self.when_scanned(
                [plugin_id = std::move(plugin_id), enabled](impl& me)
                { me.set_enabled(plugin_id, enabled); });
        });
}

void plugin_service::decide(plugin_decision decision)
{
    impl_->post([decision = std::move(decision)](impl& self) mutable
                { self.decide(std::move(decision)); });
}

void plugin_service::forget(std::string plugin_id)
{
    impl_->post(
        [plugin_id = std::move(plugin_id)](impl& self) mutable
        {
            self.when_scanned([plugin_id = std::move(plugin_id)](impl& me)
                              { me.forget(plugin_id); });
        });
}

void plugin_service::set_instances(std::string plugin_id,
                                   std::vector<instance_spec> wanted)
{
    impl_->post([plugin_id = std::move(plugin_id),
                 wanted = std::move(wanted)](impl& self) mutable
                { self.set_instances(plugin_id, std::move(wanted)); });
}

void plugin_service::approve_key(approved_key key)
{
    impl_->post([key = std::move(key)](impl& self) mutable
                { self.approve_key(std::move(key)); });
}

void plugin_service::forget_key(std::string key_id)
{
    impl_->post([key_id = std::move(key_id)](impl& self)
                { self.forget_key(key_id); });
}

std::shared_ptr<const service_snapshot> plugin_service::snapshot() const
{
    return impl_->snapshot();
}

std::shared_ptr<plugin_runner>
plugin_service::runner(std::string_view plugin_id) const
{
    return impl_->runner(plugin_id);
}

void plugin_service::shut_down(std::function<void()> done)
{
    impl_->post([done = std::move(done)](impl& self) mutable
                { self.shut_down(std::move(done)); });
}

} // namespace vss::plugin::host
