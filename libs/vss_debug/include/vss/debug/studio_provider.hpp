//
// Copyright (c) 2026 Virtual Serial Studio contributors
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#pragma once

#include <vss/debug/provider.hpp>
#include <vss/studio/studio.hpp>
#include <vss/trace/recorder.hpp>

namespace vss::debug
{

/// The part of a provider that needs only a studio: the engine snapshot, the
/// COM number picture, the driver state, the traffic ring and the closed list
/// of operations.  The window's own provider derives from it and adds the
/// window.
///
/// The studio must outlive the provider.  Every call is safe from any thread,
/// as the studio's own are.
class studio_provider : public provider
{
public:
    explicit studio_provider(studio& target,
                             trace::recorder* recorder = nullptr);

    op_result snapshot() override;
    op_result com_numbers() override;
    op_result driver() override;
    op_result traffic(const std::string& args_json) override;
    op_result run(const std::string& args_json) override;

    /// The snapshot as JSON text, for the support bundle as well.
    std::string snapshot_json() const;

protected:
    studio& engine() noexcept { return *studio_; }
    trace::recorder& recorder() noexcept { return *recorder_; }

private:
    studio* studio_;
    trace::recorder* recorder_;
};

} // namespace vss::debug
