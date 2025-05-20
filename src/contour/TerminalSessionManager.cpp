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

    if (!_allowCreation)
    {
        managerLog()("Session creation is disabled.");
        // try to find for the selected display a session to use

        for (auto& session: _sessions)
        {
            if (_displayStates[activeDisplay] == session)
            {
                managerLog()("Found suitable session Returning it.");
                return session;
            }
        }
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
    if (!_sessions.empty())
    {
        auto& terminal = _sessions[0]->terminal();
        {
            auto _l = std::scoped_lock { terminal };
            ptyPath = terminal.currentWorkingDirectory();
        }
    }
#endif

    auto* session = new TerminalSession(this, createPty(ptyPath), _app);
    managerLog()(
        "Create new session with ID {}({}) at index {}", session->id(), (void*) session, _sessions.size());

    _sessions.insert(_sessions.end(), session);

    connect(session, &TerminalSession::sessionClosed, [this, session]() { removeSession(*session); });

    // Claim ownership of this object, so that it will be deleted automatically by the QML's GC.
    //
    // QQmlEngine falsely assumed that the object would not be needed anymore at random times in active
    // sessions. This will work around it, by explicitly claiming ownership of the object.
    QQmlEngine::setObjectOwnership(session, QQmlEngine::CppOwnership);

    _allowCreation = false;
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

    // debug for displayStates
    for (auto& [display, state]: _displayStates)
    {
        managerLog()("display: {}, session: {}\n", (void*) display, (void*) state);
    }

    managerLog()("Activating session ID {} {} at index {}",
                 session->id(),
                 (void*) session,
                 getSessionIndexOf(session).value_or(-1));

    // iterate over _displayStates to see if this session is already active
    for (auto& [display, state]: _displayStates)
    {
        if (state == session && (nullptr != display))
        {
            managerLog()("Session is already active : (display {}, ID {} {})",
                         (void*) display,
                         session->id(),
                         (void*) session);
            return session;
        }
    }

    if (!activeDisplay)
    {
        managerLog()("No active display found. something went wrong.");
    }

    auto& displayState = _displayStates[activeDisplay];
    displayState = session;
    updateStatusLine();

    if (activeDisplay)
    {

        auto const pixels = activeDisplay->pixelSize();
        auto const totalPageSize =
            activeDisplay->calculatePageSize() + displayState->terminal().statusLineHeight();

        // Ensure that the existing session is resized to the display's size.
        if (!isNewSession)
        {
            managerLog()("Resize existing session to display size: {}x{}.",
                         activeDisplay->width(),
                         activeDisplay->height());
            displayState->terminal().resizeScreen(totalPageSize, pixels);
        }

        managerLog()(
            "Set display {} to session: {}({}).", (void*) activeDisplay, session->id(), (void*) session);
        // resize terminal session before display is attached to it
        activeDisplay->setSession(displayState);

        // Resize active session after display is attached to it
        // to return a lost line
        displayState->terminal().resizeScreen(totalPageSize, pixels);
    }

    return session;
}

void TerminalSessionManager::FocusOnDisplay(display::TerminalDisplay* display)
{
    managerLog()("Setting active display to {}", (void*) display);
    activeDisplay = display;

    // if we have a session in nullptr display, set it to this one
    if (_displayStates[nullptr] != nullptr)
    {
        _displayStates[activeDisplay] = _displayStates[nullptr];
        _displayStates[nullptr] = nullptr;
        activateSession(_displayStates[activeDisplay]);
    }

    // if this is new display, find a session to attach to
    if (_displayStates[activeDisplay] == nullptr)
    {
        tryFindSessionForDisplayOrClose();
    }
}

TerminalSession* TerminalSessionManager::createSession()
{
    return activateSession(createSessionInBackground(), true /*force resize on before display-attach*/);
}

void TerminalSessionManager::switchToPreviousTab()
{
    return;
    // managerLog()("switch to previous tab (current: {}, previous: {})",
    //              getSessionIndexOf(_displayStates[activeDisplay].activeSession).value_or(-1),
    //              getSessionIndexOf(_displayStates[activeDisplay].previousActiveSession).value_or(-1));

    // activateSession(_displayStates[activeDisplay].previousActiveSession);
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
                 getSessionIndexOf(_displayStates[activeDisplay]).value_or(-1),
                 position - 1,
                 _sessions.size());

    if (1 <= position && position <= static_cast<int>(_sessions.size()))
        activateSession(_sessions[position - 1]);
}

void TerminalSessionManager::closeTab()
{
    managerLog()("Close tab: current session ID {}, index {}",
                 getSessionIndexOf(_displayStates[activeDisplay]).value_or(-1),
                 _displayStates[activeDisplay]->id());

    removeSession(*_displayStates[activeDisplay]);
}

void TerminalSessionManager::moveTabTo(int position)
{
    auto const currentIndexOpt = getSessionIndexOf(_displayStates[activeDisplay]);
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

void TerminalSessionManager::currentSessionIsTerminated()
{
    managerLog()("got notified that session is terminated, number of existing sessions: _sessions.size(): {}",
                 _sessions.size());
    return;
}

void TerminalSessionManager::removeSession(TerminalSession& thatSession)
{
    managerLog()("remove session: session: {}, _sessions.size(): {}", (void*) &thatSession, _sessions.size());

    auto i = std::ranges::find(_sessions, &thatSession);
    if (i == _sessions.end())
    {
        managerLog()("Session not found in session list.");
        return;
    }
    _sessions.erase(i);
    tryFindSessionForDisplayOrClose();
    //_app.onExit(thatSession); // TODO: the logic behind that impl could probably be moved here.
}

void TerminalSessionManager::tryFindSessionForDisplayOrClose()
{
    for (auto& session: _sessions)
    {
        bool saveToSwitch { true };
        // check if session is not used by any display and then switch
        for (auto& [display, state]: _displayStates)
        {
            if ((state == session) && (display != nullptr))
            {
                saveToSwitch = false;
                break;
            }
        }

        if (saveToSwitch)
        {
            managerLog()("Switching to session: {}", (void*) session);
            activateSession(session);
            return;
        }
    }

    updateStatusLine();
    _displayStates.erase(activeDisplay);
    activeDisplay->closeDisplay();
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
