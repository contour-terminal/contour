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

#include <memory>

class QQmlEngine;

namespace contour::window
{

/// A PromptHost on the router's BootstrapRoute and the small QML window (PromptWindow.qml) that shows it. The
/// window is shown only while a prompt is open. Once a terminal window has a sink of its own, the router
/// sends new prompts there and this one stays hidden.
class BootstrapPrompt
{
  public:
    /// @param router Where the host attaches; shared.
    /// @param engine The QML engine the window is made on; it owns the window, so the engine ends it first.
    /// @return The stand-in, or null when its window could not be made.
    [[nodiscard]] static std::unique_ptr<BootstrapPrompt> create(
        std::shared_ptr<askpass::AskpassPromptRouter> router, QQmlEngine& engine);

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

} // namespace contour::window
