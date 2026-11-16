//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What this build can and cannot install of a compiled rule set.  A pure
// function of the set: no studio, no port, no clock.  `studio::install_rules`
// and the entry points that name a rule-set file (the setup loader, `vss_cli
// rules check`) ask the same code, so they refuse the same things in the same
// words.
//

#pragma once

#include <vss/rules/compile.hpp>
#include <vss/rules/model.hpp>

#include <vector>

namespace vss
{

/// What this build cannot install, whatever the port is.  Appends to
/// `problems`, each with the path of the value (`attach.role`,
/// `rules[2].when.lines`, `rules[0].do[1].precision`) and in the order of the
/// document: the role, then the rules, then the timeline.  Every problem is
/// found, not only the first.
///
/// Refused: a role other than `peer`; a rule on `lines` or `port`; an `ask`;
/// a `lines` action on a line a device does not drive (DTR, RTS, BREAK); a
/// `lines` action that sets RI, which does not reach a program in this build;
/// an action of `"precision": "tight"`, for which this build has no timing
/// thread.
///
/// `line_errors_allowed` is whether the port the set will go on takes line
/// errors (a fixture and a port of the Windows driver do).  When false a
/// `line_error` action is a problem too.  A caller that does not know the
/// port leaves it true, and the check of the port is made when the set is
/// installed.
void check_rules_for_build(const rules::compiled_rule_set& set,
                           std::vector<rules::problem>& problems,
                           bool line_errors_allowed = true);

/// The same, for a set that a setup file names: also what no entry point of
/// this build can drive.  A set that declares `signals` is a problem, because
/// nothing in this build can raise one (the library call `install_rules`
/// still accepts such a set).
void check_rules_for_setup(const rules::compiled_rule_set& set,
                           std::vector<rules::problem>& problems);

} // namespace vss
