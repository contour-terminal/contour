// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The stand-in window for ssh's prompts: `contour client --host` attaches before its first terminal window
/// exists, and the attach cannot settle before the user has answered, so the prompts need a window of their
/// own until a terminal window can take them.

#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/window/PromptHost.hpp>

#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <functional>
#include <memory>

class QQmlEngine;

namespace contour::window
{

/// A PromptHost on the router's BootstrapRoute and the small QML window (PromptWindow.qml) that shows it. The
/// window is shown only while a prompt is open. Once a terminal window has a sink of its own, the router
/// sends new prompts there and this one stays hidden. It is not meant to outlive its prompts: it says when
/// the last one is answered, so that its owner can end it rather than keep a hidden window.
class BootstrapPrompt
{
  public:
    /// @param router Where the host attaches; shared.
    /// @param engine The QML engine the window is made on; it owns the window, so the engine ends it first.
    /// @param onDrained Called each time the host is left with no prompt to show, from inside the answer that
    ///        left it so. The owner ends the stand-in later, not from within the call, and only if nothing is
    ///        asked of it meanwhile.
    /// @return The stand-in, or null when its window could not be made.
    [[nodiscard]] static std::unique_ptr<BootstrapPrompt> create(
        std::shared_ptr<askpass::AskpassPromptRouter> router,
        QQmlEngine& engine,
        std::function<void()> onDrained);

    /// Takes the window down if the engine still has it, then the host, which cancels what it still holds.
    ~BootstrapPrompt();

    BootstrapPrompt(BootstrapPrompt const&) = delete;
    BootstrapPrompt& operator=(BootstrapPrompt const&) = delete;
    BootstrapPrompt(BootstrapPrompt&&) = delete;
    BootstrapPrompt& operator=(BootstrapPrompt&&) = delete;

    /// @return The window; null once the engine took it down.
    [[nodiscard]] QObject* window() const noexcept { return _window.data(); }

    /// @return The host that holds the prompts the window shows.
    [[nodiscard]] PromptHost& host() noexcept { return _host; }

  private:
    explicit BootstrapPrompt(std::shared_ptr<askpass::AskpassPromptRouter> router):
        _host { std::move(router), askpass::BootstrapRoute }
    {
    }

    PromptHost _host;
    QPointer<QObject> _window;
};

/// Makes the stand-in. The app's own makes it over the app's QML engine; a test makes it over an engine of
/// its own, as the app builds none there.
/// @param router Where the stand-in's host attaches.
/// @param onDrained As BootstrapPrompt::create() takes it.
/// @return The stand-in, or null when it cannot be made (no engine yet, a window that does not load).
using BootstrapPromptFactory = std::function<std::unique_ptr<BootstrapPrompt>(
    std::shared_ptr<askpass::AskpassPromptRouter> router, std::function<void()> onDrained)>;

} // namespace contour::window
