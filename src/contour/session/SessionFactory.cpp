// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/session/SessionFactory.hpp>

#include <vtpty/Process.hpp>

#include <core/Utf8Path.hpp>
#ifdef VTPTY_LIBSSH2
    #include <vtpty/SshSession.hpp>
#endif

#include <algorithm>
#include <functional>
#include <iterator>
#include <utility>

using std::make_unique;
using std::nullopt;

namespace contour::session
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

std::optional<vtpty::SshTerminfoInstaller> sshTerminfoFor(
    config::TerminalProfile const& profile, std::optional<vtpty::SshTerminfoInstaller> const& installer)
{
    if (!installer)
        return std::nullopt;
    auto const& pinned = profile.configuredEnvironment;
    auto const pinnedTerm = pinned.find("TERM");
    auto const profileTerm =
        pinnedTerm != pinned.end() ? std::optional<std::string_view> { pinnedTerm->second } : std::nullopt;
    if (vtpty::effectiveTerminfoInstall(
            profile.ssh.value().terminfoInstall, profileTerm, installer->entryName)
        != vtpty::TerminfoInstall::Install)
        return std::nullopt;
    return installer;
}

std::unique_ptr<vtpty::Pty> AppSessionFactory::createPty(
    std::optional<std::string> cwd,
    std::optional<vtbackend::PageSize> pageSize,
    std::optional<vtpty::Process::ExecInfo> commandOverride,
    std::optional<std::string> profileName)
{
    auto const& profile = _app.config().profile(profileName.value_or(_app.profileName()));
#ifdef VTPTY_LIBSSH2
    // A layout pane's command overrides the profile's shell and should run locally, not open the
    // SSH session the profile would otherwise use. Only a REAL program override counts: a
    // directory-only pane (engaged override, empty program) still runs the profile's shell and
    // must honor its SSH configuration — silently opening a local shell instead would have the
    // user typing on the wrong host.
    if (!overridesShellProgram(commandOverride) && !profile->ssh.value().hostname.empty())
    {
        auto const& ssh = profile->ssh.value();
        auto terminfo = sshTerminfoFor(*profile, _sshTerminfo);
        return make_unique<vtpty::SshSession>(ssh,
                                              std::bind(&AppSessionFactory::requestSshHostkeyVerification,
                                                        this,
                                                        std::placeholders::_1,
                                                        std::placeholders::_2),
                                              std::move(terminfo));
    }
#endif
    // Work on a local copy of the shell config: profile->shell.value() is a reference into the
    // shared, cached profile, so writing the per-session working directory back into it would leak
    // one pane/tab's cwd into every later session created with no explicit cwd. The copy keeps the
    // override session-local.
    auto shell = profile->shell.value();
    vtpty::Process::applyCommandOverride(shell, commandOverride);
    // Variables Contour set to choose its own GPU must not steer the programs run inside it.
    std::ranges::copy(_app.selfOnlyEnvironment(), std::back_inserter(shell.removedEnvironment));
    dropHostingDaemonVariables(shell);
    if (cwd)
        if (auto directory = core::pathFromUtf8(*cwd))
            shell.workingDirectory = *std::move(directory);
    // Spawn the child at the caller's requested grid size when given (a new tab/split inherits the live
    // window's page size), otherwise at the profile's configured terminalSize (a brand-new window). Both
    // are TOTAL page sizes; childPtyPageSize() subtracts the status-line row(s) so the child is born at
    // the terminal's MAIN-display size (see that function for why a background layout pane aborts without
    // this).
    auto const initialSize = childPtyPageSize(pageSize.value_or(profile->terminalSize.value()),
                                              profile->statusLine.value().initialType);
    return make_unique<vtpty::Process>(
        shell, vtpty::createPty(initialSize, nullopt), profile->escapeSandbox.value(), _placement);
}

SessionTransport AppSessionFactory::transportOf([[maybe_unused]] vtpty::Pty const& pty) const noexcept
{
    if (dynamic_cast<vtpty::Process const*>(&pty) != nullptr)
        return SessionTransport::LocalProcess;
#ifdef VTPTY_LIBSSH2
    if (dynamic_cast<vtpty::SshSession const*>(&pty) != nullptr)
        return SessionTransport::Ssh;
#endif
    return SessionTransport::Unknown;
}

#ifdef VTPTY_LIBSSH2
void AppSessionFactory::requestSshHostkeyVerification(
    vtpty::SshHostkeyVerificationRequest const& request,
    vtpty::SshHostkeyVerificationResponseCallback const& response)
{
    (void) request;

    // TODO: implement SSH host key verification dialog
    response(true);
}
#endif

} // namespace contour::session
