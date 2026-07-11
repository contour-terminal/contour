// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.h>
#include <contour/SessionFactory.h>

#include <vtpty/Process.h>
#if defined(VTPTY_LIBSSH2)
    #include <vtpty/SshSession.h>
#endif

#include <filesystem>
#include <functional>

using std::make_unique;
using std::nullopt;

namespace contour
{

vtbackend::PageSize childPtyPageSize(vtbackend::PageSize total,
                                     vtbackend::StatusDisplayType statusLine) noexcept
{
    // The terminal reserves the bottom row(s) for the status line (height 1 for Indicator/HostWritable,
    // 0 for None), so the child's usable area — and the winsize it must be born with — is the total page
    // size minus the status line. Clamp so a degenerate 1-line total never underflows to 0.
    auto const rows =
        statusLine != vtbackend::StatusDisplayType::None ? vtbackend::LineCount(1) : vtbackend::LineCount(0);
    if (total.lines > rows)
        total.lines = total.lines - rows;
    return total;
}

std::unique_ptr<vtpty::Pty> AppSessionFactory::createPty(
    std::optional<std::string> cwd,
    std::optional<vtbackend::PageSize> pageSize,
    std::optional<vtpty::Process::ExecInfo> commandOverride,
    std::optional<std::string> profileName)
{
    auto const& profile = _app.config().profile(profileName.value_or(_app.profileName()));
#if defined(VTPTY_LIBSSH2)
    // A layout pane's command overrides the profile's shell and should run locally, not open the
    // SSH session the profile would otherwise use.
    if (!commandOverride && !profile->ssh.value().hostname.empty())
        return make_unique<vtpty::SshSession>(profile->ssh.value(),
                                              std::bind(&AppSessionFactory::requestSshHostkeyVerification,
                                                        this,
                                                        std::placeholders::_1,
                                                        std::placeholders::_2));
#endif
    // Work on a local copy of the shell config: profile->shell.value() is a reference into the
    // shared, cached profile, so writing the per-session working directory back into it would leak
    // one pane/tab's cwd into every later session created with no explicit cwd. The copy keeps the
    // override session-local.
    auto shell = profile->shell.value();
    if (commandOverride)
    {
        // Replace the shell program/args with the layout pane's command; keep env from the profile.
        // Only overlay when a program was actually given: a directory-only pane override has an empty
        // program, and must not wipe the profile shell's default arguments.
        if (!commandOverride->program.empty())
        {
            shell.program = commandOverride->program;
            shell.arguments = commandOverride->arguments;
        }
        if (!commandOverride->workingDirectory.empty())
            shell.workingDirectory = commandOverride->workingDirectory;
    }
    if (cwd)
        shell.workingDirectory = std::filesystem::path(cwd.value());
    // Spawn the child at the caller's requested grid size when given (a new tab/split inherits the live
    // window's page size), otherwise at the profile's configured terminalSize (a brand-new window). Both
    // are TOTAL page sizes; childPtyPageSize() subtracts the status-line row(s) so the child is born at
    // the terminal's MAIN-display size (see that function for why a background layout pane aborts without
    // this).
    auto const initialSize = childPtyPageSize(pageSize.value_or(profile->terminalSize.value()),
                                              profile->statusLine.value().initialType);
    return make_unique<vtpty::Process>(
        shell, vtpty::createPty(initialSize, nullopt), profile->escapeSandbox.value());
}

#if defined(VTPTY_LIBSSH2)
void AppSessionFactory::requestSshHostkeyVerification(
    vtpty::SshHostkeyVerificationRequest const& request,
    vtpty::SshHostkeyVerificationResponseCallback const& response)
{
    (void) request;

    // TODO: implement SSH host key verification dialog
    response(true);
}
#endif

} // namespace contour
