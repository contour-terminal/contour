// SPDX-License-Identifier: Apache-2.0
//
// What a daemon-world extension may cost: the world's own values where they differ from the status line's.
#pragma once

#include <chrono>
#include <cstddef>

#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionStore.hpp>

namespace vtextension
{

/// The shortest delay `wake` arms: a guest asking for less is woken after this.
inline constexpr auto MinWakeInterval = std::chrono::milliseconds { 16 };

/// The most characters (code points) of `summary-set`'s one-line summary.
inline constexpr std::size_t MaxSummaryShortChars = 120;

/// The most bytes of `summary-set`'s detail, as the guest publishes it: the cap is on the input.
inline constexpr std::size_t MaxSummaryDetailBytes = 16 << 10;

/// How many times its input a neutralized text may grow at most: a one-byte control character is shown as
/// the eight bytes `<U+XXXX>`, and nothing grows more.
inline constexpr std::size_t MaxNeutralizedGrowth = 8;

/// The most bytes of a detail as it is shown, neutralized: 128 KiB.
inline constexpr std::size_t MaxSummaryDetailShownBytes = MaxSummaryDetailBytes * MaxNeutralizedGrowth;

/// The bound on durable host calls (`store-set`, `store-remove`): they pause the guest's call deadline, which
/// bounds the guest's own work and not the disk, so this bounds them instead. One guest callback (`load`,
/// `update`, a reactor's `_initialize`) may make at most `maxCalls` of them and spend at most `maxTime` in
/// them; past either, a durable call answers `limit` without writing, and the next callback starts afresh.
struct DurableBudget
{
    std::size_t maxCalls = 64;                  ///< Durable calls one callback may make.
    std::chrono::milliseconds maxTime { 2000 }; ///< Time one callback may spend in them.
};

/// How many times slower the WebAssembly interpreter runs a guest in this build than in a clang or GCC one.
/// What compiled WAMR decides, not what compiled the host: an MSVC build has WAMR compiled by clang-cl where
/// one is found, and the build defines CONTOUR_WAMR_SLOW_INTERPRETER only where cl compiled it. cl cannot use
/// WAMR's labels-as-values dispatch and optimizes its switch dispatch poorly: the orchestrator's full
/// repaint, about 8 ms in a release build with WAMR compiled by clang-cl, takes about 290 ms with WAMR
/// compiled by cl, past the 200 ms deadline, and the daemon then stops the extension after repeated faults.
#if defined(CONTOUR_WAMR_SLOW_INTERPRETER)
inline constexpr auto InterpreterSlowdown = 5;
#else
inline constexpr auto InterpreterSlowdown = 1;
#endif

/// @return The daemon world's limits: the status line's, but with a call deadline of 200 ms of CPU time and a
///         load deadline of 500 ms (both times InterpreterSlowdown), each allowed ten times as long in real
///         time while the daemon's machine is busy (the agents an extension starts compile on it), and a
///         per-call argument budget that fits one full store value under its longest key (64 KiB + 128 B),
///         so that the store's own limits are what a guest reaches first.
[[nodiscard]] constexpr ExtensionLimits daemonWorldLimits() noexcept
{
    auto limits = ExtensionLimits {};
    limits.callDeadline = std::chrono::milliseconds { 200 } * InterpreterSlowdown;
    limits.loadDeadline = std::chrono::milliseconds { 500 } * InterpreterSlowdown;
    limits.deadlineRealTimeFactor = 10;
    limits.maxHostCallBytes = StoreLimits {}.maxValueBytes + StoreLimits {}.maxKeyBytes;
    return limits;
}

static_assert(daemonWorldLimits().maxHostCallBytes == 65'664, "one 64 KiB value under a 128-byte key");

} // namespace vtextension
