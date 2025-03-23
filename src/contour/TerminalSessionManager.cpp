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

#include <algorithm>
#include <filesystem>
#include <string>

using namespace std::string_literals;

using std::make_unique;
using std::nullopt;

namespace contour
{

TerminalSessionManager::TerminalSessionManager(ContourGuiApp& app): _app { app }, _earlyExitThreshold {}
{
}

std::unique_ptr<vtpty::Pty> TerminalSessionManager::createPty(std::optional<std::string> cwd)
{
    auto const& profile = _app.config().profile(_app.profileName());
#if defined(VTPTY_LIBSSH2)
    if (!profile->ssh.value().hostname.empty())
        return make_unique<vtpty::SshSession>(profile->ssh.value());
#endif
    if (cwd)
        profile->shell.value().workingDirectory = std::filesystem::path(cwd.value());
    return make_unique<vtpty::Process>(profile->shell.value(),
                                       vtpty::createPty(profile->terminalSize.value(), nullopt),
                                       profile->escapeSandbox.value());
}

TerminalSession* TerminalSessionManager::createSessionInBackground()
{
    // TODO: Remove dependency on app-knowledge and pass shell / terminal-size instead.
    // The GuiApp *or* (Global)Config could be made a global to be accessable from within QML.

    if (!activeDisplay)
    {
        managerLog()("No active display found. something went wrong.");
    }

#if !defined(_WIN32)
    auto ptyPath = [this]() -> std::optional<std::string> {
        if (_sessions.empty())
            return std::nullopt;
        auto& terminal = _sessions[0]->terminal();
        if (auto const* ptyProcess = dynamic_cast<vtpty::Process const*>(&terminal.device()))
            return ptyProcess->workingDirectory();
        return std::nullopt;
    }();
#else
    std::optional<std::string> ptyPath = std::nullopt;
    if (_activeSession)
    {
        auto& terminal = _activeSession->terminal();
        {
            auto _l = std::scoped_lock { terminal };
            ptyPath = terminal.currentWorkingDirectory();
        }
    }
#endif

    auto* session = new TerminalSession(this, createPty(ptyPath), _app);
    managerLog()("Create new session with ID {} at index {}", session->id(), _sessions.size());

    _sessions.insert(_sessions.end(), session);

    connect(session, &TerminalSession::sessionClosed, [this, session]() { removeSession(*session); });

    // Claim ownership of this object, so that it will be deleted automatically by the QML's GC.
    //
    // QQmlEngine falsely assumed that the object would not be needed anymore at random times in active
    // sessions. This will work around it, by explicitly claiming ownership of the object.
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);

    return session;
}

void TerminalSessionManager::setSession(size_t index)
{
    Require(index <= _sessions.size());
    managerLog()(std::format("SET SESSION: index: {}, _sessions.size(): {}", index, _sessions.size()));

    if (index < _sessions.size())
        activateSession(_sessions[index]);
    else
        activateSession(createSessionInBackground());
}

TerminalSession* TerminalSessionManager::activateSession(TerminalSession* session, bool isNewSession)
{
    if (!session)
        return nullptr;

    managerLog()(
        "Activating session ID {} at index {}", session->id(), getSessionIndexOf(session).value_or(-1));

    // iterate over _displayStates to see if this session is already active
    for (auto& [display, state]: _displayStates)
    {
        if (state.activeSession == session)
        {
            managerLog()("Session is already active : (display {}, ID {})", (void*) display, session->id());
            // return session;
        }
    }

    if (!activeDisplay)
    {
        managerLog()("No active display fond. something went wrong.");
    }
    else
    {
        managerLog()("Active display found: {}", (void*) activeDisplay);
        _displayStates[activeDisplay].activeSession = session;
        _displayStates.erase(nullptr);
    }

    auto& displayState = _displayStates[activeDisplay];
    displayState.previousActiveSession = displayState.activeSession;
    displayState.activeSession = session;
    updateStatusLine();

    if (session->display())
    {
        managerLog()("Session already has another display attached.");
    }
    if (activeDisplay)
    {
        // check if we are trying to attach to another display, we need to deatach the session from the old
        // display first
        // if (session->display() != activeDisplay)
        // {
        //     managerLog()(
        //         "Deataching display {} from session: {}.", (void*) session->display(), session->id());
        //     if (session->display() != nullptr)
        //         //session->detachDisplay(*session->display());
        // }

        auto const pixels = activeDisplay->pixelSize();
        auto const totalPageSize = activeDisplay->calculatePageSize() + [&] {
            if (displayState.previousActiveSession)
                return displayState.previousActiveSession->terminal().statusLineHeight();
            return vtpty::LineCount(0);
        }();

        // Ensure that the existing session is resized to the display's size.
        if (!isNewSession)
        {
            managerLog()("Resize existing session to display size: {}x{}.",
                         activeDisplay->width(),
                         activeDisplay->height());
        }
        //     displayState.activeSession->terminal().resizeScreen(totalPageSize, pixels);

        managerLog()("Set display {} to session: {}.", (void*) activeDisplay, session->id());
        activeDisplay->setSession(displayState.activeSession);
        managerLog()("Done.");

        // Resize active session after display is attached to it
        // to return a lost line
        displayState.activeSession->terminal().resizeScreen(totalPageSize, pixels);
    }

    return session;
}

TerminalSession* TerminalSessionManager::createSession()
{
    return activateSession(createSessionInBackground(), true /*force resize on before display-attach*/);
}

void TerminalSessionManager::switchToPreviousTab()
{
    managerLog()("switch to previous tab (current: {}, previous: {})",
                 getSessionIndexOf(_displayStates[activeDisplay].activeSession).value_or(-1),
                 getSessionIndexOf(_displayStates[activeDisplay].previousActiveSession).value_or(-1));

    activateSession(_displayStates[activeDisplay].previousActiveSession);
}

void TerminalSessionManager::switchToTabLeft()
{
    const auto currentSessionIndex = getCurrentSessionIndex();
    managerLog()(std::format("previous tab: currentSessionIndex: {}, _sessions.size(): {}",
                             currentSessionIndex,
                             _sessions.size()));

    if (currentSessionIndex > 0)
    {
        setSession(currentSessionIndex - 1);
    }
    else // wrap
    {
        setSession(_sessions.size() - 1);
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
    else // wrap
    {
        setSession(0);
    }
}

void TerminalSessionManager::switchToTab(int position)
{
    managerLog()("switchToTab from index {} to {} (out of {})",
                 getSessionIndexOf(_displayStates[activeDisplay].activeSession).value_or(-1),
                 position - 1,
                 _sessions.size());

    if (1 <= position && position <= static_cast<int>(_sessions.size()))
        activateSession(_sessions[position - 1]);
}

void TerminalSessionManager::closeTab()
{
    managerLog()("Close tab: current session ID {}, index {}",
                 getSessionIndexOf(_displayStates[activeDisplay].activeSession).value_or(-1),
                 _displayStates[activeDisplay].activeSession->id());

    removeSession(*_displayStates[activeDisplay].activeSession);
}

void TerminalSessionManager::moveTabTo(int position)
{
    auto const currentIndexOpt = getSessionIndexOf(_displayStates[activeDisplay].activeSession);
    if (!currentIndexOpt)
        return;

    if (position < 1 || position > static_cast<int>(_sessions.size()))
        return;

    auto const index = static_cast<size_t>(position - 1);

    std::swap(_sessions[currentIndexOpt.value()], _sessions[index]);
    updateStatusLine();
}

void TerminalSessionManager::moveTabToLeft(TerminalSession* session)
{
    auto const maybeIndex = getSessionIndexOf(session);
    if (!maybeIndex)
        return;

    auto const index = maybeIndex.value();

    if (index > 0)
    {
        std::swap(_sessions[index], _sessions[index - 1]);
        updateStatusLine();
    }
}

void TerminalSessionManager::moveTabToRight(TerminalSession* session)
{
    auto const maybeIndex = getSessionIndexOf(session);
    if (!maybeIndex)
        return;

    auto const index = maybeIndex.value();

    if (index + 1 < _sessions.size())
    {
        std::swap(_sessions[index], _sessions[index + 1]);
        updateStatusLine();
    }
}

void TerminalSessionManager::removeSession(TerminalSession& thatSession)
{
    managerLog()("REMOVE SESSION: session: {}, _sessions.size(): {}", (void*) &thatSession, _sessions.size());

    if (&thatSession == _displayStates[activeDisplay].activeSession
        && _displayStates[activeDisplay].previousActiveSession)
        activateSession(_displayStates[activeDisplay].previousActiveSession);

    auto i = std::ranges::find(_sessions, &thatSession);
    if (i == _sessions.end())
    {
        managerLog()("Session not found in session list.");
        return;
    }
    _sessions.erase(i);
    _app.onExit(thatSession); // TODO: the logic behind that impl could probably be moved here.

    _displayStates[activeDisplay].previousActiveSession = [&]() -> TerminalSession* {
        auto const currentIndex = getSessionIndexOf(_displayStates[activeDisplay].activeSession).value_or(0);
        if (currentIndex + 1 < _sessions.size())
            return _sessions[currentIndex + 1];
        else if (currentIndex > 0)
            return _sessions[currentIndex - 1];
        else
            return nullptr;
    }();
    managerLog()("Calculated next \"previous\" session index {}",
                 getSessionIndexOf(_displayStates[activeDisplay].previousActiveSession).value_or(-1));

    updateStatusLine();
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
