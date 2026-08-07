// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The table of @c EventSource backends a test should run against.
///
/// Shared rather than copied per test file for the reason the parity suite exists
/// at all: `Socket_test` hardcoded @c PollEventSource in every case, so two
/// native-backend defects — a registration holding the peer's connection open, and
/// a parked flow never resuming after close — passed a green suite. One table
/// means adding a backend enrols every suite that uses it, instead of enrolling
/// whichever ones someone remembers.

#include <array>
#include <string_view>

#include <net/DefaultEventSource.hpp>

namespace net::testing
{

/// One backend to exercise, with a label so a failure names which one broke.
struct Backend
{
    EventSourceKind kind;  ///< The backend to construct.
    std::string_view name; ///< Its name, for the section label.
};

/// Every backend the interface defines. A test enumerates these, skips the ones
/// @c makeEventSource reports unavailable on this platform, and runs the same
/// scenario against the rest:
///
/// @code
/// for (auto const& backend: net::testing::AllBackends)
/// {
///     auto source = net::makeEventSource(backend.kind);
///     if (!source)
///         continue; // not available on this platform
///     DYNAMIC_SECTION("backend=" << backend.name) { ... }
/// }
/// @endcode
constexpr auto AllBackends = std::array {
    Backend { EventSourceKind::Poll, "poll" },
    Backend { EventSourceKind::Epoll, "epoll" },
    Backend { EventSourceKind::Kqueue, "kqueue" },
};

} // namespace net::testing
