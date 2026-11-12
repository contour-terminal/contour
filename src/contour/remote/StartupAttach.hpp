// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The launch's wait for its attach, from the event loop: `contour client --host` attaches before its first
/// window opens, and the first window needs the attach settled. Nothing here blocks the GUI thread: the
/// attach is looked at when it has news and when its time is up, so ssh's prompts are answered by the windows
/// of the same loop meanwhile.

#include <contour/config/Actions.hpp>

#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>

namespace contour::remote
{

/// Whether an action may run while the launch still waits for its attach.
enum class DuringStartup : uint8_t
{
    Refused =
        0, ///< It does nothing: no terminal window exists, and a window started now would not be the first.
    Allowed,
};

/// @param action An action asked for from outside any window (a menu row, the Dock).
/// @return Allowed for Quit alone: it needs no window; everything else would start a window or reconfigure
///         the app before its first window has adopted the attach.
[[nodiscard]] DuringStartup duringStartup(actions::Action const& action) noexcept;

/// Told, from the event loop, that the attach has started, or why it cannot be.
using StartupAttachReady = std::function<void(std::expected<void, std::string> started)>;

/// What the launch's attach wait reaches: the attach, and what follows it. Injected, so the wait is driven by
/// fakes in a test.
struct StartupAttachSteps
{
    /// One look at the attach: nothing while it goes on; else how it ended.
    std::function<std::optional<std::expected<void, std::string>>()> look;
    /// How long from now until the attach is given up; none while that is not known.
    std::function<std::optional<std::chrono::milliseconds>()> untilDeadline;
    /// Decides what the attach's end means and opens the first window.
    /// @return Nothing once the first window is open; why the launch cannot go on otherwise.
    std::function<std::expected<void, std::string>(std::expected<void, std::string> attached)>
        openFirstWindow;
    /// The launch ends: its reason, on stderr, and its exit status.
    std::function<void(std::string const& reason, int exitCode)> end;
    /// What comes before the attach can be looked at, asynchronously: a launch that names a workspace lists
    /// the daemon's workspaces first, which may ask the user about the daemon, and starts its attach with the
    /// one it names. Called once, by start(); it tells its argument, from the event loop, that the attach
    /// started or why it cannot. The launch waits meanwhile as it waits for the attach. Empty for an attach
    /// that started already.
    std::function<void(StartupAttachReady ready)> prepare = nullptr;
};

/// Waits for a launch's attach to settle, then opens the first window, or ends the launch with the reason.
class StartupAttach: public QObject
{
    Q_OBJECT
  public:
    /// @param steps What the wait reaches.
    explicit StartupAttach(StartupAttachSteps steps);

    /// Prepares the attach, if there is anything to (StartupAttachSteps::prepare); then takes a first look
    /// (from the event loop), and looks again on every progress and at the deadline.
    void start();

    /// @return Whether the attach has not settled yet, its preparation included: no terminal window exists,
    ///         and none may be started.
    [[nodiscard]] bool isWaiting() const noexcept
    {
        return _phase == Phase::Preparing || _phase == Phase::Waiting;
    }

  public slots:
    /// The attach has news: looks at it again.
    void progressed();

  private:
    /// Where the wait is.
    enum class Phase : uint8_t
    {
        Idle = 0,  ///< Not started.
        Preparing, ///< The attach is being prepared: nothing to look at yet.
        Waiting,   ///< The attach goes on: looked at on its news and at its deadline.
        Settled,   ///< The attach ended, and the first window opened or the launch ended.
    };

    /// The preparation ended: the attach is looked at now, or the launch ends with why it cannot start.
    void prepared(std::expected<void, std::string> started);

    StartupAttachSteps _steps;
    QTimer _deadline;
    Phase _phase = Phase::Idle;
};

} // namespace contour::remote
