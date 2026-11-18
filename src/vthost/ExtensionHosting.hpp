// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The seams through which a `SessionHost` runs extensions: status-line extensions in the sessions it hosts
/// (IExtensionHosting), and the daemon-scoped extensions that run beside them, with no session or window
/// (IDaemonWorldHosting).
///
/// vthost names its own interface rather than vtextension's types, and this header includes nothing of
/// vtextension, for two reasons. A build without extensions links no vtextension, and every class here must
/// keep one layout in both builds, so that no translation unit can disagree with another about a
/// `SessionHost` (an ODR split the GUI's sessions already met once). And a test can stand a fake in for the
/// whole runtime, to drive what a host does with a frame without running WebAssembly.
///
/// The production implementations are `DaemonExtensions` (vthost/DaemonExtensions.hpp) and
/// `DaemonWorldExtensions` (vthost/DaemonWorldExtensions.hpp), built only with extensions; `contour daemon`
/// makes them from its configuration. Without one, a host constructs, polls and wakes
/// nothing of extensions at all.

#include <vtbackend/screen/StatusLineExtensions.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vthost/ConnectionId.hpp>
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

    /// Serves a control verb an extension calls, as the session's pane: the method, its request JSON, what
    /// the extension is granted, and the longest to wait. Called on the extension's lane, and blocks it until
    /// the host's loop answers or the time is up (@see control::LoopControlBridge).
    std::function<std::expected<std::string, vtcontrol::ControlError>(
        std::string_view, std::string_view, vtcontrol::ControlGrants, std::chrono::milliseconds)>
        callControl {};
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

/// What the daemon-scoped extensions tell their host. Each may be called on any thread, and must be cheap.
struct DaemonWorldEvents
{
    /// The set of extensions waiting for the user's consent changed.
    std::function<void()> requestsChanged;
    /// What an extension shows of itself changed: it published a new summary, or it failed to start.
    std::function<void()> summaryChanged;
    /// What the palette shows of an extension changed: where it stands, or its declared actions.
    std::function<void()> presenceChanged {};
};

/// A daemon-scoped extension waiting for its user's grant, and the question the daemon rendered for it.
struct DaemonExtensionRequest
{
    std::string name;        ///< What the daemon's configuration calls it.
    std::string grantDigest; ///< What an answer is recorded for: its manifest and resolved roots.
    std::string title;       ///< The question's title.
    /// The question's lines, each with its weight as the wire spells vtcontrol::ConsentWeight.
    std::vector<std::pair<std::string, uint8_t>> lines;

    bool operator==(DaemonExtensionRequest const&) const = default;
};

/// Where a daemon-scoped extension stands.
enum class DaemonExtensionState : uint8_t
{
    Stopped = 0, ///< Not running, and not waiting for its user: it waits for nothing to ask for it.
    Pending,     ///< It was to start, and waits for its user's grant.
    Denied,      ///< Its user refused it.
    Running,     ///< It runs (or recovers from a fault).
    Disabled,    ///< It faulted too often, and runs no more until started again.
    Failed,      ///< It was granted and started, and could not run (its module does not load, for one).
};

/// One daemon-scoped extension, as `extension.list` shows it.
struct DaemonExtensionListing
{
    std::string name;                                           ///< What the daemon's configuration calls it.
    DaemonExtensionState state = DaemonExtensionState::Stopped; ///< Where it stands.
    std::string grantDigest;                                    ///< What its grant is recorded for.
    std::optional<DaemonExtensionRequest> pending;              ///< Its question, while it is Pending.
    std::string problem; ///< Why it could not run, neutralized, while it is Failed; empty otherwise.

    bool operator==(DaemonExtensionListing const&) const = default;
};

/// What one daemon-scoped extension shows of itself, as `extension.status` reports it.
struct DaemonExtensionStatus
{
    DaemonExtensionState state = DaemonExtensionState::Stopped; ///< Where it stands.
    std::string summary; ///< The one-line summary it last published, neutralized; empty for none.
    std::string detail;  ///< The longer text it last published, neutralized, `\n` between its lines.
    std::string problem; ///< Why it could not run, neutralized, while it is Failed; empty otherwise.

    bool operator==(DaemonExtensionStatus const&) const = default;
};

/// Why vthost calls a daemon-scoped extension: what it raises for one, in its own words (vtextension's
/// DaemonHint has more, which vthost never raises).
enum class ServedHint : uint8_t
{
    Tabs,    ///< Its served tabs changed.
    Actions, ///< The user invoked its actions (IExtensionActions::take()).
};

/// One daemon-scoped extension as the palette shows it: its declared `action` lines, and where it stands --
/// which decides what invoking one of them does.
struct ExtensionActionRows
{
    std::string extension;                                      ///< What the daemon's configuration calls it.
    std::string title;                                          ///< Its manifest's `title`, else its name.
    DaemonExtensionState state = DaemonExtensionState::Stopped; ///< Where it stands.
    std::vector<std::pair<std::string, std::string>> rows = {}; ///< Each declared action: its id and title.

    bool operator==(ExtensionActionRows const&) const = default;
};

/// Which connection an invoked action came from, as an extension is told it (the WIT `origin`, a `u32`).
/// Minted once per connection, never reused.
using Origin = uint32_t;

/// The actions the user invoked, per extension, with the connection each came from: what an extension's
/// `actions-take` reads.
class IExtensionActions
{
  public:
    IExtensionActions() = default;
    IExtensionActions(IExtensionActions const&) = delete;
    IExtensionActions(IExtensionActions&&) = delete;
    IExtensionActions& operator=(IExtensionActions const&) = delete;
    IExtensionActions& operator=(IExtensionActions&&) = delete;
    virtual ~IExtensionActions() = default;

    /// Takes the actions invoked for @p extension since the last take, oldest first. From any thread.
    /// @param extension What the daemon's configuration calls it.
    /// @return Each action's id and the origin it came from.
    [[nodiscard]] virtual std::vector<std::pair<std::string, Origin>> take(std::string_view extension) = 0;

    /// From any thread. An origin is bound to the extension it was handed to: one extension cannot act for
    /// a window through an origin another one was given.
    /// @param extension What the daemon's configuration calls the extension asking.
    /// @param origin An origin take() handed out.
    /// @return The connection it stands for; nullopt once that connection detached, or for an origin take()
    ///         never handed @p extension.
    [[nodiscard]] virtual std::optional<ConnectionId> connectionOf(std::string_view extension,
                                                                   Origin origin) const = 0;
};

/// What asking the windows again came to.
enum class AskOutcome : uint8_t
{
    NotPending = 0, ///< The extension waits for no answer: nothing was sent.
    Asked,          ///< Its question was sent again.
};

/// What IDaemonExtensionControl::ask() did.
struct AskResult
{
    AskOutcome outcome = AskOutcome::NotPending; ///< Whether it asked.
    std::size_t windows = 0;                     ///< How many connections that can answer were sent it.

    bool operator==(AskResult const&) const = default;
};

/// What the control API may do with daemon-scoped extensions: look, ask the windows again, and revoke. Never
/// record an answer: a grant completes only as a confirmation in a Contour window, since the control socket
/// is reachable from every pane. A window is a connection that proved the daemon's consent key
/// (DaemonConsentKey.hpp), so no extension, no control verb, and no process that merely inherits a pane's
/// environment can grant; a process of the same user that reads the key file on purpose can still pass for a
/// window, as it could fake key presses -- the user is the trust boundary. Implemented by SessionHost; loop
/// thread only.
class IDaemonExtensionControl
{
  public:
    IDaemonExtensionControl() = default;
    IDaemonExtensionControl(IDaemonExtensionControl const&) = delete;
    IDaemonExtensionControl(IDaemonExtensionControl&&) = delete;
    IDaemonExtensionControl& operator=(IDaemonExtensionControl const&) = delete;
    IDaemonExtensionControl& operator=(IDaemonExtensionControl&&) = delete;
    virtual ~IDaemonExtensionControl() = default;

    /// @return Every declared extension that composed, in the configuration's order, its question when it is
    ///         pending.
    [[nodiscard]] virtual std::vector<DaemonExtensionListing> list() const = 0;

    /// Sends the extension's pending question again to every connection that can answer it.
    /// @param name What the configuration calls it.
    /// @return Asked, and to how many windows; NotPending when it waits for no answer.
    [[nodiscard]] virtual AskResult ask(std::string_view name) = 0;

    /// Forgets the user's answer for the extension's manifest and roots and stops it; it is asked again.
    /// @param name What the configuration calls it.
    virtual void revoke(std::string_view name) = 0;

    /// @param name What the configuration calls an extension.
    /// @return Where it stands, what it last published of itself, and why it failed if it did; nullopt for a
    ///         name that does not compose.
    [[nodiscard]] virtual std::optional<DaemonExtensionStatus> status(std::string_view name) const = 0;
};

/// A daemon's daemon-scoped extensions: the ones that run beside its sessions, with no session or window.
/// Loop thread only, unless a member says otherwise.
class IDaemonWorldHosting
{
  public:
    IDaemonWorldHosting() = default;
    IDaemonWorldHosting(IDaemonWorldHosting const&) = delete;
    IDaemonWorldHosting(IDaemonWorldHosting&&) = delete;
    IDaemonWorldHosting& operator=(IDaemonWorldHosting const&) = delete;
    IDaemonWorldHosting& operator=(IDaemonWorldHosting&&) = delete;
    virtual ~IDaemonWorldHosting() = default;

    /// Starts an extension that waits until something asks for it; one the user has not granted waits for
    /// the user's consent instead.
    /// @param name What the configuration calls it.
    virtual void startOnDemand(std::string_view name) = 0;

    /// @return The extensions waiting for their user's grant, in the configuration's order.
    [[nodiscard]] virtual std::vector<DaemonExtensionRequest> pendingRequests() const = 0;

    /// @return Every extension that composed, in the configuration's order.
    [[nodiscard]] virtual std::vector<DaemonExtensionListing> list() const = 0;

    /// Records a user's answer to a pending request, and starts the extension once granted. One to no pending
    /// request is ignored. SessionHost::answerDaemonExtension() is its only caller, and is reached only with
    /// a ticket a window's connection was given (ConsentTicket).
    /// @param name What the configuration calls the extension.
    /// @param grantDigest DaemonExtensionRequest::grantDigest.
    /// @param answer Granted or Denied.
    virtual void answer(std::string_view name, std::string_view grantDigest, GrantState answer) = 0;

    /// Forgets the user's answer for the extension's manifest and roots and stops it; it is pending again.
    /// What it published of itself is forgotten with it.
    /// @param name What the configuration calls it.
    virtual void revoke(std::string_view name) = 0;

    /// @param name What the configuration calls an extension.
    /// @return Where it stands, what it last published of itself, and why it failed if it did; nullopt for a
    ///         name that does not compose.
    [[nodiscard]] virtual std::optional<DaemonExtensionStatus> status(std::string_view name) const = 0;

    /// @return What the hosted terminals' `{DaemonExtension}` items read: each extension's one-line summary,
    ///         or why it failed. Readable from any thread; lives as long as this. DaemonWorldEvents's
    ///         summaryChanged tells when it changed.
    [[nodiscard]] virtual vtbackend::IDaemonExtensionSummaries const& summaries() const noexcept = 0;

    /// @return Every extension that composed, in the configuration's order: its declared actions, and where
    ///         it stands. DaemonWorldEvents's presenceChanged tells when it changed.
    [[nodiscard]] virtual std::vector<ExtensionActionRows> actionRows() const = 0;

    /// Raises @p hint for a running extension; does nothing for one that does not run. Never waits for it.
    /// @param name What the configuration calls it.
    /// @param hint Why it is to be called.
    virtual void hint(std::string_view name, ServedHint hint) = 0;

    /// Starts a disabled extension again, with a fresh fault window; does nothing unless it is disabled.
    /// @param name What the configuration calls it.
    virtual void restart(std::string_view name) = 0;

    /// Stops every extension for good, for a host about to go: joins the thread they run on, then cancels
    /// the processes in flight. Called by the host before it drains its loop of what they posted; after it,
    /// nothing of this posts any more. Idempotent.
    virtual void stop() noexcept = 0;
};

/// Makes a daemon's daemon-scoped extensions, once its loop exists.
/// @param events What the host hears from them.
/// @param post Where they hand jobs to the loop.
/// @return The extensions.
using DaemonWorldFactory =
    std::function<std::shared_ptr<IDaemonWorldHosting>(DaemonWorldEvents events, OwnerPost post)>;

} // namespace vthost
