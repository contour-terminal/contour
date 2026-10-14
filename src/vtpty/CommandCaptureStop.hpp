// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/async/StopToken.hpp>

namespace vtpty
{

/// The two reasons a captured run is cancelled: its caller's request and the capture's shutdown.
struct RunStop
{
    core::async::StopToken request;  ///< The caller's per-run stop request.
    core::async::StopToken shutdown; ///< The capture-wide shutdown.

    /// @return Whether either token has been stopped.
    [[nodiscard]] bool requested() const noexcept
    {
        return request.stop_requested() || shutdown.stop_requested();
    }
};

} // namespace vtpty
