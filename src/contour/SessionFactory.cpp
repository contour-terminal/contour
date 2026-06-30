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

std::unique_ptr<vtpty::Pty> SessionFactory::createPty(std::optional<std::string> cwd)
{
    auto const& profile = _app.config().profile(_app.profileName());
#if defined(VTPTY_LIBSSH2)
    if (!profile->ssh.value().hostname.empty())
        return make_unique<vtpty::SshSession>(profile->ssh.value(),
                                              std::bind(&SessionFactory::requestSshHostkeyVerification,
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
    return make_unique<vtpty::Process>(shell,
                                       vtpty::createPty(profile->terminalSize.value(), nullopt),
                                       profile->escapeSandbox.value());
}

#if defined(VTPTY_LIBSSH2)
void SessionFactory::requestSshHostkeyVerification(
    vtpty::SshHostkeyVerificationRequest const& request,
    vtpty::SshHostkeyVerificationResponseCallback const& response)
{
    (void) request;

    // TODO: implement SSH host key verification dialog
    response(true);
}
#endif

} // namespace contour
