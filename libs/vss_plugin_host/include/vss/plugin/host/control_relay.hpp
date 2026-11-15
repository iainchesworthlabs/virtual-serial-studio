//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The control relay: how one plugin, the remote control and monitoring
// plugin, uses the control gateway (docs/remote-control.md) without ever
// holding a token or opening a connection.  docs/plugins.md, section 7.7,
// says why.
//
// A confined plugin cannot reach this computer's loopback addresses on
// Windows.  A TCP port is open to every local user, while the gateway's
// local endpoint (a named pipe or a Unix socket) is this user's alone.  And
// a token that the plugin never sees cannot leak from it.  So the host does
// the parts that need a secret:
//
// 1. The plugin asks (`control.session.request`).  The owner is asked to
//    approve the scopes, and nothing is made before the answer is yes.
// 2. The host connects to the gateway's administration endpoint as this
//    user and makes a token with exactly the approved scopes that lives for
//    the time in the options and is good on the local endpoint alone.
// 3. The host connects to the gateway's local endpoint and says `hello`
//    with that token.  Only when the gateway answers as that token does the
//    session open.  The token is wiped once the `hello` is written.
// 4. The plugin's frames (`control.frame`) go to the gateway unchanged, and
//    the gateway's frames come back unchanged.
//
// The one rule that matters: a frame of the plugin is only ever written to
// a connection whose `hello` carried the plugin's own token and whose answer
// said it is that token.  A `hello` without a token would be this user's
// with the local role, so no connection is ever used that was not made so.
// A frame whose operation is `hello` is refused.
//
// Every session has a token of its own, and the host revokes it when the
// session ends for any reason (a close, the gateway going away, the plugin
// stopping), so a plugin that has gone leaves no usable token behind.
//
// The gateway's transport is used as the gateway's own client uses it
// (`connect_local`, the agent's frame reader and writer, the messages of
// `vss_control_proto`).  `vss::control::client` is not used: each of its
// calls blocks the calling thread, and the relay lives on a strand.
//
// Threads.  A relay lives on the executor it is given, which must be a
// strand (or an executor of one thread).  Every public function and every
// handler runs on it, and no function waits for the gateway.  The one thing
// that may take time on the strand is `connect_local` itself: on Windows it
// waits up to five seconds for a pipe whose instances are all taken, and
// the gateway only lets that happen when it is overloaded.
//

#pragma once

#include <vss/core/bytes.hpp>

#include <boost/asio/any_io_executor.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vss::plugin::host
{

// -- the messages between the plugin and the host ----------------------------
//
// The relay is given these already decoded, and hands back what it answers
// with.  The part of the plugin API that reads and writes them is the
// caller's.

/// `control.session.request`, plugin to host, a request.  The answer is a
/// `control_session_opened` (`ok`, with `session`, `scopes` granted and
/// `expires`) or a `control_session_refused` (an error).
struct control_session_request
{
    /// The names of the scopes the plugin asks for: `monitor`,
    /// `monitor.data`, `control.ports`, `control.data`, `control.faults`,
    /// `control.apply` and `record`.  Never `admin`.
    std::vector<std::string> scopes;
    /// Who the plugin says is behind this session, such as the name of a
    /// remote computer.  Untrusted text, at most
    /// `max_control_peer_size` bytes.
    std::string peer;
    /// Why the plugin asks, at most `max_control_reason_size` bytes.
    std::string reason;
};

/// The answer `ok` to `control.session.request`.
struct control_session_opened
{
    /// Names the session in `control.frame`, `control.session.close` and
    /// `control.session.closed`.  Never 0, never used twice by one relay.
    std::uint64_t session = 0;
    /// The scopes the gateway gave the session: the approved ones, less
    /// what the gateway's own ceiling for the local endpoint takes away.
    std::vector<std::string> scopes;
    /// When the session's token ends, in milliseconds since 1970-01-01 UTC.
    /// The gateway closes the session then, and the plugin hears `closed`.
    std::uint64_t expires_ms = 0;
};

/// Why a request was refused.  The plugin API has two errors for it,
/// `permission_denied` and `plugin_unavailable`; the other two are for the
/// caller to map as it sees fit (`invalid_argument` and `busy` suggest
/// themselves).
enum class control_refusal
{
    /// The request is malformed: an unknown scope name, no scope, a text
    /// that is too long.
    invalid_request,
    /// `admin` was asked for, the owner said no, or nobody could be asked.
    permission_denied,
    /// A request is open already, or the plugin has as many sessions as it
    /// may.
    busy,
    /// No gateway runs, or it did not do what was asked in time.
    plugin_unavailable
};

/// The error answer to `control.session.request`.
struct control_session_refused
{
    control_refusal why = control_refusal::plugin_unavailable;
    /// Plain text for a person.  Never holds a token.
    std::string text;
};

using control_session_answer =
    std::variant<control_session_opened, control_session_refused>;

// `control.frame` is not a struct: it is the pair of arguments of
// `control_relay::frame_from_plugin` and of the `to_plugin` handler: the
// session, the gateway header as JSON text, and the bytes of the blob.
//
// `control.session.close` is `control_relay::close_session`, and
// `control.session.closed` is the `closed` handler: the session, a
// `control_close_code` and a plain reason.

/// Why a session ended, for `control.session.closed`.  A plugin can act on
/// the code; the reason that goes with it is for a person.
enum class control_close_code
{
    /// The gateway answered `unauthenticated` on the open session, before
    /// the token's time was up: the token was taken back.
    revoked,
    /// The token's time has passed: the gateway said `unauthenticated`, or
    /// closed the connection, after it.
    expired,
    /// The connection to the gateway ended for any other reason.
    gateway_closed,
    /// The relay closed the session because of a frame it refused: one
    /// that is not a gateway header, that says `hello`, that is too long,
    /// that is for a session that is not open, or too many waiting to be
    /// written.
    refused_frame,
    /// The host closed the session: `close_session` or `close_all`.
    host_stopped
};

/// The name of a code as it is written in the message: `revoked`,
/// `expired`, `gateway_closed`, `refused_frame` or `host_stopped`.
std::string_view to_string(control_close_code code) noexcept;

// -- limits and options ------------------------------------------------------

inline constexpr std::size_t max_control_peer_size = 64;
inline constexpr std::size_t max_control_reason_size = 256;
/// The most sessions one plugin has at a time.
inline constexpr std::size_t max_control_sessions = 4;

/// What the owner is asked to approve.  `scopes` are exactly the scopes
/// that will be asked of the gateway: valid names, each once, in the order
/// of the scope list.
struct control_approval_request
{
    std::string plugin_id;
    std::string plugin_name;
    /// The plugin's text for who is behind this, cleaned: no control
    /// characters, valid UTF-8, at most `max_control_peer_size` bytes.
    std::string peer;
    /// The plugin's reason, cleaned in the same way, at most
    /// `max_control_reason_size` bytes.  Still the plugin's words.
    std::string reason;
    std::vector<std::string> scopes;
    /// How long the token lives.
    std::chrono::seconds token_life{0};
};

struct control_relay_options
{
    /// Whose relay this is.  The id goes into the label of each token and
    /// into the gateway's audit log.
    std::string plugin_id;
    std::string plugin_name;
    /// The instance name of the gateway; empty for this user's ordinary
    /// one.  A test gives its own gateway another name.
    std::string instance;
    /// How long a session's token lives.  The gateway takes at most a year.
    std::chrono::seconds token_life{std::chrono::hours{24}};
    /// How long the gateway has to make a token, and to answer `hello`.
    std::chrono::milliseconds step_timeout{10000};
    /// How long revoking tokens may take in all.  Past it the token is left
    /// to expire and a line says so.
    std::chrono::milliseconds revoke_timeout{3000};
    /// The most sessions at one time; never more than
    /// `max_control_sessions`.
    std::size_t max_sessions = max_control_sessions;
    /// The most bytes that may wait to be written to one gateway
    /// connection.  One byte more and the session is closed instead of
    /// growing.
    std::size_t max_waiting_bytes = std::size_t{4} * 1024 * 1024;
};

/// What the relay calls.  All run on the relay's strand and must not block.
struct control_relay_handlers
{
    /// Asks the owner whether the plugin may have a session with these
    /// scopes.  The second argument is called with the answer, once, from
    /// any thread; a second call does nothing.  Without a handler no
    /// request is approved.
    std::function<void(const control_approval_request&,
                       std::function<void(bool)>)>
        approve;

    /// A frame of the gateway, unchanged, for the plugin: `control.frame`
    /// host to plugin.  The arguments are valid for the call only.  The
    /// function returns true when it took the frame (it copied what it
    /// needs) and has room for more.  It returns false when it did not
    /// take the frame because the plugin is slow and the caller's queue is
    /// full: the relay then holds that one frame, reads nothing more from
    /// that gateway connection (so the gateway's own back-pressure
    /// applies), and offers the frame again when `plugin_has_room` is
    /// called.  What waits for a slow plugin is therefore one frame and one
    /// read of 64 KiB for each session.
    std::function<bool(std::uint64_t session, std::string_view header,
                       bytes_view blob)>
        to_plugin;

    /// The session ended: `control.session.closed`.  `reason` is plain text
    /// and never holds a token.  Called for every end of an open session,
    /// also for `close_session` and `close_all` (`host_stopped`), but not
    /// from the relay's destructor.
    std::function<void(std::uint64_t session, control_close_code code,
                       std::string_view reason)>
        closed;

    /// One line for the host's log, without a line ending.  Never holds a
    /// token.  May be empty.
    std::function<void(std::string_view line)> log;
};

/// `text` as safe to show a person: bytes that are not valid UTF-8 are
/// dropped, and so are control characters, the line and paragraph
/// separators, the zero width and direction controls, and the byte order
/// mark.  Spaces at the ends are cut, and the result is at most
/// `most_bytes` bytes without cutting a character.
std::string clean_control_text(std::string_view text, std::size_t most_bytes);

class control_relay
{
public:
    using answer_handler = std::function<void(control_session_answer)>;

    /// A relay with no session.  `executor` is the plugin's strand.
    static std::shared_ptr<control_relay>
    create(const boost::asio::any_io_executor& executor,
           control_relay_options options, control_relay_handlers handlers);

    /// Closes what is open and starts revoking its tokens, with no more
    /// calls to the handlers.  Call `close_all` first to know when the
    /// tokens are revoked.
    ~control_relay();
    control_relay(const control_relay&) = delete;
    control_relay& operator=(const control_relay&) = delete;

    /// `control.session.request`.  Checks the request, asks the owner,
    /// makes a token, opens the connection and says `hello`; then calls
    /// `done` once, on the strand and never before this function has
    /// returned.  The checks are: no `admin`, only scope names the control
    /// API has, no scope that is not asked for, the sizes of the texts, at
    /// most one request open at a time, and at most `max_sessions`
    /// sessions.  Nothing is made before the owner has approved.  Nothing
    /// is retried.
    void request_session(const control_session_request& request,
                         answer_handler done);

    /// `control.frame`, plugin to host.  The frame is the plugin's: a JSON
    /// object that is a header of the gateway's protocol, and its blob.  It
    /// is written to the gateway as it is.  A frame that is not a JSON
    /// object, that nests deeper than the gateway reads, that is longer
    /// than a frame may be, or that asks for `hello`, and a frame for a
    /// session that is not open, are refused: the session is closed and
    /// the `closed` handler says why.  The same when more than
    /// `max_waiting_bytes` wait to be written.
    void frame_from_plugin(std::uint64_t session, std::string header,
                           byte_buffer blob);

    /// The caller has room again after `to_plugin` returned false.
    void plugin_has_room(std::uint64_t session);

    /// `control.session.close`.  Closes the connection and revokes the
    /// token; `closed` hears `host_stopped`.  Nothing happens for a session
    /// that is not open.
    void close_session(std::uint64_t session);

    /// For when the plugin stops or fails: cancels a request that is open
    /// (its `done` hears `plugin_unavailable`), closes every connection
    /// and revokes every token on the administration endpoint, all in one
    /// connection.  `done` is called when the gateway has answered or when
    /// `revoke_timeout` has passed, and after the revoking that the closing
    /// of single sessions began earlier (the relay makes one call on the
    /// administration endpoint at a time, as the gateway has room for two
    /// connections there).  A token that was being made while this ran is
    /// revoked as soon as the gateway has made it, and `done` does not wait
    /// for that.  `closed` hears `host_stopped` for each open session.  The
    /// relay can be used again afterwards.
    void close_all(std::function<void()> done = {});

    /// How many sessions are open or opening.
    std::size_t session_count() const noexcept;

    class impl;

private:
    explicit control_relay(std::shared_ptr<impl> state);
    std::shared_ptr<impl> impl_;
};

} // namespace vss::plugin::host
