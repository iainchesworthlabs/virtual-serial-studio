//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test plugin as a plugin: it speaks the plugin API on its channel, for
// the tests of `vss::plugin::host::plugin_runner`.  What it does wrong, and
// when, is chosen by the modifiers that come before `--serve` on the command
// line.  main.cpp lists them.
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace test_plugin
{

/// What the modifiers before `--serve` ask for.  The defaults make a plugin
/// that behaves.
struct serve_options
{
    /// What its "hello" says of itself.
    std::string hello_id = "com.example.test";
    std::string hello_version = "1.2.0";
    /// Serves without ever saying "hello".
    bool no_hello = false;
    /// Says "hello" a second time at once.
    bool hello_twice = false;
    /// Exits with this code as soon as "hello_ack" arrives.
    std::optional<int> exit_after_ready;
    /// Ends the way a fault does as soon as "hello_ack" arrives.
    bool crash_after_ready = false;
    /// Goes on serving after "shutdown", and never says "bye".
    bool ignore_shutdown = false;
    /// Never answers "ping".
    bool ignore_ping = false;
    /// After "hello_ack": a "status" with the level warning and this text.
    std::optional<std::string> status;
    /// After "hello_ack": a "log" message with this text.
    std::optional<std::string> log;
    /// After "hello_ack": a request with this "op" and an empty body, whose
    /// answer is printed as "answer <op of the answer>".
    std::optional<std::string> send_op;
    /// A request with one of these "op"s is answered with "ok" and printed
    /// as "got <op>".
    std::vector<std::string> echo_ops;
    /// A request with one of these "op"s is answered with an "error" whose
    /// text is "refused by the test plugin", and printed as "got <op>".
    std::vector<std::string> refuse_ops;
    /// After "hello_ack": 64 bytes that are not a frame.
    bool garbage_after_ready = false;
    /// After "hello_ack": this many "log" messages, as fast as possible.
    std::optional<std::uint32_t> flood_after_ready;
    /// After "hello_ack": this text, once, on standard error.
    std::optional<std::string> stderr_each;
};

/// Applies the argument when it is one of the modifiers.  False when it is
/// not one, or when its value is not valid.
bool take_serve_modifier(std::string_view argument, serve_options& options);

/// Serves on the channel until it ends or "shutdown" is answered.  The exit
/// code of the program: 0 after "bye", 2 when the channel ended, 3 when
/// there is no channel, 4 when a message could not be written.
int serve(const serve_options& options);

} // namespace test_plugin
