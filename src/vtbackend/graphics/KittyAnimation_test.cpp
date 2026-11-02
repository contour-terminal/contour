// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the kitty animation playback model, clock-free: each case hands advance() a time.

#include <vtbackend/graphics/KittyAnimation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

using namespace std::chrono_literals;
using namespace vtbackend;
using kitty_graphics::AnimationState;
using FrameChanged = KittyAnimation::FrameChanged;

namespace
{

/// An animation of one distinct one-pixel image per gap in @p gaps, on frame @p start.
[[nodiscard]] KittyAnimation makeAnimation(std::vector<int32_t> const& gaps,
                                           AnimationState state,
                                           uint32_t loopCount,
                                           uint32_t start)
{
    auto animation = KittyAnimation {};
    auto id = uint32_t { 1 };
    for (auto const gap: gaps)
        animation.frames.push_back({ .image = std::make_shared<Image const>(ImageId(id++),
                                                                            ImageFormat::RGB,
                                                                            Image::Data { 0, 0, 0 },
                                                                            ImageSize { Width(1), Height(1) },
                                                                            [](Image const*) {}),
                                     .gapMilliseconds = gap });
    animation.state = state;
    animation.loopCount = loopCount;
    animation.currentFrame = start;
    return animation;
}

} // namespace

TEST_CASE("KittyAnimation.advance", "[kitty]")
{
    auto const defaultGap = std::chrono::milliseconds(kitty_graphics::DefaultFrameGapMilliseconds);
    struct Case
    {
        std::string_view name;
        std::vector<int32_t> gaps;
        AnimationState state;
        uint32_t loopCount;
        uint32_t loopsDoneBefore = 0;                   ///< Loops already played when it is armed.
        uint32_t start;                                 ///< The frame armed on, at time zero.
        std::chrono::milliseconds now;                  ///< When advance() is called, after arming.
        uint32_t frame;                                 ///< The frame current afterwards.
        std::optional<std::chrono::milliseconds> dueIn; ///< nullopt: held.
        FrameChanged changed;
        uint32_t loopsDone;
    };
    auto const cases = std::vector<Case> {
        { .name = "not yet due",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 9ms,
          .frame = 1,
          .dueIn = 1ms,
          .changed = FrameChanged::No,
          .loopsDone = 0 },
        { .name = "s=3 steps on by the frame's gap",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 10ms,
          .frame = 2,
          .dueIn = 20ms,
          .changed = FrameChanged::Yes,
          .loopsDone = 0 },
        { .name = "s=3 wraps to frame 1",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 3,
          .now = 30ms,
          .frame = 1,
          .dueIn = 10ms,
          .changed = FrameChanged::Yes,
          .loopsDone = 1 },
        { .name = "s=2 holds at the last frame",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::RunAwaitingFrames,
          .loopCount = 0,
          .start = 3,
          .now = 1000ms,
          .frame = 3,
          .dueIn = std::nullopt,
          .changed = FrameChanged::No,
          .loopsDone = 0 },
        { .name = "s=2 walks to the last frame and holds there",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::RunAwaitingFrames,
          .loopCount = 0,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = std::nullopt,
          .changed = FrameChanged::Yes,
          .loopsDone = 0 },
        { .name = "gapless frames are skipped",
          .gaps = { 10, -1, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 10ms,
          .frame = 3,
          .dueIn = 30ms,
          .changed = FrameChanged::Yes,
          .loopsDone = 0 },
        { .name = "all gapless holds",
          .gaps = { -1, -1, -1 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 1000ms,
          .frame = 1,
          .dueIn = std::nullopt,
          .changed = FrameChanged::No,
          .loopsDone = 0 },
        // v=3 plays two 30 ms loops: one call a second later rests on the last frame.
        { .name = "v=n stops on the last frame",
          .gaps = { 10, 10, 10 },
          .state = AnimationState::Loop,
          .loopCount = 3,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = std::nullopt,
          .changed = FrameChanged::Yes,
          .loopsDone = 2 },
        // 1000 ms is 40 ms into the 60 ms loop: the third frame's, shown until 1020 ms.
        { .name = "whole loops are skipped",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = 20ms,
          .changed = FrameChanged::Yes,
          .loopsDone = 16 },
        // Ten 60 ms loops to the millisecond: the first frame again, with its whole gap ahead.
        { .name = "an exact multiple of the loop lands on its start",
          .gaps = { 10, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 600ms,
          .frame = 1,
          .dueIn = 10ms,
          .changed = FrameChanged::No,
          .loopsDone = 10 },
        { .name = "z=0 is shown for the default gap",
          .gaps = { 0, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = defaultGap - 1ms,
          .frame = 1,
          .dueIn = 1ms,
          .changed = FrameChanged::No,
          .loopsDone = 0 },
        { .name = "z=0 moves on after the default gap",
          .gaps = { 0, 20, 30 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = defaultGap,
          .frame = 2,
          .dueIn = 20ms,
          .changed = FrameChanged::Yes,
          .loopsDone = 0 },
        // v=4 plays three loops and one is played already: one is skipped, and the walk plays the last.
        { .name = "v=n with loops already played stops on the last frame",
          .gaps = { 10, 10, 10 },
          .state = AnimationState::Loop,
          .loopCount = 4,
          .loopsDoneBefore = 1,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = std::nullopt,
          .changed = FrameChanged::Yes,
          .loopsDone = 3 },
        // v=2 plays one loop: none is skipped, the walk plays it and stops.
        { .name = "v=2 plays its one loop to the last frame",
          .gaps = { 10, 10, 10 },
          .state = AnimationState::Loop,
          .loopCount = 2,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = std::nullopt,
          .changed = FrameChanged::Yes,
          .loopsDone = 1 },
        // A gapless current frame is left by a step before whole loops can be skipped: 1000 ms is 10 ms
        // into the 30 ms loop of frames 2 and 3, the third frame's, shown until 1020 ms.
        { .name = "a late call from a gapless frame steps, then skips whole loops",
          .gaps = { -1, 10, 20 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = 20ms,
          .changed = FrameChanged::Yes,
          .loopsDone = 33 },
        { .name = "s=3 with only one frame with a gap holds",
          .gaps = { 10, -1, -1 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 1000ms,
          .frame = 1,
          .dueIn = std::nullopt,
          .changed = FrameChanged::No,
          .loopsDone = 0 },
        // From a gapless frame onto the only one with a gap, which has nowhere to go from there.
        { .name = "s=3 reaching its only frame with a gap holds there",
          .gaps = { -1, -1, 10 },
          .state = AnimationState::Loop,
          .loopCount = 0,
          .start = 1,
          .now = 1000ms,
          .frame = 3,
          .dueIn = std::nullopt,
          .changed = FrameChanged::Yes,
          .loopsDone = 99 },
    };

    auto const origin = std::chrono::steady_clock::time_point {} + 1h;
    for (auto const& testCase: cases)
    {
        DYNAMIC_SECTION(testCase.name)
        {
            auto animation = makeAnimation(testCase.gaps, testCase.state, testCase.loopCount, testCase.start);
            animation.loopsDone = testCase.loopsDoneBefore;
            animation.arm(origin);
            auto const now = origin + testCase.now;

            CHECK(animation.advance(now) == testCase.changed);
            CHECK(animation.currentFrame == testCase.frame);
            CHECK(animation.loopsDone == testCase.loopsDone);
            if (testCase.dueIn)
                CHECK(animation.nextFrameDue - now == *testCase.dueIn);
            else
                CHECK(animation.isHeld());
        }
    }
}

TEST_CASE("KittyAnimation.v_n_stops_playback", "[kitty]")
{
    auto animation = makeAnimation({ 10, 10, 10 }, AnimationState::Loop, 3, 1);
    auto const origin = std::chrono::steady_clock::time_point {} + 1h;
    animation.arm(origin);
    std::ignore = animation.advance(origin + 1000ms);
    CHECK(animation.state == AnimationState::Stop);
    CHECK_FALSE(animation.isPlaying());
}

TEST_CASE("KittyAnimation.toDeadline", "[kitty]")
{
    auto const due = std::chrono::steady_clock::time_point {} + 1h;
    CHECK(toDeadline(due) == due);
    CHECK_FALSE(toDeadline(KittyAnimation::Held).has_value());
}
