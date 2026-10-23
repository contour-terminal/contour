// SPDX-License-Identifier: Apache-2.0
//
// Test-only: what a lane of a scheduler reports about its thread's preparation to call into instances.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <future>

#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/testing/PreparedGeneration.hpp>

namespace vtextension::testing
{

/// How long a test waits for something a lane must do, far beyond any scheduler's latency: the bound
/// proves only that it happened at all.
inline constexpr auto Eventually = std::chrono::seconds { 30 };

/// What a lane's thread is prepared for, as seen from a job running on it.
struct LaneReport
{
    /// The generation the lane's guard was entered for: the one fact the neutral interface does not
    /// tell, and what shows that the guard was renewed rather than merely that the thread is usable.
    uint64_t prepared = 0;
    bool ready = false; ///< Whether the lane may call into instances, by the backend's own predicate.
};

/// Posts a job to @p lane that reports on its thread, and waits for it.
/// @param scheduler Where the lane is.
/// @param lane The lane to report on.
/// @param backend Whose predicate tells whether the lane is prepared.
/// @return What the lane reported; the test fails if it does not within Eventually.
[[nodiscard]] inline LaneReport reportFromLane(IExtensionScheduler& scheduler,
                                               LaneId lane,
                                               wasm::IWasmBackend const& backend)
{
    auto report = std::promise<LaneReport> {};
    auto future = report.get_future();
    scheduler.post(lane, [&report, &backend] {
        report.set_value(LaneReport { .prepared = wasm::testing::preparedGeneration(),
                                      .ready = backend.isCurrentThreadPrepared() });
    });
    REQUIRE(future.wait_for(Eventually) == std::future_status::ready);
    return future.get();
}

} // namespace vtextension::testing
