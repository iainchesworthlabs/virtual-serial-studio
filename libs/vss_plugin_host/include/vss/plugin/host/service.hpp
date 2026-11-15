//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plugins of one Studio: found, checked, started where they may run,
// kept running, and stopped.  docs/plugins.md, sections 5 and 11, are the
// design.
//
// A service joins what the other files decide.  `discover_plugins` finds
// the installed plugins, `inspect_plugin` checks each, `plan_start` says
// whether and how it runs, and a `plugin_runner` keeps each one running.
// The service adds the order of those steps, the threads, and the user's
// decisions written to the trust store.  It makes no rule of its own.
//
// Threads.  Reading plugin folders and hashing their files is slow, so it
// is done on a second executor the service is given, never on the first.  The
// runners and the service's own state live on a strand of the executor the
// service is given.  Every member function may be called from any thread
// and returns at once.  `snapshot` never waits.  The change handler is
// called on the service's strand and must not block.
//
// The service belongs to the program that owns the ports: Studio's own
// process today, the agent when Studio becomes its client.
//

#pragma once

#include <vss/core/error.hpp>
#include <vss/license/license.hpp>
#include <vss/plugin/discover.hpp>
#include <vss/plugin/hash.hpp>
#include <vss/plugin/host/runner.hpp>
#include <vss/plugin/host/start_plan.hpp>
#include <vss/plugin/install.hpp>
#include <vss/plugin/manifest.hpp>
#include <vss/plugin/trust_store.hpp>
#include <vss/plugin/verify.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::plugin::host
{

struct service_options
{
    /// The folders plugins are installed in, in the order they count.
    std::vector<plugin_root> roots;
    /// Where the policy file and the trust store of this computer are.
    computer_options computer;
    /// Where plugins keep their data, the engine capabilities of this
    /// build, and the seed of the waits between starts.  `min_sandbox` is
    /// taken from the administrator's policy when there is one.
    plan_context plan;
    /// Asked at every scan for the licence on this computer.  When it is
    /// not set, no plugin that needs a licence runs.
    std::function<license::status()> licence;
    /// For tests only: changes what was read from this computer before
    /// any plugin is checked, so that a test can name a key list of its
    /// own as the official one.  A program that ships leaves it unset;
    /// setting it there would let it decide what counts as official.
    std::function<void(inspect_context&)> change_context_for_tests;
};

/// One installed plugin, as the service sees it at one moment.
struct service_plugin
{
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::filesystem::path directory;
    plugin_scope scope = plugin_scope::for_user;
    plugin_hash hash;
    std::optional<trust_level> level;
    std::string key_id;
    /// Why the plugin does not run, with one line for a person; success
    /// when it may run.  The codes are those of `plugin_errc`.
    error_code problem;
    std::string detail;
    /// It may run and is switched on.
    bool enabled = false;
    /// The user has decided about this exact copy.
    bool decided = false;
    /// What the manifest asks for, what it gets and what it does not.
    std::vector<permission_request> asked;
    std::vector<permission> granted;
    std::vector<permission> withheld;
    /// Its run, when it has a runner; null otherwise.
    std::shared_ptr<const runner_snapshot> run;
};

struct service_snapshot
{
    std::vector<service_plugin> plugins;
    /// Lines for a person about the computer: a policy file that could
    /// not be used, a trust store that could not be written.
    std::vector<std::string> notes;
    /// True while the plugin folders are being read.
    bool scanning = false;
    std::uint64_t revision = 0;
};

class plugin_service
{
public:
    using change_handler = std::function<void()>;

    /// A service that has found nothing yet.  Call `rescan`.
    ///
    /// `executor` carries the service and its runners.  `slow` carries
    /// the reading and hashing of plugin folders, one scan at a time; it
    /// must be another thread than the one that must stay responsive, and
    /// it must outlive every scan: call `shut_down` and let both run dry
    /// before either goes.
    static std::shared_ptr<plugin_service>
    create(const boost::asio::any_io_executor& executor,
           const boost::asio::any_io_executor& slow, service_options options);

    ~plugin_service();
    plugin_service(const plugin_service&) = delete;
    plugin_service& operator=(const plugin_service&) = delete;

    /// Set once, before `rescan`.
    void on_change(change_handler handler);

    /// Reads the plugin folders again, checks every plugin, and starts
    /// and stops plugins so that what runs is what may run.  A plugin
    /// whose files changed is stopped and judged anew.
    void rescan();

    /// The user switches a plugin on or off.  The choice is stored with
    /// the decision for this exact copy.  For a plugin nobody has decided
    /// about, only an official one can be switched this way.
    void set_enabled(std::string plugin_id, bool enabled);

    /// The user decides about one copy of a plugin: to trust it, with the
    /// permissions granted, and whether it is on.  Stored in the trust
    /// store, then the plugins are judged anew.
    void decide(plugin_decision decision);

    /// Forgets the decision about one copy of a plugin.
    void forget(std::string plugin_id);

    /// The latest snapshot.  Any thread; never waits.
    std::shared_ptr<const service_snapshot> snapshot() const;

    /// The runner of a plugin that is running, for the code that carries
    /// an extension point; null when it has none.  Call it on the
    /// service's strand, as from the change handler.
    std::shared_ptr<plugin_runner> runner(std::string_view plugin_id) const;

    /// Stops every plugin, each asked and then ended, all at once, and
    /// calls `done` on the service's strand when all have gone or after
    /// five seconds, when those left are ended without asking.  The
    /// service starts nothing afterwards.
    void shut_down(std::function<void()> done);

    class impl;

private:
    explicit plugin_service(std::shared_ptr<impl> state);
    std::shared_ptr<impl> impl_;
};

} // namespace vss::plugin::host
