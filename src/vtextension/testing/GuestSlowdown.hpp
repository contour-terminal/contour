// SPDX-License-Identifier: Apache-2.0
//
// Test-only: the guest's deadlines, stretched in a ThreadSanitizer build.
#pragma once

#include <vtextension/DaemonLimits.hpp>
#include <vtextension/ExtensionLimits.hpp>

// Whether this translation unit is built with ThreadSanitizer: GCC says so with __SANITIZE_THREAD__, clang
// with __has_feature(thread_sanitizer). Local to this header; undefined right below.
#ifdef __SANITIZE_THREAD__
    #define VTEXTENSION_GUEST_SLOWDOWN_TSAN
#endif
#ifdef __has_feature
    #if __has_feature(thread_sanitizer)
        #define VTEXTENSION_GUEST_SLOWDOWN_TSAN
    #endif
#endif

namespace vtextension::testing
{

/// How many times longer the guest's deadlines are in this build than the daemon's own. ThreadSanitizer runs
/// code 5 to 15 times slower (its documentation) -- the WebAssembly interpreter included, which runs the
/// guest -- so a guest that keeps to the daemon's 200 ms in a release build overruns it there, and is
/// disabled: the upper end of that range. AddressSanitizer runs it within the daemon's own limits, and keeps
/// them.
#ifdef VTEXTENSION_GUEST_SLOWDOWN_TSAN
inline constexpr auto GuestSlowdown = 15;
#else
inline constexpr auto GuestSlowdown = 1;
#endif
#undef VTEXTENSION_GUEST_SLOWDOWN_TSAN

/// @return The daemon world's limits, their call and load deadlines stretched by GuestSlowdown.
[[nodiscard]] constexpr ExtensionLimits guestTestLimits() noexcept
{
    auto limits = daemonWorldLimits();
    limits.callDeadline *= GuestSlowdown;
    limits.loadDeadline *= GuestSlowdown;
    return limits;
}

} // namespace vtextension::testing
