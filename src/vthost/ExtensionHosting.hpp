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
#include <vtbackend/vt/ProgramStatus.hpp>

#include <vtpty/DirectoryHandle.hpp>

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
    std::string summary;      ///< The one-line summary it last published, neutralized; empty for none.
    std::string detail;       ///< The longer text it last published, neutralized, `\n` between its lines.
    std::string problem;      ///< Why it could not run, neutralized, while it is Failed; empty otherwise.
    std::string lastFault {}; ///< Its run's latest fault, neutralized; empty if it never faulted.

    bool operator==(DaemonExtensionStatus const&) const = default;
};

/// Why vthost calls a daemon-scoped extension: what it raises for one, in its own words (vtextension's
/// DaemonHint has more, which vthost never raises).
enum class ServedHint : uint8_t
{
    Tabs,    ///< Its served tabs changed.
    Actions, ///< The user invoked its actions (IExtensionActions::take()).
    Panes,   ///< Its agent panes changed: opened, gone, or their status (IOwnedPanes::list()).
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

/// How long after a user action a reveal it produced still counts as that user's: after an action invoked
/// through an origin, or a key typed or a button pressed into one of an extension's served tabs. Past it the
/// user is doing something else, and a window raised then would take the keys meant for it.
inline constexpr auto ServedInputGrace = std::chrono::seconds { 2 };

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

    /// Claims @p origin for one reveal, from any thread. An origin authorises a reveal -- a window raised, a
    /// tab activated -- only for ServedInputGrace after the action the user invoked through it, and once:
    /// the first reveal consumes it, and the next action the user invokes allows the next one.
    /// @param extension What the daemon's configuration calls the extension asking.
    /// @param origin An origin it names.
    /// @return The connection @p origin stands for, if take() handed it to @p extension, that connection is
    ///         attached, and an action invoked through it at most ServedInputGrace ago was not claimed yet;
    ///         nullopt otherwise.
    [[nodiscard]] virtual std::optional<ConnectionId> claimReveal(std::string_view extension,
                                                                  Origin origin) = 0;
};

/// Why a call about a served tab was refused.
enum class ServedTabError : uint8_t
{
    NotOwned,    ///< The id names no tab this extension serves: another's, a shell pane's, or none at all.
    Gone,        ///< The connection the call came from detached.
    Limit,       ///< A limit is reached: too much at once, or a live upgrade runs (retry later).
    BadArgument, ///< An argument is not what the manifest declares, or the call accepts.
};

/// Where a served tab stands, as its extension lists it. A tab closed before it was created is never listed.
enum class ServedTabState : uint8_t
{
    Pending = 0, ///< Opened, and its session not created yet: written bytes wait in its device.
    Open,        ///< Its session exists.
};

/// One served tab, as its extension lists it.
struct ServedTabInfo
{
    std::string tab;                                ///< Its id: its pane's UUID.
    std::string scheme;                             ///< What it was opened as.
    uint32_t cols = 0;                              ///< Its width, in cells.
    uint32_t rows = 0;                              ///< Its height, in cells.
    ServedTabState state = ServedTabState::Pending; ///< Whether its session exists yet.

    bool operator==(ServedTabInfo const&) const = default;
};

/// The tabs daemon-scoped extensions serve, one dashboard per extension and workspace.
///
/// Thread rules: every member may be called on any thread and never blocks on the host's loop. The
/// implementation keeps a mutex-guarded mirror of its tabs (id → extension, workspace, state, device, input)
/// that these read and write, and posts every model change -- creation, activation, close -- to the loop.
/// A tab id is looked up in that mirror only: an id that is not one of the calling extension's tabs answers
/// NotOwned, and touches nothing.
class IServedTabs
{
  public:
    IServedTabs() = default;
    IServedTabs(IServedTabs const&) = delete;
    IServedTabs(IServedTabs&&) = delete;
    IServedTabs& operator=(IServedTabs const&) = delete;
    IServedTabs& operator=(IServedTabs&&) = delete;
    virtual ~IServedTabs() = default;

    /// Opens the extension's tab in the workspace the origin's window shows, or the one it has there. It is
    /// shown -- activated, its window raised -- only as IExtensionActions::claimReveal() allows: the origin's
    /// action invoked just now, and not claimed yet. Otherwise a new tab is created in the background (no
    /// window raised, nothing activated, no focus taken), and one open already stays where it is. Returns
    /// before the tab's session exists: it is created on the loop.
    /// @param extension What the daemon's configuration calls it.
    /// @param title The tab's fixed name: the manifest's `title`, else the extension's name.
    /// @param moduleDigest The hex SHA-256 of the module that draws it, which the chrome names.
    /// @param scheme What the tab is served as; its declaration was checked by the caller.
    /// @param origin The connection the action that asked for it came from.
    /// @return The tab's id; Gone for an origin that detached; Limit while a live upgrade runs.
    [[nodiscard]] virtual std::expected<std::string, ServedTabError> open(std::string_view extension,
                                                                          std::string_view title,
                                                                          std::string_view moduleDigest,
                                                                          std::string_view scheme,
                                                                          Origin origin) = 0;

    /// Closes one of the extension's tabs; one not created yet is never created.
    /// @param extension What the daemon's configuration calls it.
    /// @param tab The tab's id.
    /// @return Nothing; NotOwned for an id that is not one of its tabs.
    [[nodiscard]] virtual std::expected<void, ServedTabError> close(std::string_view extension,
                                                                    std::string_view tab) = 0;

    /// Brings one of the extension's tabs in front of the origin's window: activated where it is shown, the
    /// window asked first when it is in another workspace. The origin counts once, and only for
    /// ServedInputGrace after the action the user invoked through it (IExtensionActions::claimReveal());
    /// without it, the window whose user typed or clicked into one of its served tabs at most
    /// ServedInputGrace ago is the one asking. A tab not created yet is shown once created; without such a
    /// user action it stays in the background, and the reveal answers nothing.
    /// @param extension What the daemon's configuration calls it.
    /// @param tab The tab's id.
    /// @param origin The connection the action that asked for it came from.
    /// @return Nothing; NotOwned for an id that is not one of its tabs; Gone, for a tab created already, for
    /// an
    ///         origin that is stale, claimed already or detached, and no recent key or click to stand for it.
    [[nodiscard]] virtual std::expected<void, ServedTabError> reveal(std::string_view extension,
                                                                     std::string_view tab,
                                                                     Origin origin) = 0;

    /// @param extension What the daemon's configuration calls it.
    /// @return Its tabs, pending ones included, in the order they were opened.
    [[nodiscard]] virtual std::vector<ServedTabInfo> list(std::string_view extension) const = 0;

    /// Takes the input the tab's terminal wrote since the last read, at most MaxServedTransferBytes.
    /// @param extension What the daemon's configuration calls it.
    /// @param tab The tab's id.
    /// @return The bytes, whole chunks only; empty for a tab not created yet; NotOwned for a foreign id.
    [[nodiscard]] virtual std::expected<std::string, ServedTabError> read(std::string_view extension,
                                                                          std::string_view tab) = 0;

    /// Draws into the tab: @p bytes reach its terminal as its program's output, once it exists.
    /// @param extension What the daemon's configuration calls it.
    /// @param tab The tab's id.
    /// @param bytes At most MaxServedTransferBytes.
    /// @return Nothing; NotOwned for a foreign id; Gone for a tab whose device closed; Limit for too many
    ///         bytes.
    [[nodiscard]] virtual std::expected<void, ServedTabError> write(std::string_view extension,
                                                                    std::string_view tab,
                                                                    std::string_view bytes) = 0;

    /// Closes every tab of the extension, as its grant is revoked.
    /// @param extension What the daemon's configuration calls it.
    virtual void closeAll(std::string_view extension) = 0;

    /// Draws the host's own frame into every tab of the extension, once it stopped after repeated faults: a
    /// rounded box, centred, that names it, its last fault, and the palette row that restarts it. Each tab
    /// is drawn again at its new size on every resize, until resumeDrawing().
    /// @param extension What the daemon's configuration calls it.
    /// @param title Its manifest's `title`, else its name.
    /// @param lastFault Its last fault, neutralized; empty for none.
    virtual void drawStopped(std::string_view extension,
                             std::string_view title,
                             std::string_view lastFault) = 0;

    /// Hands the extension's tabs back to it as it runs again: a resize no longer draws the host's frame, and
    /// what the extension draws covers it.
    /// @param extension What the daemon's configuration calls it.
    virtual void resumeDrawing(std::string_view extension) = 0;
};

/// Why a call about an extension's agent pane was refused.
enum class OwnedPaneError : uint8_t
{
    NotOwned,      ///< The id names no pane this extension opened: another's, a shell's, or none at all.
    TooMany,       ///< The extension has MaxOwnedPanesPerExtension panes already.
    Upgrading,     ///< A live upgrade runs: nothing opens until it is over.
    NotReconciled, ///< The host has not restored its sessions yet: the answer would be wrong; retry later.
    NotSupported,  ///< This host cannot start a program in a directory held open (a Flatpak's host, Windows).
    DirectoryGone, ///< The directory held open was removed since (whatever stands at its path now).
    ProgramMissing, ///< The program is not found where its spawn would look it up.
    Gone, ///< The pane was closed (its tab is going, or was never created), or no action is behind it.
};

/// Where an agent pane stands, as its extension lists it.
enum class OwnedPaneState : uint8_t
{
    Pending = 0, ///< Opened, and its session not created yet.
    Open,        ///< Its session exists.
    Cancelled,   ///< Closed while pending: its creation creates nothing, and drops it.
    Closing,     ///< Closed while open: listed until its session ended.
};

/// Whether the program the host spawned in a pane still runs there.
enum class PaneLiveness : uint8_t
{
    Gone = 0, ///< It exited, and its session is still hosted.
    Alive,    ///< It runs, or is about to.
};

/// One agent pane, as its extension lists it.
struct OwnedPaneInfo
{
    std::string pane;                                 ///< Its id: its pane's UUID.
    std::string tag;                                  ///< The extension's own label for it.
    std::optional<vtbackend::ProgramState> status {}; ///< What its program last reported (PaneStatusSources).
    PaneLiveness program = PaneLiveness::Alive;       ///< Whether its program still runs.
    OwnedPaneState state = OwnedPaneState::Pending;   ///< Whether its session exists yet.

    bool operator==(OwnedPaneInfo const&) const = default;
};

/// What an extension's agent pane is to run, everything its policy checked: the program, its environment,
/// and the directory it starts in, held open.
struct OwnedPaneSpawn
{
    std::string extension;                                           ///< Whose pane it is.
    std::vector<std::string> argv;                                   ///< The program and its arguments.
    std::vector<std::pair<std::string, std::string>> environment {}; ///< Laid over the inherited one.
    std::vector<std::string> removedEnvironment {};                  ///< Inherited names it does not see.
    std::shared_ptr<vtpty::DirectoryHandle const> directory {};      ///< Entered by its descriptor.
    std::string cwd {};                                              ///< The directory's resolved path.
    std::string title {};                                            ///< The tab's fixed name.
    std::string tag {};                                              ///< The extension's own label.
    std::optional<std::string> nearTab {}; ///< The served tab whose workspace it opens in, if any.
};

/// The most agent panes one extension may have at once, pending ones included.
inline constexpr std::size_t MaxOwnedPanesPerExtension = 32;

/// The agent panes daemon-scoped extensions open: tabs running a program of theirs, opened beside their
/// dashboard without taking the focus, and known to be theirs across a live upgrade.
///
/// Thread rules: every member may be called on any thread and never blocks on the host's loop. The
/// implementation keeps a mutex-guarded mirror of the panes that these read and write, and posts every model
/// change -- creation, close -- to the loop.
class IOwnedPanes
{
  public:
    IOwnedPanes() = default;
    IOwnedPanes(IOwnedPanes const&) = delete;
    IOwnedPanes(IOwnedPanes&&) = delete;
    IOwnedPanes& operator=(IOwnedPanes const&) = delete;
    IOwnedPanes& operator=(IOwnedPanes&&) = delete;
    virtual ~IOwnedPanes() = default;

    /// Opens an agent pane. Returns before its session exists: it is created on the loop, and listed Pending
    /// until then.
    /// @param spawn What it runs.
    /// @return The pane's id; Limit past MaxOwnedPanesPerExtension or while a live upgrade runs;
    ///         NotReconciled before the host restored its sessions; NotSupported where no program can be
    ///         started in a directory held open; DirectoryGone for one removed since it was opened. Each
    ///         refusal is decided before any pane exists.
    [[nodiscard]] virtual std::expected<std::string, OwnedPaneError> open(OwnedPaneSpawn spawn) = 0;

    /// Closes one of the extension's panes: a pending one is never created; an open one stays listed until
    /// its session ended.
    /// @param extension Whose pane it is.
    /// @param pane The pane's id.
    /// @return Nothing; NotOwned for an id that is not one of its panes.
    [[nodiscard]] virtual std::expected<void, OwnedPaneError> close(std::string_view extension,
                                                                    std::string_view pane) = 0;

    /// @param extension Whose panes.
    /// @return Its panes, pending ones included, in the order they were opened; NotReconciled until the host
    ///         restored its sessions.
    [[nodiscard]] virtual std::expected<std::vector<OwnedPaneInfo>, OwnedPaneError> list(
        std::string_view extension) const = 0;

    /// Brings one of the extension's panes in front of the origin's window: activated where that window
    /// shows it, the window asked first when it is in another workspace. The origin counts once, and only
    /// for ServedInputGrace after the action the user invoked through it (IExtensionActions::claimReveal());
    /// without it, the window whose user typed or clicked into one of the extension's served tabs at most
    /// ServedInputGrace ago is the one asking. Without either -- an origin never handed to it (0, made up,
    /// another extension's), gone stale, claimed already, or whose window detached -- it reveals nothing, and
    /// activates nothing: no user action is behind it. A pane not created yet is revealed once it is.
    /// @param extension Whose pane it is.
    /// @param pane The pane's id.
    /// @param origin The connection the action that asked for it came from.
    /// @return Nothing; NotOwned for an id that is not one of its panes; Gone for one it closed, or with no
    ///         user action behind it; NotReconciled until the host restored its sessions.
    [[nodiscard]] virtual std::expected<void, OwnedPaneError> reveal(std::string_view extension,
                                                                     std::string_view pane,
                                                                     Origin origin) = 0;
};

/// What the daemon world reaches of its host, handed to the factory complete: the cyclic wiring of host and
/// world resolved at construction.
struct DaemonWorldWiring
{
    IServedTabs& tabs;          ///< Any thread (@see IServedTabs).
    IExtensionActions& actions; ///< take() and connectionOf() from any thread; the rest is the host's.
    IOwnedPanes& panes;         ///< Any thread (@see IOwnedPanes).
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
/// @param wiring What they reach of the host; it outlives them.
/// @return The extensions.
using DaemonWorldFactory = std::function<std::shared_ptr<IDaemonWorldHosting>(
    DaemonWorldEvents events, OwnerPost post, DaemonWorldWiring wiring)>;

} // namespace vthost
