// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The table of @c IoBackend kinds a test should run against.
///
/// Shared rather than copied per test file for the reason the parity suite exists at
/// all: `Socket_test` hardcoded one backend in every case, so two native-backend
/// defects — a registration holding the peer's connection open, and a parked flow
/// never resuming after close — passed a green suite. One table means adding a
/// backend enrols every suite that uses it, instead of enrolling whichever ones
/// someone remembers.

#include <core/net/IoBackend.hpp>

#include <array>
#include <string_view>

namespace core::net::testing
{

/// One backend to exercise, with a label so a failure names which one broke.
struct BackendUnderTest
{
    BackendKind kind;      ///< The backend to construct.
    std::string_view name; ///< Its name, for the section label.
};

/// Every backend @c makeBackend can build. A test enumerates these, skips the ones it
/// reports unavailable on this platform, and runs the same scenario against the rest:
///
/// @code
/// for (auto const& entry: net::testing::BackendMatrix)
/// {
///     auto backend = net::makeBackend(entry.kind);
///     if (!backend)
///         continue; // not built on this platform
///     DYNAMIC_SECTION("backend=" << entry.name) { ... }
/// }
/// @endcode
///
/// @c BackendKind::Scripted, @c BackendKind::Null and @c BackendKind::HostDriven are
/// deliberately absent: the first two are test doubles a case constructs directly,
/// and the third needs the @c IHostScheduler its host provides, so none of them comes
/// from @c makeBackend. `HostDrivenBackend_test` drives the third over
/// @c testing::ManualHostScheduler instead, on every platform.
constexpr auto BackendMatrix = std::array {
    BackendUnderTest { BackendKind::Poll, "poll" },
    BackendUnderTest { BackendKind::Epoll, "epoll" },
    BackendUnderTest { BackendKind::Kqueue, "kqueue" },
    BackendUnderTest { BackendKind::Iocp, "iocp" },
};

} // namespace core::net::testing
