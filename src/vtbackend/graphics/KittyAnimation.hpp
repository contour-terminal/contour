// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>
#include <vtbackend/graphics/KittyGraphics.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace vtbackend
{

/// @return @p due as a deadline, or nullopt for time_point::max(), which stands for none.
[[nodiscard]] constexpr std::optional<std::chrono::steady_clock::time_point> toDeadline(
    std::chrono::steady_clock::time_point due) noexcept
{
    if (due == std::chrono::steady_clock::time_point::max())
        return std::nullopt;
    return due;
}

/// The animation frames of one kitty image, and where its playback stands.
///
/// Frame 1 is the transmitted image itself: the protocol has no separate command to send it, and every
/// stored image therefore has one frame from the moment it exists. The model reads no clock and knows no
/// page: advance() is handed the time, and the caller shows whichever frame it lands on.
struct KittyAnimation
{
    /// One frame: its pixels and how long it is shown.
    struct Frame
    {
        std::shared_ptr<Image const> image;
        int32_t gapMilliseconds = 0;
    };

    /// Whether advance() moved playback onto another frame.
    enum class FrameChanged : uint8_t
    {
        No,  ///< The frame shown before is still the one to show.
        Yes, ///< Another frame is current, and must be shown.
    };

    std::vector<Frame> frames; ///< Frame N of the wire is index N-1 here.
    uint32_t currentFrame = 1; ///< Which frame a placement displays.
    kitty_graphics::AnimationState state = kitty_graphics::AnimationState::Unset;
    uint32_t loopCount = 0; ///< 0 unspecified, 1 forever, n>1 loop n-1 times.
    uint32_t loopsDone = 0; ///< Loops played since playback started or `v=` was last given.

    /// The nextFrameDue of an animation that holds: never.
    static constexpr auto Held = std::chrono::steady_clock::time_point::max();

    /// When playback moves on from the current frame; Held while it holds -- stopped, out of frames under
    /// `s=2`, with no other frame to move to, or with no placement on the page to show one in.
    std::chrono::steady_clock::time_point nextFrameDue = Held;

    /// @return whether playback holds on the current frame, with no deadline to move on at.
    [[nodiscard]] bool isHeld() const noexcept { return nextFrameDue == Held; }

    /// Holds playback on the current frame until something re-arms it.
    void hold() noexcept { nextFrameDue = Held; }

    /// @return the image of 1-based frame @p number, or nullptr if there is no such frame.
    [[nodiscard]] std::shared_ptr<Image const> frameImage(uint32_t number) const
    {
        return number != 0 && number <= frames.size() ? frames[number - 1].image : nullptr;
    }

    /// @return the image a placement of this animation shows now.
    [[nodiscard]] std::shared_ptr<Image const> currentImage() const { return frameImage(currentFrame); }

    /// @return whether playback runs: `s=2` or `s=3`, and not yet stopped by its loop count.
    [[nodiscard]] bool isPlaying() const noexcept
    {
        return state == kitty_graphics::AnimationState::RunAwaitingFrames
               || state == kitty_graphics::AnimationState::Loop;
    }

    /// @return how long 1-based frame @p number is shown: zero for a gapless frame (`z<0`), which playback
    ///         skips, and the protocol's default for one whose gap was never given.
    [[nodiscard]] std::chrono::milliseconds shownFor(uint32_t number) const noexcept;

    /// @return the frame playback moves to from the current one -- the next with a gap, returning to the
    ///         first under `s=3` -- or nullopt when there is none to move to.
    [[nodiscard]] std::optional<uint32_t> successor() const noexcept;

    /// @return how long one loop takes: every frame's shownFor(), gapless ones counting nothing.
    [[nodiscard]] std::chrono::milliseconds loopDuration() const noexcept;

    /// Skips, under `s=3`, as many whole loops as fit between the deadline and @p now, counting them towards
    /// `v=` -- but never the last one `v=` allows, which the caller plays to its end and stops on. Leaves the
    /// current frame as it is: a whole loop ends where it began.
    /// @param now The time playback is advanced to.
    /// @param loop The animation's loopDuration().
    void skipWholeLoops(std::chrono::steady_clock::time_point now, std::chrono::milliseconds loop) noexcept;

    /// Starts the current frame's gap at @p now, or holds when playback has nowhere to go.
    void arm(std::chrono::steady_clock::time_point now) noexcept;

    /// Advances playback, if its next frame is due at @p now, to the frame @p now shows: whole loops a late
    /// call missed are skipped at once and counted towards `v=`, the rest walked frame by frame, each from
    /// the deadline before it rather than from @p now, so a late call stretches no later frame. Holds when
    /// playback runs out of frames (`s=2`) or `v=` is used up, which also stops it.
    /// @param now The time to advance to.
    /// @return Whether another frame is current now. A whole number of loops lands on the frame it began on.
    [[nodiscard]] FrameChanged advance(std::chrono::steady_clock::time_point now) noexcept;
};

} // namespace vtbackend
