//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/plugin/host/service.hpp>

#include <vss/core/error.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/error.hpp>
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
        plugin_hash hash;
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
        if (options.licence)
        {
            context.licence = options.licence();
            context.licence_known = true;
        }
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
        notes_ = std::move(found.notes);
        shown_.clear();
        std::map<std::string, running_plugin> keep;
        for (scanned_plugin& scanned : found.plugins)
        {
            const std::string& id = scanned.shown.id;
            const auto old = running_.find(id);
            const bool runs = scanned.plan.has_value() && !closed_ &&
                              !id.empty() && keep.find(id) == keep.end();
            if (runs && old != running_.end() &&
                old->second.hash == scanned.shown.hash)
            {
                // The same copy still runs.
                keep.insert(*old);
                running_.erase(old);
            }
            else if (runs)
            {
                running_plugin entry;
                entry.hash = scanned.shown.hash;
                entry.runner =
                    plugin_runner::create(boost::asio::any_io_executor(strand_),
                                          std::move(scanned.plan->options));
                entry.runner->on_change(
                    [weak = weak_from_this()]()
                    {
                        if (const auto self = weak.lock())
                            self->post([](impl& me) { me.publish_soon(); });
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
            leaving_.push_back(std::move(entry.runner));
        }
        running_ = std::move(keep);
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
            const auto found = running_.find(plugin.id);
            if (found != running_.end())
                plugin.run = found->second.runner->snapshot();
        }
        next->notes = notes_;
        next->scanning = scanning_;
        next->revision = ++revision_;
        const std::lock_guard lock(snapshot_mutex_);
        snapshot_ = std::move(next);
    }

    boost::asio::strand<boost::asio::any_io_executor> strand_;
    boost::asio::any_io_executor slow_;
    service_options options_;
    change_handler change_handler_;

    bool scanning_ = false;
    bool closed_ = false;
    std::vector<store_change> waiting_changes_;
    std::vector<service_plugin> shown_;
    std::vector<std::string> notes_;
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
            const auto now = self.snapshot();
            const auto found =
                std::ranges::find(now->plugins, plugin_id, &service_plugin::id);
            if (found == now->plugins.end())
                return;
            // The decision is about this exact copy.
            plugin_decision decision;
            decision.plugin_id = found->id;
            decision.hash = found->hash;
            decision.key_id = found->key_id;
            decision.level =
                found->level.value_or(trust_level::unsigned_plugin);
            decision.enabled = enabled;
            const bool decided = found->decided;
            const bool official = found->level == trust_level::official;
            const std::vector<permission> granted = found->granted;
            self.scan(
                [decision, decided, official,
                 granted](trust_store& store, const inspect_context&,
                          std::vector<std::string>& notes) mutable
                {
                    const plugin_decision* const old = store.find(
                        decision.plugin_id, decision.hash, decision.key_id);
                    if (old != nullptr)
                    {
                        plugin_decision changed = *old;
                        changed.enabled = decision.enabled;
                        static_cast<void>(store.decide(std::move(changed)));
                        return;
                    }
                    // Nobody decided about this copy.  Only an official
                    // plugin can be switched without a decision.
                    static_cast<void>(decided);
                    if (!official)
                    {
                        notes.emplace_back(
                            "The plugin waits for your decision first.");
                        return;
                    }
                    decision.granted = granted;
                    static_cast<void>(store.decide(std::move(decision)));
                });
        });
}

void plugin_service::decide(plugin_decision decision)
{
    impl_->post(
        [decision = std::move(decision)](impl& self) mutable
        {
            self.scan(
                [decision = std::move(decision)](
                    trust_store& store, const inspect_context&,
                    std::vector<std::string>& notes) mutable
                {
                    if (const auto stored = store.decide(std::move(decision));
                        !stored)
                    {
                        notes.emplace_back(
                            "Your decision about a plugin could not "
                            "be kept.");
                    }
                });
        });
}

void plugin_service::forget(std::string plugin_id)
{
    impl_->post(
        [plugin_id = std::move(plugin_id)](impl& self) mutable
        {
            const auto now = self.snapshot();
            const auto found =
                std::ranges::find(now->plugins, plugin_id, &service_plugin::id);
            if (found == now->plugins.end())
                return;
            self.scan([id = found->id, hash = found->hash,
                       key_id = found->key_id](trust_store& store,
                                               const inspect_context&,
                                               std::vector<std::string>&)
                      { static_cast<void>(store.forget(id, hash, key_id)); });
        });
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
