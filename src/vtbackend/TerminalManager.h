// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/Terminal.h>

#include <vtpty/Pty.h>

#include <atomic>
#include <list>
#include <memory>

namespace vtbackend
{

/// Manages multiple terminal sessions (single, tabbed, etc.)
///
/// This class is responsible for managing multiple terminal sessions, such as
/// - single session,
/// - tabbed sessions,
/// - split sessions (currently not yet implemented).
///
/// The frontend (e.g. GUI) is responsible for rendering the active session through the render buffer
/// provided by the active session through this manager.
///
/// TODO: Evaluate if this is sufficient (performance wise), or if we should cache the iterator
///       to the active session.
class TerminalManager
{
  public:
    /// Function that creates a new PTY instance.
    ///
    /// This can be used to create different types of PTY instances, such as
    /// - local PTY,
    /// - SSH PTY,
    /// - Docker PTY,
    /// - etc.
    using PtyCreator = std::function<std::unique_ptr<vtpty::Pty>()>;

    /// Constructs a new terminal manager.
    ///
    /// @param ptyCreator      A function that creates a new PTY instance.
    /// @param eventListener   The event listener that will be notified about terminal events.
    explicit TerminalManager(PtyCreator ptyCreator, Terminal::Events& eventListener);

    ~TerminalManager();

    void terminate();

    void createTab(std::unique_ptr<vtpty::Pty> pty, vtbackend::Settings factorySettings);
    void switchToTab(size_t index);
    void switchToTabLeft();
    void switchToTabRight();
    void closeTab(size_t index);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] Terminal& getActiveTerminal();
    [[nodiscard]] Terminal const& getActiveTerminal() const;
    [[nodiscard]] Terminal& getSession(size_t index);
    [[nodiscard]] Terminal const& getSession(size_t index) const;

    [[nodiscard]] auto begin();
    [[nodiscard]] auto end();
    [[nodiscard]] auto begin() const;
    [[nodiscard]] auto end() const;

    /// Ensures that the render buffer is up-to-date
    [[nodiscard]] bool refreshRenderBuffer();

    /// Returns the render buffer of the active session
    [[nodiscard]] RenderBufferRef renderBuffer() const;

  private:
    void instanceMainLoop(std::reference_wrapper<Terminal> terminal);

    struct Session
    {
        Terminal terminal;
        std::thread thread;
        Session(TerminalManager& self,
                Terminal::Events& eventListener,
                std::unique_ptr<vtpty::Pty> pty,
                vtbackend::Settings factorySettings):
            terminal {
                eventListener, std::move(pty), std::move(factorySettings), std::chrono::steady_clock::now()
            },
            thread { &TerminalManager::instanceMainLoop, &self, std::ref(terminal) }
        {
        }

        ~Session()
        {
            terminal.device().close();
            thread.join();
        }
    };

    PtyCreator _ptyCreator;
    Terminal::Events& _eventListener;
    std::atomic<bool> _terminating = false;
    std::size_t _activeSessionIndex = 0;
    std::list<Session> _sessions;
};

// {{{ inlines
inline bool TerminalManager::empty() const noexcept
{
    return _sessions.empty();
}

inline size_t TerminalManager::size() const noexcept
{
    return _sessions.size();
}

inline Terminal& TerminalManager::getActiveTerminal()
{
    return getSession(_activeSessionIndex);
}

inline Terminal const& TerminalManager::getActiveTerminal() const
{
    return getSession(_activeSessionIndex);
}

inline Terminal& TerminalManager::getSession(size_t index)
{
    return std::next(std::begin(_sessions), static_cast<int>(index))->terminal;
}

inline Terminal const& TerminalManager::getSession(size_t index) const
{
    return std::next(std::begin(_sessions), static_cast<int>(index))->terminal;
}

inline auto TerminalManager::begin()
{
    return _sessions.begin();
}

inline auto TerminalManager::end()
{
    return _sessions.end();
}

inline auto TerminalManager::begin() const
{
    return _sessions.begin();
}

inline auto TerminalManager::end() const
{
    return _sessions.end();
}

// }}}

} // namespace vtbackend
