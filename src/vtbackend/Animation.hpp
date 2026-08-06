// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>

namespace vtbackend
{

/// Linear easing: identity mapping.
struct LinearEasing
{
    constexpr float operator()(float t) const noexcept
    {
        assert(t >= 0.0f && t <= 1.0f);
        return t;
    }
};

/// Ease-out cubic easing: 1 - (1 - t)^3.
struct EaseOutCubic
{
    constexpr float operator()(float t) const noexcept
    {
        assert(t >= 0.0f && t <= 1.0f);
        auto const inv = 1.0f - t;
        return 1.0f - (inv * inv * inv);
    }
};

/// Common base for one-shot animations with configurable easing.
///
/// @tparam EasingFn  A callable `float(float) noexcept` mapping linear t in [0,1] to eased output.
template <typename EasingFn>
struct AnimationState
{
    bool active = false;
    std::chrono::steady_clock::time_point startTime {};
    std::chrono::milliseconds duration {};

    /// Returns animation progress in [0, 1] with the configured easing applied.
    [[nodiscard]] float progress(std::chrono::steady_clock::time_point now) const noexcept
    {
        if (!active || duration.count() == 0)
            return 1.0f;
        auto const elapsed = std::chrono::duration<float, std::milli>(now - startTime).count();
        auto const t = std::clamp(elapsed / static_cast<float>(duration.count()), 0.0f, 1.0f);
        return EasingFn {}(t);
    }

    /// Returns true if the animation has completed.
    [[nodiscard]] bool isComplete(std::chrono::steady_clock::time_point now) const noexcept
    {
        return progress(now) >= 1.0f;
    }
};

} // namespace vtbackend
