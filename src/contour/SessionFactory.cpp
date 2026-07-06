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

std::unique_ptr<vtpty::Pty> AppSessionFactory::createPty(std::optional<std::string> cwd,
                                                         std::optional<vtbackend::PageSize> pageSize)
{
    auto const& profile = _app.config().profile(_app.profileName());
#if defined(VTPTY_LIBSSH2)
    if (!profile->ssh.value().hostname.empty())
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
    if (cwd)
        shell.workingDirectory = std::filesystem::path(cwd.value());
    // Spawn the child at the caller's requested grid size when given (a new tab/split inherits the live
    // window's page size), otherwise at the profile's configured terminalSize (a brand-new window).
    auto const initialSize = pageSize.value_or(profile->terminalSize.value());
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
