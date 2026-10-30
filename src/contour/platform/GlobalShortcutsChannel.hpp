// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The seam between the portal-backed global hotkey and the bus: a channel creates a GlobalShortcuts
/// session, binds shortcuts to it and reports their activation. Qt-free, so PortalGlobalHotkey and its
/// tests never see D-Bus; the Linux adapter is DBusGlobalShortcutsChannel.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace contour::platform
{

/// One shortcut the application asks the desktop to bind.
struct ShortcutBinding
{
    std::string id {};               ///< The application's name for the shortcut, reported on activation.
    std::string description {};      ///< What the desktop shows the user when it asks.
    std::string preferredTrigger {}; ///< The chord the application would like, in the portal's spelling.
};

/// How the portal answered a request, as the Response signal's code spells it.
enum class PortalResponse : uint8_t
{
    Success = 0, ///< The request went through.
    Cancelled,   ///< The user dismissed the desktop's dialog.
    Other,       ///< Anything else, including a portal that is not there.
};

/// Receives the answer to IGlobalShortcutsChannel::createSession: the response and, on Success, the
/// session's handle.
using SessionCreated = std::function<void(PortalResponse, std::string sessionHandle)>;

/// Receives the answer to IGlobalShortcutsChannel::bindShortcuts.
using ShortcutsBound = std::function<void(PortalResponse)>;

/// Receives an activation: the shortcut's id and the desktop's activation token, empty when none came.
using ShortcutActivated = std::function<void(std::string shortcutId, std::string activationToken)>;

/// Talks to the XDG GlobalShortcuts portal.
///
/// Every answer runs on the GUI thread, at most once, and never after the channel is destroyed. An answer
/// may run before the call that asked for it returns (the null channel's does), so a caller must be ready.
class IGlobalShortcutsChannel
{
  public:
    IGlobalShortcutsChannel() = default;
    IGlobalShortcutsChannel(IGlobalShortcutsChannel const&) = delete;
    IGlobalShortcutsChannel& operator=(IGlobalShortcutsChannel const&) = delete;
    IGlobalShortcutsChannel(IGlobalShortcutsChannel&&) = delete;
    IGlobalShortcutsChannel& operator=(IGlobalShortcutsChannel&&) = delete;
    virtual ~IGlobalShortcutsChannel() = default;

    /// Asks for a session and returns without waiting for the answer.
    /// @param onCreated Called with the response and the session handle.
    virtual void createSession(SessionCreated onCreated) = 0;

    /// Binds @p shortcuts to a session and returns without waiting for the answer.
    /// @param sessionHandle The handle createSession() answered with.
    /// @param shortcuts     What to bind.
    /// @param onBound       Called with the response.
    virtual void bindShortcuts(std::string const& sessionHandle,
                               std::vector<ShortcutBinding> const& shortcuts,
                               ShortcutsBound onBound) = 0;

    /// Closes a session, and with it every shortcut bound to it. Does not wait.
    /// @param sessionHandle The handle createSession() answered with.
    virtual void closeSession(std::string const& sessionHandle) = 0;

    /// Installs the receiver of activations. Called once, by the object that owns the channel.
    /// @param onActivated Called with each activation of a shortcut of this application's sessions.
    virtual void onActivated(ShortcutActivated onActivated) = 0;
};

/// The channel where there is no portal to talk to: it answers Other at once and never activates.
class NullGlobalShortcutsChannel final: public IGlobalShortcutsChannel
{
  public:
    /// Answers Other before returning.
    void createSession(SessionCreated onCreated) override
    {
        if (onCreated)
            onCreated(PortalResponse::Other, {});
    }

    /// Answers Other before returning.
    void bindShortcuts(std::string const& /*sessionHandle*/,
                       std::vector<ShortcutBinding> const& /*shortcuts*/,
                       ShortcutsBound onBound) override
    {
        if (onBound)
            onBound(PortalResponse::Other);
    }

    /// Does nothing.
    void closeSession(std::string const& /*sessionHandle*/) override {}

    /// Drops the receiver: nothing activates.
    void onActivated(ShortcutActivated /*onActivated*/) override {}
};

} // namespace contour::platform
