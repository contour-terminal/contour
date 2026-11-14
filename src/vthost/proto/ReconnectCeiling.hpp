// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one number a client's reconnect backoff and a daemon's reconnect grace agree on.

#include <chrono>

namespace vthost::proto
{

/// The longest wait between two reconnect attempts of a client (client::BackoffSchedule::ceiling), and so
/// how long a daemon takes a window whose connection dropped to still be reconnecting
/// (SessionHostConfig::reconnectGrace): by then the window has tried again, or is gone.
inline constexpr auto ReconnectCeiling = std::chrono::milliseconds { 5000 };

} // namespace vthost::proto
