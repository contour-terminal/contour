#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>

using std::make_unique;
using std::nullopt;

namespace contour
{

TerminalSessionManager::TerminalSessionManager(ContourGuiApp& app): _app { app }, _earlyExitThreshold {}
{
}

TerminalSession* TerminalSessionManager::createSession()
{
    auto session = new TerminalSession(
        make_unique<terminal::Process>(
            _app.config().profile(_app.profileName())->shell,
            terminal::createPty(_app.config().profile(_app.profileName())->terminalSize, nullopt)),
        _app);

    connect(session, &TerminalSession::sessionClosed, [this, session]() { removeSession(*session); });

    return session;
}

void TerminalSessionManager::removeSession(TerminalSession& thatSession)
{
    _app.onExit(thatSession); // TODO: the logic behind that impl could probably be moved here.

    auto i = std::find_if(_sessions.begin(), _sessions.end(), [&](auto p) { return p == &thatSession; });
    if (i != _sessions.end())
    {
        _sessions.erase(i);
    }

    // Notify app if all sessions have been killed to trigger app termination.
}

} // namespace contour
