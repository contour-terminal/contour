// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/TerminalManager.h>

#include <cassert>
#include <thread>

#if !defined(_WIN32)
    #include <pthread.h>
#endif

namespace vtbackend
{

namespace
{
    void setThreadName(char const* name)
    {
#if defined(__APPLE__)
        pthread_setname_np(name);
#elif !defined(_WIN32)
        pthread_setname_np(pthread_self(), name);
#endif
    }
} // namespace

TerminalManager::TerminalManager(PtyCreator ptyCreator, Terminal::Events& eventListener):
    _ptyCreator { std::move(ptyCreator) }, _eventListener { eventListener }
{

    // TODO(pr) think about dropping ptyCreator
}

TerminalManager::~TerminalManager()
{
    terminate();
}

void TerminalManager::terminate()
{
    // Kill sessions in reverse roder
    while (!empty())
        closeTab(size() - 1);
}

void TerminalManager::createTab(std::unique_ptr<vtpty::Pty> pty, vtbackend::Settings factorySettings)
{
    _sessions.emplace_back(*this, _eventListener, std::move(pty), std::move(factorySettings));
}

void TerminalManager::switchToTab(size_t index)
{
    _activeSessionIndex = index;
}

void TerminalManager::switchToTabLeft()
{
    if (_activeSessionIndex > 0)
        _activeSessionIndex = _activeSessionIndex - 1;
    else
        _activeSessionIndex = _sessions.size() - 1;
}

void TerminalManager::switchToTabRight()
{
    _activeSessionIndex = (_activeSessionIndex + 1) % _sessions.size();
}

void TerminalManager::closeTab(size_t index)
{
    assert(index < _sessions.size());

    getSession(index).device().wakeupReader();

    _sessions.erase(std::next(_sessions.begin(), static_cast<int>(index)));

    if (_activeSessionIndex >= _sessions.size())
        _activeSessionIndex = _sessions.size() - 1;
}

bool TerminalManager::refreshRenderBuffer()
{
    assert(!empty());
    return getActiveTerminal().refreshRenderBuffer();
}

RenderBufferRef TerminalManager::renderBuffer() const
{
    assert(!empty());
    return getActiveTerminal().renderBuffer();
}

void TerminalManager::instanceMainLoop(std::reference_wrapper<Terminal> terminal)
{
    setThreadName("Terminal.Loop");

    std::string const threadIdString = [] {
        std::stringstream sstr;
        sstr << std::this_thread::get_id();
        return sstr.str();
    }();

    terminalLog()("Starting terminal main loop with thread id {}", threadIdString);

    terminal.get().device().start();

    while (!_terminating)
    {
        if (!terminal.get().processInputOnce())
            break;
    }

    terminalLog()("Event loop terminating for {} (PTY {}).",
                  threadIdString,
                  terminal.get().device().isClosed() ? "closed" : "open");
}

} // namespace vtbackend
