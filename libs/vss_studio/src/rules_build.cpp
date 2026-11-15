//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <vss/core/control_lines.hpp>
#include <vss/studio/rules_build.hpp>

#include <cstddef>
#include <string>
#include <utility>

namespace vss
{

namespace
{

class build_check
{
public:
    build_check(const rules::compiled_rule_set& set,
                std::vector<rules::problem>& problems, bool line_errors_allowed)
        : set_(set), problems_(problems),
          line_errors_allowed_(line_errors_allowed)
    {
    }

    void run()
    {
        if (set_.attach.as != rules::role::peer)
            bad("attach.role",
                "only a \"peer\" set can be installed in this build");
        for (std::size_t i = 0; i < set_.rules.size(); ++i)
        {
            const rules::compiled_rule& rule = set_.rules[i];
            const std::string path = "rules[" + std::to_string(i) + "]";
            switch (rule.kind)
            {
            case rules::trigger_kind::bytes:
            case rules::trigger_kind::quiet:
            case rules::trigger_kind::signal:
                break;
            case rules::trigger_kind::lines:
                bad(path + ".when.lines",
                    "a rule on \"lines\" cannot be installed in this build");
                break;
            case rules::trigger_kind::port:
                bad(path + ".when.port",
                    "a rule on \"port\" cannot be installed in this build");
                break;
            }
            // The path the writer of the document will find the action at.
            for (std::size_t s = 0; s < rule.sequence.size(); ++s)
                actions(rule.sequence[s].actions,
                        rule.written_as_do
                            ? path + ".do"
                            : path + ".sequence[" + std::to_string(s) + "].do");
        }
        for (std::size_t i = 0; i < set_.timeline.size(); ++i)
            actions(set_.timeline[i].actions,
                    "timeline[" + std::to_string(i) + "].do");
    }

private:
    void bad(std::string path, std::string message)
    {
        problems_.push_back(
            rules::problem{std::move(path), std::move(message)});
    }

    void actions(const std::vector<rules::compiled_action>& list,
                 const std::string& path)
    {
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            const rules::compiled_action& action = list[i];
            const std::string here = path + "[" + std::to_string(i) + "]";
            switch (action.kind)
            {
            case rules::action_kind::ask:
                bad(here, "\"ask\" cannot be installed in this build");
                break;
            case rules::action_kind::raise_error:
                if (!line_errors_allowed_)
                    bad(here + ".line_error",
                        "the port does not take line errors: only a fixture "
                        "and a port of the Windows driver do");
                break;
            case rules::action_kind::set_lines:
                for (const rules::line_value& line : action.lines)
                {
                    if (line.which == control_line::ri)
                        bad(here + ".lines",
                            "RI cannot be driven in this build");
                    else if ((line_mask(line.which) &
                              control_lines::input_bits()) == 0)
                        bad(here + ".lines",
                            "a rule sets the lines a device drives: CTS, "
                            "DSR and DCD");
                }
                break;
            case rules::action_kind::reply:
            case rules::action_kind::pass:
            case rules::action_kind::set_state:
            case rules::action_kind::set_slot:
            case rules::action_kind::mark:
                break;
            }
            if (action.when.how == rules::precision::tight)
                bad(here + ".precision",
                    "\"tight\" precision cannot be installed in this build");
        }
    }

    const rules::compiled_rule_set& set_;
    std::vector<rules::problem>& problems_;
    const bool line_errors_allowed_;
};

} // namespace

void check_rules_for_build(const rules::compiled_rule_set& set,
                           std::vector<rules::problem>& problems,
                           bool line_errors_allowed)
{
    build_check(set, problems, line_errors_allowed).run();
}

void check_rules_for_setup(const rules::compiled_rule_set& set,
                           std::vector<rules::problem>& problems)
{
    check_rules_for_build(set, problems);
    if (!set.signals.empty())
        problems.push_back(rules::problem{
            "signals", "nothing in this build can raise a signal, so a set "
                       "that declares one is not installed from a setup "
                       "file"});
}

} // namespace vss
