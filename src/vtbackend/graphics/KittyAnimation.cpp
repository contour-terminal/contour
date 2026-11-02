// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyAnimation.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <ranges>

namespace vtbackend
{

std::chrono::milliseconds KittyAnimation::shownFor(uint32_t number) const noexcept
{
    auto const gap = frames[number - 1].gapMilliseconds;
    if (gap < 0)
        return std::chrono::milliseconds::zero();
    // The root frame is the transmitted image, so nothing gave it a gap unless a=a did; z=0 is "unset" too.
    return std::chrono::milliseconds(gap != 0 ? gap : kitty_graphics::DefaultFrameGapMilliseconds);
}

std::optional<uint32_t> KittyAnimation::successor() const noexcept
{
    auto const hasGap = [this](uint32_t number) {
        return frames[number - 1].gapMilliseconds >= 0;
    };
    auto const count = static_cast<uint32_t>(frames.size());
    for (auto const number: std::views::iota(currentFrame + 1, count + 1))
        if (hasGap(number))
            return number;
    // Out of frames: `s=2` waits here for another to arrive, `s=3` starts over. Not at the current frame
    // itself, which would "advance" to what is already shown and wake the display for nothing.
    if (state != kitty_graphics::AnimationState::Loop)
        return std::nullopt;
    for (auto const number: std::views::iota(uint32_t { 1 }, currentFrame))
        if (hasGap(number))
            return number;
    return std::nullopt;
}

std::chrono::milliseconds KittyAnimation::loopDuration() const noexcept
{
    auto total = std::chrono::milliseconds::zero();
    for (auto const number: std::views::iota(uint32_t { 1 }, static_cast<uint32_t>(frames.size()) + 1))
        total += shownFor(number);
    return total;
}

void KittyAnimation::skipWholeLoops(std::chrono::steady_clock::time_point now,
                                    std::chrono::milliseconds loop) noexcept
{
    // Only `s=3` loops, and only from a frame inside the loop: a gapless current frame is left by a step
    // first. A loop of no time at all (every frame gapless) has nowhere to go and holds instead.
    if (state != kitty_graphics::AnimationState::Loop || nextFrameDue > now
        || loop <= std::chrono::milliseconds::zero() || frames[currentFrame - 1].gapMilliseconds < 0)
        return;
    auto loops = static_cast<uint64_t>((now - nextFrameDue) / loop);
    if (loopCount > 1)
    {
        // `v=n` plays n-1 loops. Skip at most to one short of that, so the walk plays the last to its end
        // and stops there, on the last frame.
        auto const left = uint64_t { loopCount - 1 } - std::min<uint64_t>(loopsDone, loopCount - 1);
        loops = std::min(loops, left > 0 ? left - 1 : 0);
    }
    nextFrameDue += static_cast<int64_t>(loops) * loop;
    loopsDone += static_cast<uint32_t>(loops);
}

void KittyAnimation::arm(std::chrono::steady_clock::time_point now) noexcept
{
    nextFrameDue = isPlaying() && successor() ? now + shownFor(currentFrame) : Held;
}

auto KittyAnimation::advance(std::chrono::steady_clock::time_point now) noexcept -> FrameChanged
{
    if (nextFrameDue > now)
        return FrameChanged::No;

    auto const showing = currentImage();
    // Summed only for a call late by more than the current frame's gap: one less late cannot hold a loop.
    auto loop = std::optional<std::chrono::milliseconds> {};
    // Whole loops are skipped in one go, so what is left to walk frame by frame is less than one loop.
    for ([[maybe_unused]] auto const step: std::views::iota(size_t { 0 }, frames.size()))
    {
        if (nextFrameDue + shownFor(currentFrame) <= now)
        {
            if (!loop)
                loop = loopDuration();
            skipWholeLoops(now, *loop);
        }
        if (nextFrameDue > now)
            break;
        auto const next = successor();
        if (!next)
        {
            // `s=2` out of frames: a transmitted frame ends the wait when one arrives.
            hold();
            break;
        }
        if (*next < currentFrame)
        {
            // Back to the start is one loop played; `v=n` plays n-1 of them, then rests on the last frame.
            ++loopsDone;
            if (loopCount > 1 && loopsDone >= loopCount - 1)
            {
                state = kitty_graphics::AnimationState::Stop;
                hold();
                break;
            }
        }
        currentFrame = *next;
        // From the deadline rather than from now, so a late call does not stretch every later frame.
        nextFrameDue += shownFor(*next);
    }
    // Whole loops skipped, less than one is left to walk, and the walk takes a frame per step.
    Require(nextFrameDue > now);

    // A call can land on the frame it started from (a whole number of loops later): nothing to show.
    return currentImage() == showing ? FrameChanged::No : FrameChanged::Yes;
}

} // namespace vtbackend
