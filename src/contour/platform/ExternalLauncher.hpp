// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QStringList>
#include <QtCore/QUrl>

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace contour::platform
{

/// Why a resource could not be opened. Each value names a distinct, user-actionable cause, so the
/// caller can report WHY nothing happened rather than just that nothing did.
///
/// Every cause here is knowable without a round trip to the desktop — @see ExternalLauncher::openUrl
/// for why that matters and what is deliberately NOT in this enumeration.
enum class LaunchError : std::uint8_t
{
    InvalidUrl = 0, ///< The URL is empty, or not something a handler could be found for.
    NoHandler,      ///< Nothing on this desktop is registered to open it.
};

/// A human-readable explanation of @p error, for logs and user-facing notices.
/// @param error The failure to describe.
/// @return A short sentence naming the cause.
[[nodiscard]] constexpr std::string_view describe(LaunchError error) noexcept
{
    switch (error)
    {
        case LaunchError::InvalidUrl: return "the address is not a valid URL";
        case LaunchError::NoHandler: return "no application is registered to open it";
    }
    return "unknown error";
}

/// Why a child process could not be started, or did not finish.
///
/// Separate from LaunchError because the two ask different questions: opening a URL asks the desktop
/// to CHOOSE a handler, while this names a program outright and can say which part of running it
/// went wrong. Qt reports one FailedToStart for both of the first two, which are different things to
/// tell a user, so the launcher determines the first itself rather than guessing.
enum class SpawnError : std::uint8_t
{
    NotFound = 0, ///< No such program: not on the search path, or the named path is not executable.
    StartFailed,  ///< The program is there, but the platform would not start it.
    Crashed,      ///< It ran and died on a signal. Only execute() waits long enough to observe this.
};

/// A human-readable explanation of @p error, for logs and user-facing notices.
/// @param error The failure to describe.
/// @return A short sentence naming the cause.
[[nodiscard]] constexpr std::string_view describe(SpawnError error) noexcept
{
    switch (error)
    {
        case SpawnError::NotFound: return "no such program on the search path";
        case SpawnError::StartFailed: return "it could not be started";
        case SpawnError::Crashed: return "it crashed";
    }
    return "unknown error";
}

/// Whether @p url is something an ExternalLauncher could hand to the desktop at all.
///
/// The one rejection every implementation makes for itself, stated once here rather than restated
/// per implementation: it is a property of the URL, not of how the desktop is reached.
///
/// @param url The resource to open.
/// @return true when the desktop can be asked about it.
[[nodiscard]] inline bool isOpenable(QUrl const& url)
{
    return !url.isEmpty() && url.isValid();
}

/// Launches external resources on behalf of a terminal session: opening URLs/documents in the
/// desktop's default handler and running detached or blocking child processes.
///
/// This is the part of a session that reaches OUT of the process — into the desktop environment
/// (a browser, a file manager, an editor) — and it is an interface per the project's
/// dependency-injection principle: spawning a browser or a child `contour` is process I/O, so it
/// must be reached through an abstraction rather than the concrete static Qt calls
/// (`QDesktopServices::openUrl`, `QProcess::execute`/`startDetached`). Production wires
/// QtExternalLauncher; tests inject a recording launcher and assert the routing/validation of the
/// open-document, follow-hyperlink, and spawn-terminal actions without actually launching anything.
class ExternalLauncher
{
  public:
    virtual ~ExternalLauncher() = default;

    /// DISPATCHES a request to open @p url in the desktop's default handler (browser, file manager,
    /// editor, ...).
    ///
    /// It dispatches rather than completes, and the distinction is the whole contract: on Linux the
    /// desktop is reached over D-Bus, and waiting for that reply would block whichever thread called
    /// — the GUI thread, on every hyperlink click. @see PortalExternalLauncher and
    /// https://github.com/contour-terminal/contour/issues/2075
    ///
    /// So a success here means the request was accepted for delivery, NOT that anything opened; an
    /// error means it could be rejected without asking the desktop at all. A failure the desktop
    /// itself reports arrives later and is logged, because by then the action has visibly finished
    /// and there is nothing left for the caller to do about it.
    ///
    /// @param url The resource to open.
    /// @return Nothing on acceptance, or why the request was rejected outright.
    [[nodiscard]] virtual std::expected<void, LaunchError> openUrl(QUrl const& url) = 0;

    /// Starts @p program with @p arguments as a detached background process (fire-and-forget).
    ///
    /// Fire-and-forget names the CHILD's lifetime, not the report: whether it started is knowable
    /// here and now, and a caller that cannot say why nothing happened has nothing to log.
    ///
    /// @param program The executable path.
    /// @param arguments The argument list.
    /// @return Nothing once started, or why it was not.
    [[nodiscard]] virtual std::expected<void, SpawnError> runDetached(QString const& program,
                                                                      QStringList const& arguments) = 0;

    /// Runs @p program with @p arguments and blocks until it exits (used where the caller wants the
    /// child — e.g. an `$EDITOR` — to run to completion).
    ///
    /// The exit code and the reasons it has none are separate channels: a program that exited 1 and
    /// a program that is not installed used to be told apart by the SIGN of one integer.
    ///
    /// @param program The executable path.
    /// @param arguments The argument list.
    /// @return The process exit code, or why there is none.
    [[nodiscard]] virtual std::expected<int, SpawnError> execute(QString const& program,
                                                                 QStringList const& arguments) = 0;
};

/// The launcher this platform can offer.
///
/// A PortalExternalLauncher on Linux and a QtExternalLauncher everywhere else. Mirrors
/// makeDesktopNotifier() and makeSpeechSynthesizer(): the platform split lives here, so nothing
/// above this layer needs an #ifdef to hold a launcher.
///
/// Note that this does NOT branch on SandboxState the way selectNotificationBackend() does. The
/// reason is worth reading before "fixing" the asymmetry: @see PortalExternalLauncher.
///
/// @return The launcher; never null.
[[nodiscard]] std::unique_ptr<ExternalLauncher> makeExternalLauncher();

} // namespace contour::platform
