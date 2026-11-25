// SPDX-License-Identifier: Apache-2.0
//
// Every access is sequentially consistent, which the parent's interrupt relies on: a call records its frame
// (enter(), the depth store last) and then reads its instance's interrupt flag, while interrupt() sets that
// flag and then reads the depth -- so that either the call sees the flag and does not start, or the interrupt
// sees the frame and arms it. The frames are rare and the cost is a few stores per call, beside a round trip
// to another process.
#include <vtextension/wasm/remote/LaneWatch.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vtextension::wasm::remote
{

void LaneWatch::enter(uint32_t instanceId) noexcept
{
    auto const depth = _depth.load();
    if (depth < LaneFrameCapacity)
    {
        auto& frame = _frames[depth];
        frame.instanceId.store(instanceId);
        frame.state.store(FrameState::Running);
        // Last: an arming of the frame that was here before carries that frame's serial, never this one.
        frame.serial.store(_nextSerial.fetch_add(1));
    }
    _depth.store(depth + 1);
}

void LaneWatch::leave() noexcept
{
    auto const depth = _depth.load();
    Require(depth > 0);
    // The frame is gone once the depth says so: its arming, if any, goes with it, since a frame entered here
    // next has a serial of its own.
    _depth.store(depth - 1);
}

void LaneWatch::beginHostCall() noexcept
{
    if (auto const depth = _depth.load(); depth > 0 && depth <= LaneFrameCapacity)
        _frames[depth - 1].state.store(FrameState::ServingHost);
}

void LaneWatch::endHostCall() noexcept
{
    if (auto const depth = _depth.load(); depth > 0 && depth <= LaneFrameCapacity)
        _frames[depth - 1].state.store(FrameState::Running);
}

bool LaneWatch::interrupt(uint32_t instanceId) noexcept
{
    auto const depth = std::min(_depth.load(), LaneFrameCapacity);
    for (auto& frame: std::span { _frames }.first(depth))
    {
        if (frame.instanceId.load() != instanceId)
            continue;
        // Should the frame have been left and another entered in its place meanwhile, this arms a serial no
        // frame has any more: an arming that the supervisor never sees.
        frame.armedSerial.store(frame.serial.load());
        return true;
    }
    return false;
}

LaneObservation LaneWatch::observe() const noexcept
{
    auto const depth = _depth.load();
    if (depth == 0)
        return LaneObservation {};
    auto observation =
        LaneObservation { .epoch = 0, .arming = LaneArming::Disarmed, .place = LanePlace::InHelper };
    auto const recorded = std::span { _frames }.first(std::min(depth, LaneFrameCapacity));
    for (auto const& frame: recorded)
    {
        auto const serial = frame.serial.load();
        if (serial != 0 && frame.armedSerial.load() == serial)
        {
            observation.epoch = serial;
            observation.arming = LaneArming::Armed;
            break;
        }
    }
    // A frame deeper than the record runs in the host, as far as the backstop knows: its grace runs.
    if (depth <= LaneFrameCapacity && recorded.back().state.load() == FrameState::ServingHost)
        observation.place = LanePlace::ServingHost;
    return observation;
}

LaneWatch& LaneWatchSet::lane(std::size_t index) noexcept
{
    Require(index < _lanes.size());
    return _lanes[index];
}

bool LaneWatchSet::interrupt(uint32_t instanceId) noexcept
{
    auto found = false;
    for (auto& lane: _lanes)
        found = lane.interrupt(instanceId) || found;
    return found;
}

} // namespace vtextension::wasm::remote
