//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the operating system may stop Studio doing, and the smallest thing the
// user can do about it.
//
// Windows asks for elevation (UAC) and filters inbound connections (Windows
// Firewall). Linux limits serial devices to groups and may confine a program
// (AppArmor, SELinux, Flatpak, Snap). macOS has an application firewall,
// privacy prompts and a sandbox. This header is the one place the GUI and the
// command line ask about all of it.
//
// The design follows three rules:
//
//  * Honest. A platform that cannot be asked answers `state::unknown`, and the
//    caller says so. Nothing is guessed from a missing answer.
//  * Explained. A fact carries a reason code and, where there is one, the exact
//    command or text the user can run. The GUI turns the code into a sentence
//    (translated); the library never writes user-facing prose.
//  * Never silent. Nothing in this header changes a protection. A step that
//  does
//    (a firewall rule) runs only when the caller asks for it by name, after the
//    user has agreed, in a separate elevated process that does that one thing.
//
// `observe()` reads the host and is the only part that depends on the platform.
// `evaluate()` turns an observation into facts and is plain code, so every
// outcome is tested on every platform with injected observations.
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vss::permissions
{

/// How a fact stands.
enum class state
{
    /// Nothing stands in the way.
    ok,
    /// Nothing is wrong, but the user should know (for example "this needs
    /// administrator rights when you create a pair").
    info,
    /// Something is blocked or missing and Studio cannot work around it.
    attention,
    /// The platform cannot be asked, or the question failed. Say so.
    unknown,
    /// The fact does not apply to this operating system or situation.
    not_applicable
};

/// What a fact is about.
enum class topic
{
    /// Windows: is this process elevated, and could the user elevate it.
    windows_elevation,
    /// Windows: would Windows Firewall let other computers reach a listener.
    windows_firewall,
    /// Linux and macOS: may this user open the serial devices (group, mode,
    /// policy or sandbox).
    serial_access,
    /// Linux and macOS: does something confine this program (Flatpak, Snap,
    /// AppArmor, the macOS sandbox,
    /// SELinux) so that it may be refused devices or directories.
    confinement,
    /// Linux: would ufw or firewalld stand between a listener and the network.
    linux_firewall,
    /// macOS: what the Application Firewall does to a listener.
    macos_firewall,
    /// macOS: the Local Network privacy permission (macOS 15 and later).
    macos_local_network
};

enum class transport_protocol
{
    tcp,
    udp
};

/// A socket Studio listens on, or is about to.
struct listener
{
    transport_protocol protocol = transport_protocol::tcp;
    /// Empty means every address, as in `tcp-listen://:4001`.
    std::string host;
    /// Zero when the port is not chosen yet.
    std::uint16_t port = 0;
};

/// What the check is for.
struct context
{
    /// The program that listens. Empty: this executable.
    std::string program_path;
    /// The listeners the check should cover. None: only the general facts.
    std::vector<listener> listeners;
    /// The serial devices the check should cover (`/dev/ttyUSB0`, ...). None:
    /// no device is checked.
    std::vector<std::string> device_paths;
    /// Paths a real open already failed on with "permission denied". A path the
    /// file mode permits but that was refused is a policy denial (AppArmor,
    /// SELinux, a sandbox), which the fact says.
    std::vector<std::string> refused_paths;
};

/// One reason code per outcome. The GUI chooses the sentence from the topic and
/// the reason, and the tests assert them, so a code is never reused for a new
/// meaning.
namespace reason
{
inline constexpr std::string_view none = "";
// windows_elevation
inline constexpr std::string_view elevated = "elevated";
inline constexpr std::string_view admin_not_elevated = "admin_not_elevated";
inline constexpr std::string_view standard_user = "standard_user";
// windows_firewall
inline constexpr std::string_view loopback_only = "loopback_only";
inline constexpr std::string_view firewall_off = "firewall_off";
inline constexpr std::string_view allow_rule = "allow_rule";
inline constexpr std::string_view no_allow_rule = "no_allow_rule";
inline constexpr std::string_view blocked_by_rule = "blocked_by_rule";
inline constexpr std::string_view block_all_inbound = "block_all_inbound";
// serial_access
inline constexpr std::string_view devices_open = "devices_open";
inline constexpr std::string_view no_devices = "no_devices";
inline constexpr std::string_view not_in_group = "not_in_group";
inline constexpr std::string_view relogin_needed = "relogin_needed";
inline constexpr std::string_view mode_denies = "mode_denies";
inline constexpr std::string_view denied_by_policy = "denied_by_policy";
// confinement
inline constexpr std::string_view unconfined = "unconfined";
inline constexpr std::string_view flatpak = "flatpak";
inline constexpr std::string_view snap = "snap";
inline constexpr std::string_view apparmor_profile = "apparmor_profile";
inline constexpr std::string_view selinux_enforcing = "selinux_enforcing";
// linux_firewall
inline constexpr std::string_view ufw_active = "ufw_active";
inline constexpr std::string_view firewalld_active = "firewalld_active";
inline constexpr std::string_view rules_need_root = "rules_need_root";
// macos_firewall, macos_local_network, confinement on macOS
inline constexpr std::string_view alf_prompt = "alf_prompt";
inline constexpr std::string_view local_network_prompt = "local_network_prompt";
inline constexpr std::string_view macos_sandbox = "macos_sandbox";
// every topic
inline constexpr std::string_view not_asked = "not_asked";
inline constexpr std::string_view other_platform = "other_platform";
} // namespace reason

/// The networks a rule can apply to, as the bits Windows Firewall uses.
inline constexpr long profile_domain = 1;
inline constexpr long profile_private = 2;
inline constexpr long profile_public = 4;
inline constexpr long profile_all = 0x7fffffff;

/// Which computers a rule lets in.
enum class rule_scope
{
    /// The local subnet only (`LocalSubnet`). The default.
    local_subnet,
    /// Any address. Only when the user chooses it.
    any_address
};

/// The only firewall rule Studio makes: one program, one protocol, one port,
/// one scope, on the networks the computer is on now.
struct firewall_rule_spec
{
    std::string program;
    transport_protocol protocol = transport_protocol::tcp;
    std::uint16_t port = 0;
    rule_scope scope = rule_scope::local_subnet;
    /// Bits of `profile_*`. Never `profile_all`.
    long profiles = profile_private;
};

/// The one privileged step on Linux: put the current user in the group that
/// owns the serial devices. Only the user running Studio, and only a group on
/// the short list in `validate`, can be named.
struct group_membership_spec
{
    std::string user;
    std::string group;
};

/// One answer.
struct fact
{
    topic what = topic::windows_elevation;
    state status = state::unknown;
    /// One of `reason::*`.
    std::string why;
    /// The thing the fact is about: a program path, a port, a group, a device.
    std::string subject;
    /// The exact command the user can run, shown and copied but never run by
    /// Studio. Empty when there is none.
    std::string remedy;
    /// True when Studio can do the step itself, in an elevated helper, after
    /// the user agrees. The step is named by `firewall_rule_spec`.
    bool can_act = false;
    /// The step itself, when `can_act`: what the confirmation shows and what
    /// the elevated helper does.
    std::optional<firewall_rule_spec> step;
    /// The Linux step, when the fix is a group membership and `pkexec` can ask
    /// for it.
    std::optional<group_membership_spec> group_step;
    /// For a listener fact, the listener it is about (zero: none).
    std::uint16_t port = 0;
    transport_protocol protocol = transport_protocol::tcp;
};

// ---------------------------------------------------------------------------
// What the host says
// ---------------------------------------------------------------------------

/// A yes or no that may not be known.
enum class tri
{
    unknown,
    no,
    yes
};

/// How a rule or the firewall treats one listener.
enum class firewall_verdict
{
    unknown,
    /// No enabled rule mentions the program and the port.
    no_rule,
    /// An enabled allow rule covers the program, protocol and port.
    allowed,
    /// An enabled block rule covers them. Block wins over allow.
    blocked
};

/// A Windows firewall rule, reduced to what decides a listener.
struct firewall_rule_record
{
    bool enabled = false;
    bool inbound = true;
    bool allow = true;
    /// Empty: any program.
    std::string program;
    /// 6 TCP, 17 UDP, 256 any.
    int protocol = 256;
    /// The Windows list: "*", "4001", "4000-4010,5000". Empty or "*" is any.
    std::string local_ports;
    /// Bit 1 domain, 2 private, 4 public; 0x7fffffff is every profile.
    long profiles = 0x7fffffff;
};

struct windows_observation
{
    tri elevated = tri::unknown;
    /// The account is an administrator, elevated or not (a split token).
    tri administrator = tri::unknown;
    /// The firewall is on for at least one active profile.
    tri firewall_on = tri::unknown;
    /// The active profile blocks every inbound connection, rules or not.
    tri block_all_inbound = tri::unknown;
    /// Bits of `profile_*` for the networks the computer is on now. Zero when
    /// not known.
    long active_profiles = 0;
    /// Every rule, when the rules could be read.
    std::optional<std::vector<firewall_rule_record>> rules;
};

/// One serial device as the user sees it.
struct device_access
{
    std::string path;
    bool exists = false;
    /// Name of the group that owns the device; empty when unknown.
    std::string group;
    /// `open(2)` for reading and writing would be allowed by the file mode and
    /// ownership (`access(2)`). Policy (AppArmor, SELinux, a sandbox) is not
    /// part of this answer, which is the point: a refusal despite a yes here is
    /// policy.
    tri permitted = tri::unknown;
    /// The running process has the owning group.
    tri in_group = tri::unknown;
    /// The account is listed in the owning group in /etc/group, whether or not
    /// this session has picked it up yet.
    tri listed_in_group = tri::unknown;
    /// The group may read and write the device (mode bits).
    tri group_may_write = tri::unknown;
};

/// Who confines the program, from the security label in
/// /proc/self/attr/current.
struct security_label
{
    /// "", "apparmor" or "selinux".
    std::string module;
    /// The AppArmor profile, or the SELinux type.
    std::string name;
    /// "enforce", "complain", "unconfined" or "" (AppArmor); the SELinux mode
    /// ("enforcing", "permissive") comes from `selinux_mode`.
    std::string mode;
};

struct linux_observation
{
    std::string user;
    tri root = tri::unknown;
    std::vector<device_access> devices;
    security_label label;
    /// "enforcing", "permissive" or "disabled"; empty when it could not be
    /// read.
    std::string selinux_mode;
    /// "", "flatpak" or "snap".
    std::string sandbox;
    /// The Flatpak application id or the Snap name.
    std::string sandbox_name;
    tri ufw_active = tri::unknown;
    tri firewalld_active = tri::unknown;
    /// pkexec and usermod are installed, so the group step can ask polkit.
    tri can_ask_polkit = tri::unknown;
};

struct macos_observation
{
    std::string user;
    std::vector<device_access> devices;
    /// The Application Firewall is on.
    tri firewall_on = tri::unknown;
    /// "Block all incoming connections" is on.
    tri block_all = tri::unknown;
    /// The major number of the Darwin kernel: 24 is macOS 15, where apps need
    /// the Local Network permission. Zero when not known.
    int darwin_major = 0;
    /// This program runs in the App Sandbox.
    tri sandboxed = tri::unknown;
};

struct observation
{
    windows_observation windows;
    linux_observation linux_host;
    macos_observation macos_host;
};

/// Reads the host. Nothing is changed. Takes a moment on Windows, where the
/// firewall rules are read, so call it off the GUI thread.
observation observe(const context& ask = {});

/// Turns an observation into facts. The same input always gives the same facts.
std::vector<fact> evaluate(const observation& seen, const context& ask,
                           std::string_view host);

/// `observe` then `evaluate` for this host.
std::vector<fact> probe(const context& ask = {});

/// "windows", "linux" or "macos".
std::string_view host_name() noexcept;

// ---------------------------------------------------------------------------
// Firewall rules (Windows)
// ---------------------------------------------------------------------------

/// True for 127.0.0.0/8, ::1 and the name `localhost`. An empty host is every
/// address and so is not loopback.
bool is_loopback_host(std::string_view host);

/// Reads `tcp-listen://host:port` and `udp-listen://host:port` (the host may be
/// empty or a bracketed IPv6 address). False for anything else.
bool parse_listen_uri(std::string_view uri, listener& out);

/// True when the Windows list covers the port: "*", "80", "10-20,30".
bool port_list_covers(std::string_view list, std::uint16_t port) noexcept;

/// Whether `rules` let a listener in. `program` is compared without regard to
/// case. Block rules win.
firewall_verdict
firewall_verdict_for(const std::vector<firewall_rule_record>& rules,
                     std::string_view program, const listener& what,
                     long active_profiles);

/// Empty when the spec may be applied, otherwise why not.
std::string validate(const firewall_rule_spec& spec);

/// "Virtual Serial Studio TCP 4001". A rule with this name is ours, and the
/// removal step deletes by this name only.
std::string firewall_rule_name(const firewall_rule_spec& spec);

/// Empty when `spec.program` is one of our two programs (`vss_gui.exe`,
/// `vss_cli.exe`) in the same folder as `helper_path`, the elevated helper.
/// The helper refuses any other program, so the step cannot be used to open the
/// firewall for a program that is not Studio.
std::string program_is_ours(const firewall_rule_spec& spec,
                            std::string_view helper_path);

/// The group every rule of ours belongs to.
std::string_view firewall_rule_group() noexcept;

/// The command an administrator could run for the same rule, for the dialog to
/// show and copy.
std::string netsh_add_command(const firewall_rule_spec& spec);
std::string netsh_delete_command(const firewall_rule_spec& spec);

/// The arguments of `vss_cli` for the elevated step, and the reverse.
std::vector<std::string> elevated_arguments(const firewall_rule_spec& spec,
                                            bool remove);
struct parsed_step
{
    firewall_rule_spec spec;
    bool remove = false;
};
std::optional<parsed_step>
parse_elevated_arguments(const std::vector<std::string>& arguments);

/// The result of an elevated step.
enum class step_status
{
    done,
    /// The user declined the elevation prompt.
    declined,
    /// This platform or build cannot run the step.
    unsupported,
    /// Not elevated: the step must run with administrator rights.
    not_elevated,
    /// The spec failed `validate`.
    invalid,
    failed
};

struct step_result
{
    step_status status = step_status::failed;
    /// English text for the log, never shown as is.
    std::string detail;
    /// The exit code of the elevated program, when it ran to the end; -1
    /// otherwise.
    int exit_code = -1;
};

/// Applies or removes the rule. Needs administrator rights; call it only from
/// the elevated helper (`vss_cli permissions firewall-allow`).
step_result apply_firewall_rule(const firewall_rule_spec& spec, bool remove);

/// Starts `executable` with `arguments` elevated, through the operating
/// system's own prompt, waits up to `wait_seconds` and returns its exit code as
/// a status (and as `exit_code`). The user sees the prompt and may decline.
step_result run_elevated(const std::string& executable,
                         const std::vector<std::string>& arguments,
                         int wait_seconds = 60);

/// Exit codes of the elevated helper, so `run_elevated` can report them.
inline constexpr int exit_done = 0;
inline constexpr int exit_invalid = 4;
inline constexpr int exit_not_elevated = 3;
inline constexpr int exit_failed = 5;

/// True when this build can make and remove the firewall rule (Windows).
bool can_manage_firewall_rules() noexcept;

/// True when this build can start a helper elevated (Windows).
bool can_run_elevated() noexcept;

// ---------------------------------------------------------------------------
// Linux
// ---------------------------------------------------------------------------

/// Groups that may own a serial device on a Linux distribution: `dialout`
/// (Debian, Ubuntu, Fedora), `uucp` (Arch, Gentoo), `tty`, `lock`. The step
/// puts the user in one of these and no other.
bool serial_group_is_allowed(std::string_view group) noexcept;

/// Empty when the step may be asked for: a plain user name, an allowed group.
std::string validate(const group_membership_spec& spec);

/// `sudo usermod -aG dialout alice`: what the user could run themselves.
std::string usermod_command(const group_membership_spec& spec);

/// Puts the user in the group by asking `pkexec` (polkit) to run `usermod`. The
/// desktop shows its own authentication prompt, which names the command. The
/// user must sign out and in before the group takes effect.
step_result add_user_to_group(const group_membership_spec& spec);

/// True when this build can run the step (Linux) and polkit's `pkexec` and
/// `usermod` are installed.
bool can_add_user_to_group() noexcept;

/// Reads /proc/self/attr/current: `/usr/bin/foo (enforce)` and `unconfined` are
/// AppArmor, `unconfined_u:unconfined_r:unconfined_t:s0` is SELinux.
security_label parse_security_label(std::string_view text);

/// The file name of an AppArmor profile in /etc/apparmor.d: the profile path
/// without the leading slash, with dots for slashes (`/usr/bin/foo` is
/// `usr.bin.foo`). Empty for a name that is not a path (a snap profile).
std::string apparmor_profile_file(std::string_view profile);

/// The lines to add to the local override of an AppArmor profile so a serial
/// device, a pseudo terminal and Studio's link directory are allowed, with the
/// command that reloads the profile. For the user to read and copy.
std::string apparmor_snippet(std::string_view profile);

/// The commands that find an SELinux denial and build a policy module for it.
std::string selinux_commands();

/// The commands that open a port in ufw or firewalld.
std::string ufw_command(const listener& what);
std::string firewalld_command(const listener& what);

// ---------------------------------------------------------------------------
// macOS
// ---------------------------------------------------------------------------

/// Reads the output of `socketfilterfw --getglobalstate`: "Firewall is enabled.
/// (State = 1)", "... (State = 2)" (block all incoming connections) or
/// "Firewall is disabled. (State = 0)". Anything else leaves both unknown.
void parse_alf_global_state(std::string_view text, tri& on, tri& block_all);

/// Reads `socketfilterfw --getblockall`: "Block all DISABLED!" or "ENABLED!".
tri parse_alf_block_all(std::string_view text);

/// The major number in a Darwin kernel release such as "24.1.0"; zero if it is
/// not a number.
int darwin_major_from_release(std::string_view release);

/// The two commands that let a program accept incoming connections through the
/// Application Firewall. For the user to read and copy; Studio never runs them.
std::string socketfilterfw_commands(std::string_view program);

/// The entitlements an App Sandbox build needs for serial devices and for
/// network clients and servers, as the lines of an entitlements file.
std::string sandbox_entitlements();

/// Full path of this program.
std::string this_program();

} // namespace vss::permissions
