// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The seam through which a `SessionHost` runs status-line extensions in the sessions it hosts.
///
/// vthost names its own interface rather than vtextension's types, and this header includes nothing of
/// vtextension, for two reasons. A build without extensions links no vtextension, and every class here must
/// keep one layout in both builds, so that no translation unit can disagree with another about a
/// `SessionHost` (an ODR split the GUI's sessions already met once). And a test can stand a fake in for the
/// whole runtime, to drive what a host does with a frame without running WebAssembly.
///
/// The production implementation is `DaemonExtensions` (vthost/DaemonExtensions.hpp), built only with
/// extensions; `contour daemon` makes it from the profile. Without one, a host constructs, polls and wakes
/// nothing of extensions at all.

#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <functional>
#include <memory>
#include <string_view>

#include <vthost/PermissionWire.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Hands @p job to the thread that owns a hosted session -- the host's loop thread -- from any thread. A
/// job posted once the session is gone is dropped, unrun.
using OwnerPost = std::function<void(std::function<void()> job)>;

/// What a hosted session's status-line extensions tell their host.
struct SessionExtensionEvents
{
    /// An extension's command was refused as pending: its user is to be asked. @p request names no session;
    /// the host fills it in. Called on the extension's lane: cheap, never throwing, never waiting.
    std::function<void(proto::PermissionRequest request)> onPermissionNeeded;

    /// Other extensions run now (a build landed): the terminal must be told. Called on the owner's thread,
    /// holding no lock.
    std::function<void()> onReplaced;
};

/// One hosted session's status-line extensions, as its host drives them. Owned by the session, and declared
/// before its terminal, which it outlives.
class ISessionExtensions
{
  public:
    ISessionExtensions() = default;
    ISessionExtensions(ISessionExtensions const&) = delete;
    ISessionExtensions(ISessionExtensions&&) = delete;
    ISessionExtensions& operator=(ISessionExtensions const&) = delete;
    ISessionExtensions& operator=(ISessionExtensions&&) = delete;
    virtual ~ISessionExtensions() = default;

    /// @return What the session's terminal is built with; both outlive the terminal.
    [[nodiscard]] virtual vtbackend::StatusLineExtensionServices terminalServices() noexcept = 0;

    /// Starts building the extensions, once the terminal exists. Returns at once: they are found, read and
    /// loaded on another thread, and run once handed back (SessionExtensionEvents::onReplaced).
    virtual void start() = 0;

    /// @return The event kinds the running extensions want.
    [[nodiscard]] virtual vtbackend::StatusLineEventKinds subscriptions() const noexcept = 0;
};

/// A daemon's status-line extensions: what every session it hosts runs, and where its users' answers to
/// their permission requests are kept. Loop thread only, unless a member says otherwise.
class IExtensionHosting
{
  public:
    IExtensionHosting() = default;
    IExtensionHosting(IExtensionHosting const&) = delete;
    IExtensionHosting(IExtensionHosting&&) = delete;
    IExtensionHosting& operator=(IExtensionHosting const&) = delete;
    IExtensionHosting& operator=(IExtensionHosting&&) = delete;
    virtual ~IExtensionHosting() = default;

    /// Makes a new session's extensions, built once ISessionExtensions::start() is called.
    /// @param events What the session's host hears from them.
    /// @param post Where their builds are handed back to.
    /// @return The session's extensions.
    [[nodiscard]] virtual std::unique_ptr<ISessionExtensions> createSession(SessionExtensionEvents events,
                                                                            OwnerPost post) = 0;

    /// Records a user's answer to a permission request, for every session.
    /// @param hostId The installed extension that asked.
    /// @param digest The manifest digest the answer is bound to.
    /// @param answer The answer; Pending (a dismissed prompt) records nothing.
    virtual void answer(std::string_view hostId, std::string_view digest, GrantState answer) = 0;

    /// @param hostId An installed extension.
    /// @param digest A manifest digest.
    /// @return The answer recorded for them; Pending for none.
    [[nodiscard]] virtual GrantState lookup(std::string_view hostId, std::string_view digest) const = 0;

    /// Releases, on the loop thread, the engines and modules runners retired since.
    virtual void releaseRetired() noexcept = 0;

    /// Stops every extension for good, for a host about to go: cancels the commands in flight, then joins
    /// the threads builds and runners run on, keeping alive what they reach. Called by the host once its
    /// sessions -- and so every runner -- are gone, and before it drains its loop of what they posted; after
    /// it, nothing of this posts any more, and no session is created. Idempotent.
    virtual void stop() noexcept = 0;
};

} // namespace vthost
