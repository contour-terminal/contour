// SPDX-License-Identifier: Apache-2.0
//
// What the supervisor's backstop sees of each lane: which frames run there, which of them an interrupt armed,
// and whether the innermost one waits on the parent.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>

#include <vtextension/wasm/remote/LaneWatch.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

using vtextension::wasm::remote::LaneArming;
using vtextension::wasm::remote::LaneObservation;
using vtextension::wasm::remote::LanePlace;
using vtextension::wasm::remote::LaneWatch;
using vtextension::wasm::remote::LaneWatchProbe;
using vtextension::wasm::remote::LaneWatchSet;
using vtextension::wasm::remote::MaxLaneChannels;

namespace proto = vtextension::wasm::remote::proto;

TEST_CASE("LaneWatch.interruptArmsOnlyAFrameOnTheLane", "[vtextension]")
{
    auto watch = LaneWatch {};
    CHECK(watch.observe().place == LanePlace::Idle);
    watch.interrupt(7); // nothing runs
    CHECK(watch.observe().arming == LaneArming::Disarmed);

    watch.enter(7);
    watch.interrupt(8);
    CHECK(watch.observe().arming == LaneArming::Disarmed);
    CHECK(watch.observe().place == LanePlace::InHelper);

    watch.interrupt(7);
    auto const armed = watch.observe();
    CHECK(armed.arming == LaneArming::Armed);
    CHECK(armed.place == LanePlace::InHelper);

    // Interrupted again, the same frame stays armed: the backstop's grace runs on.
    watch.interrupt(7);
    CHECK(watch.observe().epoch == armed.epoch);
}

TEST_CASE("LaneWatch.leavingTheInterruptedFrameDisarms", "[vtextension]")
{
    auto watch = LaneWatch {};
    watch.enter(7);
    watch.enter(9);
    watch.interrupt(9);
    auto const armed = watch.observe();
    CHECK(armed.arming == LaneArming::Armed);

    watch.leave();
    auto const left = watch.observe();
    CHECK(left.arming == LaneArming::Disarmed);
    CHECK(left.epoch != armed.epoch);
    CHECK(left.place == LanePlace::InHelper);

    watch.interrupt(7);
    auto const again = watch.observe();
    CHECK(again.arming == LaneArming::Armed);
    CHECK(again.epoch != armed.epoch);
    CHECK(again.epoch != left.epoch);

    watch.leave();
    CHECK(watch.observe().arming == LaneArming::Disarmed);
    CHECK(watch.observe().place == LanePlace::Idle);
}

TEST_CASE("LaneWatch.frameEnteredAfterItsInstanceLeftIsNotArmed", "[vtextension]")
{
    // An interrupt arms the frame it found, not a later one of the same instance.
    auto watch = LaneWatch {};
    watch.enter(7);
    watch.interrupt(7);
    CHECK(watch.observe().arming == LaneArming::Armed);
    watch.leave();
    watch.enter(7);
    CHECK(watch.observe().arming == LaneArming::Disarmed);
}

namespace
{
/// A lane thread racing an interrupt: what it does to the lane between the interrupt's two reads of a frame,
/// once.
struct Race
{
    std::function<void(LaneWatch&)> change;
    LaneWatch* watch = nullptr;
    bool isRun = false;

    /// @return The probe that runs the change.
    [[nodiscard]] LaneWatchProbe probe() noexcept
    {
        return LaneWatchProbe { .betweenReads =
                                    [](void* context) noexcept {
                                        auto& race = *static_cast<Race*>(context);
                                        if (std::exchange(race.isRun, true))
                                            return;
                                        race.change(*race.watch);
                                    },
                                .context = this };
    }
};
} // namespace

TEST_CASE("LaneWatch.interruptNeverArmsAnotherInstancesFrame", "[vtextension]")
{
    // Instance 7's frame is left, and 9's entered in its place, while the watchdog reads the frame: the
    // healthy call of 9 is not armed, whatever the watchdog read of 7.
    auto race = Race { .change =
                           [](LaneWatch& lane) {
                               lane.leave();
                               lane.enter(9);
                           },
                       .watch = nullptr,
                       .isRun = false };
    auto watch = LaneWatch { race.probe() };
    race.watch = &watch;
    watch.enter(7);
    watch.interrupt(7);
    CHECK(race.isRun);
    CHECK(watch.observe().arming == LaneArming::Disarmed);
}

TEST_CASE("LaneWatch.staleInterruptKeepsANewerArming", "[vtextension]")
{
    // One watchdog reads 7's frame; meanwhile 7 calls again in its place, and another interrupt arms that
    // call. The first, done reading, must not put the arming back to the frame that is gone.
    auto race = Race { .change =
                           [](LaneWatch& lane) {
                               lane.leave();
                               lane.enter(7);
                               lane.interrupt(7);
                           },
                       .watch = nullptr,
                       .isRun = false };
    auto watch = LaneWatch { race.probe() };
    race.watch = &watch;
    watch.enter(7);
    watch.interrupt(7);
    CHECK(race.isRun);
    CHECK(watch.observe().arming == LaneArming::Armed);
}

TEST_CASE("LaneWatch.hostCallParksTheInnermostFrame", "[vtextension]")
{
    auto watch = LaneWatch {};
    watch.enter(7);
    watch.beginHostCall();
    CHECK(watch.observe().place == LanePlace::ServingHost);

    // The parent calls back in: the helper runs again.
    watch.enter(9);
    CHECK(watch.observe().place == LanePlace::InHelper);
    watch.leave();
    CHECK(watch.observe().place == LanePlace::ServingHost);

    watch.endHostCall();
    CHECK(watch.observe().place == LanePlace::InHelper);
    watch.leave();
    CHECK(watch.observe().place == LanePlace::Idle);
}

TEST_CASE("LaneWatch.deepNestingIsBalanced", "[vtextension]")
{
    constexpr auto Depth = uint32_t { 20 };
    static_assert(Depth > proto::MaxCallDepth + 1);
    auto watch = LaneWatch {};
    for (auto const id: std::views::iota(uint32_t { 1 }, Depth + 1))
        watch.enter(id);
    CHECK(watch.observe().place == LanePlace::InHelper);

    // Recorded up to MaxCallDepth + 1 frames; the deeper ones are counted only, and cannot be armed.
    watch.interrupt(Depth);
    CHECK(watch.observe().arming == LaneArming::Disarmed);
    watch.interrupt(proto::MaxCallDepth + 1);
    CHECK(watch.observe().arming == LaneArming::Armed);

    for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, Depth))
        watch.leave();
    auto const idle = watch.observe();
    CHECK(idle.place == LanePlace::Idle);
    CHECK(idle.arming == LaneArming::Disarmed);
}

TEST_CASE("LaneWatchSet.interruptsTheLaneTheInstanceRunsOn", "[vtextension]")
{
    auto set = LaneWatchSet {};
    CHECK(set.lanes().size() == MaxLaneChannels);
    set.lane(2).enter(5);
    set.interrupt(6);
    for (auto const& lane: set.lanes())
        CHECK(lane.observe().arming == LaneArming::Disarmed);
    set.interrupt(5);
    for (auto const index: std::views::iota(std::size_t { 0 }, MaxLaneChannels))
        CHECK((set.lanes()[index].observe().arming == LaneArming::Armed) == (index == 2));
}

TEST_CASE("LaneWatchSet.interruptRacesFrames", "[vtextension][threading]")
{
    constexpr auto Rounds = 100'000;
    auto set = LaneWatchSet {};
    auto running = std::atomic<bool> { true };
    auto lane = std::jthread { [&] {
        for ([[maybe_unused]] auto const round: std::views::iota(0, Rounds))
        {
            set.lane(0).enter(7);
            set.lane(0).beginHostCall();
            set.lane(0).enter(9);
            set.lane(0).leave();
            set.lane(0).endHostCall();
            set.lane(0).leave();
        }
        running.store(false);
    } };
    auto observations = std::size_t { 0 };
    while (running.load())
    {
        set.interrupt(7);
        set.interrupt(9);
        std::ignore = set.lanes()[0].observe();
        ++observations;
    }
    lane.join();
    auto const ended = set.lanes()[0].observe();
    CHECK(ended.arming == LaneArming::Disarmed);
    CHECK(ended.place == LanePlace::Idle);
    CHECK(observations > 0);
}
