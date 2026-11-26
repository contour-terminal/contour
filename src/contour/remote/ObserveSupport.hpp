// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace contour::remote
{

/// Whether the daemon of a controller can be observed (vthost::proto::ConnectionRole::Observe).
enum class ObserveSupport : uint8_t
{
    Unknown = 0, ///< No connection was answered yet, or none asked to observe.
    Supported,   ///< The daemon shares vthost::proto::Capability::Observe.
    /// An Observe hello was answered by a daemon that does not offer the capability (it read the role as
    /// Control): the controller stopped, and does not redial.
    TooOld,
};

} // namespace contour::remote
