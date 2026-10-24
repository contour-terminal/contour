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

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtextension/CommandPolicy.hpp>
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

    /// Tells whether the user lets the extension use the control permissions its manifest declares. One
    /// answer covers a manifest's commands and its control permissions together, bound to its digest.
    /// Never blocks on the user: the first time the answer is `Pending`, the host asks, as it does for a
    /// command refused as pending.
    /// @return The answer: `Granted`, `Denied` (or extensions may not run commands at all), or `Pending`.
    ///         `Granted`, without asking, where the host serves no control API: callControl() then answers
    ///         every verb `unsupported`, and nobody is asked to grant what nothing can do.
    [[nodiscard]] virtual GrantState controlConsent() = 0;

    /// Serves one control verb for the extension, as its pane: the guest's import, after its checks (see
    /// callControlImport()). Blocks the calling thread until the host answers, at most @p timeout.
    /// @param method The verb's wire name (`pane.split`).
    /// @param paramsJson Its request, as the control API's JSON.
    /// @param granted What the extension is granted; the host refuses a verb outside it again.
    /// @param timeout The longest the call may wait: what is left of the guest call's deadline.
    /// @return The reply's JSON; or the control error, `failed` "timed out" when @p timeout passed, and
    ///         `unsupported` where the host serves no control API.
    [[nodiscard]] virtual std::expected<std::string, vtcontrol::ControlError> callControl(
        std::string_view method,
        std::string_view paramsJson,
        vtcontrol::ControlGrants granted,
        std::chrono::milliseconds timeout) = 0;
};

} // namespace vtextension
