// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/Event.hpp>
#include <vtextension/ExtensionError.hpp>

namespace vtextension
{

/// How loud a message an extension logs is; the world's `log-level`.
enum class LogLevel : uint8_t
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

/// The current values of everything an event can announce; the world's `state-snapshot`.
struct StateSnapshot
{
    std::string title;                       ///< The pane's or window's title.
    std::string cwd;                         ///< The working directory.
    std::string foregroundProcess;           ///< The foreground process's name.
    InputMode inputMode = InputMode::Insert; ///< The input mode.
    std::vector<Tab> tabs;                   ///< The window's tabs, in order.
    std::vector<std::pair<std::string, std::string>>
        userVars;                                         ///< Every user variable set, as name and value.
    Progress progress;                                    ///< The progress indicator.
    FocusState focus = FocusState::Unfocused;             ///< Whether the pane has the focus.
    SurfaceSize size;                                     ///< The extension's own surface.
    std::chrono::sys_time<std::chrono::milliseconds> now; ///< Wall-clock time: the extension's only clock.
    std::chrono::minutes utcOffset {}; ///< How far local time at @ref now is ahead of UTC (DST included).
    FontMetrics font;                  ///< The font the terminal draws in; zeros while unknown.

    [[nodiscard]] bool operator==(StateSnapshot const&) const = default;
};

/// What an extension instance may ask of the host: the world's `host` interface, as the host
/// implements it. The instance calls it on the thread that calls into the instance, from inside a
/// guest call; an implementation must not call back into the same instance.
class IHostServices
{
  public:
    IHostServices() = default;
    IHostServices(IHostServices const&) = delete;
    IHostServices(IHostServices&&) = delete;
    IHostServices& operator=(IHostServices const&) = delete;
    IHostServices& operator=(IHostServices&&) = delete;
    virtual ~IHostServices() = default;

    /// Asks for a `timer` event after at least @p after.
    /// @param after The shortest delay before the event.
    virtual void requestFrame(std::chrono::milliseconds after) = 0;

    /// Writes a message the extension logs.
    /// @param level How loud the message is.
    /// @param message The message, valid UTF-8.
    virtual void log(LogLevel level, std::string_view message) = 0;

    /// @return The current state, as `state` answers the extension.
    [[nodiscard]] virtual StateSnapshot state() const = 0;

    /// Runs a command the extension's manifest declares, and waits for it. Never blocks on the
    /// user: an unanswered permission is an error.
    /// @param name The declared command's name.
    /// @param args The call's arguments, by placeholder name.
    /// @return The command's standard output; or why it did not run or did not finish:
    ///         `CommandNotDeclared` or `ArgumentRejected` (the extension's mistake),
    ///         `PermissionPending`, `PermissionDenied`, `DeadlineExceeded`, or any other error for a
    ///         command that could not be started or did not exit normally.
    [[nodiscard]] virtual std::expected<std::string, ExtensionError> runCommand(
        std::string_view name, std::span<std::pair<std::string, std::string> const> args) = 0;
};

} // namespace vtextension
