// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>

#include <vtbackend/primitives.h>

#include <vtpty/Process.h>
#if defined(VTPTY_LIBSSH2)
    #include <vtpty/SshSession.h>
#endif

#include <QtQml/QQmlEngine>

#include <string>

using namespace std::string_literals;

using std::make_unique;
using std::nullopt;

namespace contour
{

TerminalSessionManager::TerminalSessionManager(ContourGuiApp& app): _app { app }, _earlyExitThreshold {}
{
}

std::unique_ptr<vtpty::Pty> TerminalSessionManager::createPty()
{
    auto const& profile = _app.config().profile(_app.profileName());
#if defined(VTPTY_LIBSSH2)
    if (!profile->ssh.value().hostname.empty())
        return make_unique<vtpty::SshSession>(profile->ssh.value());
#endif
    return make_unique<vtpty::Process>(profile->shell.value(),
                                       vtpty::createPty(profile->terminalSize.value(), nullopt));
}

TerminalSession* TerminalSessionManager::createSession()
{
    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessable from within QML.
    auto* session = new TerminalSession(createPty(), _app);

    _sessions.push_back(session);

    connect(session, &TerminalSession::sessionClosed, [this, session]() { removeSession(*session); });

    // Claim ownership of this object, so that it will be deleted automatically by the QML's GC.
    //
    // QQmlEngine falsely assumed that the object would not be needed anymore at random times in active
    // sessions. This will work around it, by explicitly claiming ownership of the object.
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);

    // we can close application right after session has been created
    _lastTabChange = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    return session;
}

void TerminalSessionManager::setSession(size_t index)
{
    Require(index <= _sessions.size());
    managerLog()(std::format("SET SESSION: index: {}, _sessions.size(): {}", index, _sessions.size()));
    if (!isAllowedToChangeTabs())
        return;

    auto* oldSession = _activeSession;

    if (index < _sessions.size())
        _activeSession = _sessions[index];
    else
        _activeSession = createSession();

    if (oldSession == _activeSession)
        return;

    if (oldSession)
        oldSession->detachDisplay(*display);

    Require(display != nullptr);
    auto const pixels = display->pixelSize();
    auto const totalPageSize = display->calculatePageSize() + _activeSession->terminal().statusLineHeight();

    display->setSession(_activeSession);
    _activeSession->terminal().resizeScreen(totalPageSize, pixels);
    updateStatusLine();
    _lastTabChange = std::chrono::steady_clock::now();
}

void TerminalSessionManager::addSession()
{
    setSession(_sessions.size());
}

void TerminalSessionManager::switchToTabLeft()
{
    const auto currentSessionIndex = getCurrentSessionIndex();
    managerLog()(std::format("PREVIOUS TAB: currentSessionIndex: {}, _sessions.size(): {}",
                             currentSessionIndex,
                             _sessions.size()));

    if (currentSessionIndex > 0)
    {
        setSession(currentSessionIndex - 1);
    }
}

void TerminalSessionManager::switchToTab(int position)
{
    managerLog()(std::format(
        "switchToTab from {} to {} (out of {})", getCurrentSessionIndex(), position - 1, _sessions.size()));

    if (1 <= position && position <= static_cast<int>(_sessions.size()))
    {
        setSession(position - 1);
    }
}

void TerminalSessionManager::switchToTabRight()
{
    const auto currentSessionIndex = getCurrentSessionIndex();

    managerLog()(std::format(
        "NEXT TAB: currentSessionIndex: {}, _sessions.size(): {}", currentSessionIndex, _sessions.size()));
    if (std::cmp_less(currentSessionIndex, _sessions.size() - 1))
    {
        setSession(currentSessionIndex + 1);
    }
}

void TerminalSessionManager::closeTab()
{
    const auto currentSessionIndex = getCurrentSessionIndex();
    managerLog()(std::format(
        "CLOSE TAB: currentSessionIndex: {}, _sessions.size(): {}", currentSessionIndex, _sessions.size()));

    // Session was removed outside of terminal session manager, we need to switch to another tab.
    if (currentSessionIndex == -1 && !_sessions.empty())
    {
        // We need to switch to another tab, so we permit consequent tab changes.
        // TODO: This is a bit hacky.
        _lastTabChange = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        setSession(0);
        return;
    }

    if (_sessions.size() > 1)
    {

        if (!isAllowedToChangeTabs())
            return;

        removeSession(*_activeSession);

        // We need to switch to another tab, so we permit consequent tab changes.
        // TODO: This is a bit hacky.
        _lastTabChange = std::chrono::steady_clock::now() - std::chrono::seconds(1);

        if (std::cmp_less_equal(currentSessionIndex, _sessions.size() - 1))
        {
            setSession(currentSessionIndex + 1);
        }
        else
        {
            setSession(currentSessionIndex - 1);
        }
    }
}

void TerminalSessionManager::removeSession(TerminalSession& thatSession)
{
    managerLog()(std::format(
        "REMOVE SESSION: session: {}, _sessions.size(): {}", (void*) &thatSession, _sessions.size()));

    if (!isAllowedToChangeTabs())
        return;

    _app.onExit(thatSession); // TODO: the logic behind that impl could probably be moved here.

    auto i = std::find_if(_sessions.begin(), _sessions.end(), [&](auto p) { return p == &thatSession; });
    if (i != _sessions.end())
    {
        _sessions.erase(i);
    }

    updateStatusLine();
    _lastTabChange = std::chrono::steady_clock::now();
    // Notify app if all sessions have been killed to trigger app termination.
}

void TerminalSessionManager::updateColorPreference(vtbackend::ColorPreference const& preference)
{
    for (auto& session: _sessions)
        session->updateColorPreference(preference);
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
