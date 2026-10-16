// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/TaskbarProgressRouter.hpp>

#include <optional>

namespace contour::platform
{

/// Orders Windows taskbar calls, stopping after the first failed call.
/// @param progress Desired aggregate.
/// @param setValue Writes a percentage and returns an expected result.
/// @param setState Writes a state and returns the same expected result type.
/// @return The complete update's result. Busy first clears any old determinate value.
template <typename SetValue, typename SetState>
[[nodiscard]] auto applyWindowsTaskbarProgress(TaskbarProgress progress, SetValue setValue, SetState setState)
{
    if (progress.state == vtbackend::ProgressState::Indeterminate)
    {
        if (auto result = setState(vtbackend::ProgressState::Inactive); !result)
            return result;
    }
    else if (progress.state != vtbackend::ProgressState::Inactive)
    {
        if (auto result = setValue(progress.percentage); !result)
            return result;
    }
    return setState(progress.state);
}

/// Pending versus successfully published progress for one native taskbar button.
class TaskbarProgressState
{
  public:
    /// Retains the latest desired progress, whether or not the button exists yet.
    /// @param progress Window aggregate to publish.
    void setProgress(TaskbarProgress progress) noexcept { _desired = progress; }

    /// A new button needs the current progress replayed, even if an older button already received it.
    void buttonCreated() noexcept
    {
        _button = Button::Ready;
        _sent.reset();
    }

    /// A destroyed native surface must receive no more updates until its new button is created.
    void surfaceDestroyed() noexcept
    {
        _button = Button::Unavailable;
        _sent.reset();
    }

    /// Returns the next update, suppressing only updates that already succeeded on this button.
    /// @return Progress to send, or nullopt while unavailable or unchanged.
    [[nodiscard]] std::optional<TaskbarProgress> pending() const noexcept
    {
        return _button == Button::Ready && _sent != _desired ? std::optional { _desired } : std::nullopt;
    }

    /// Acknowledges a complete native update. Failures leave pending() unchanged for a later retry.
    void applied() noexcept { _sent = _desired; }

  private:
    enum class Button : uint8_t
    {
        Unavailable,
        Ready,
    };

    Button _button = Button::Unavailable;
    TaskbarProgress _desired;
    std::optional<TaskbarProgress> _sent;
};

} // namespace contour::platform
