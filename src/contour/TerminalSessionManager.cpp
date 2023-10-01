// SPDX-License-Identifier: Apache-2.0
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
    auto pty = make_unique<vtpty::Process>(
        _app.config().profile(_app.profileName())->shell,
        vtpty::createPty(_app.config().profile(_app.profileName())->terminalSize, nullopt));

    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessable from within QML.
    auto session = new TerminalSession(std::move(pty), _app);

    _sessions.push_back(session);

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

// {{{ QAbstractListModel
QVariant TerminalSessionManager::data(const QModelIndex& index, int role) const
{
    crispy::ignore_unused(role);

    if (index.row() < static_cast<int>(_sessions.size()))
        return QVariant(_sessions.at(static_cast<size_t>(index.row()))->id());

    return QVariant();
}

int TerminalSessionManager::rowCount(const QModelIndex& parent) const
{
    crispy::ignore_unused(parent);

    return static_cast<int>(_sessions.size());
}
// }}}

} // namespace contour
