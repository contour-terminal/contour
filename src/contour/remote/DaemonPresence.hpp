// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The application's presence of every daemon its sidebar lists (spec §2, Daemon presence): which daemons
/// are listed, where each one's facts come from -- a window's own connection, or one observer connection
/// this process shares among its windows -- and the verbs a sidebar row asks of them. A window reaches it
/// only through window::IDaemonSections.

#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/platform/OffThreadJobs.hpp>
#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/ObserverPool.hpp>
#include <contour/remote/PresencePlan.hpp>
#include <contour/remote/PresenceTransport.hpp>
#include <contour/window/DaemonSections.hpp>
#include <contour/window/PaneSeen.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <vthost/Transport.hpp>
#include <vthost/client/ClientSupervisor.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

class QObject;

namespace contour::remote
{

class NativeController;

/// Whether any window of the application is visible: the periodic refresh runs only while one is.
enum class AppVisibility : uint8_t
{
    Hidden = 0, ///< No window is visible.
    Visible,    ///< A window is visible.
};

/// One attached window link of this process, as the presence reads it.
struct WindowLinkView
{
    DaemonLinkId link {};            ///< Which link it is.
    window::DaemonRef daemon;        ///< The daemon it is on.
    window::DaemonInstance instance; ///< That daemon's run.
    std::string name; ///< What its section is called; empty for the daemon's own name (daemonNameOf()).
    window::DaemonLocality locality = window::DaemonLocality::Remote; ///< Whether the daemon is local.
    window::WorkspaceDirectory directory;                             ///< Every workspace the link lists.
    window::DaemonLinkState state = window::DaemonLinkState::NotConnected; ///< Where the link stands.
    std::set<uint64_t> shownWorkspaces; ///< The daemon workspaces the link's windows show.
    /// The link's controller, which a verb on the daemon goes to. Borrowed for the pull's duration; null
    /// for none.
    NativeController* controller = nullptr;
};

/// How often the presence looks again at what is listed while a window is visible.
inline constexpr auto PresenceRefreshInterval = std::chrono::milliseconds { std::chrono::seconds { 30 } };

/// How long an observer waits before each dial after one failed for a reason that may pass, or after its
/// connection was lost: a controller's first wait, doubling up to the periodic refresh's interval, so that
/// a host that stays unreachable is logged in to no more often than the sidebar looks again anyway.
inline constexpr auto ObserverRedialBackoff =
    vthost::client::BackoffSchedule { .first = vthost::client::BackoffSchedule {}.first,
                                      .ceiling = PresenceRefreshInterval };

/// Makes an observer of a daemon: a controller of proto::ConnectionRole::Observe that never resumes into a
/// launch, dialling through the given transport, and reconnecting after a loss by the given schedule. Not
/// started.
using ObserverFactory =
    std::function<std::unique_ptr<NativeController>(window::DaemonRef const& daemon,
                                                    std::shared_ptr<vthost::ITransportFactory> transport,
                                                    vthost::client::BackoffSchedule backoff)>;

/// @param config What an observer is built with otherwise: its transport, settings and timeline.
/// @param backoff How long it waits before each reconnect.
/// @return @p config as an observer's: proto::ConnectionRole::Observe, never resuming into a launch, and
///         answering switch requests -- which a daemon never sends an observer, but which the connection is
///         to answer for life once a window adopts it (DaemonLink::adopt()), as its Config is fixed.
[[nodiscard]] NativeController::Config observerConfigOf(NativeController::Config config,
                                                        vthost::client::BackoffSchedule backoff);

/// How the presence dials a daemon in the background (remote::observerTransport()).
using ObserverDial = std::function<std::expected<std::shared_ptr<vthost::ITransportFactory>, DialRefusal>(
    window::DaemonRef const& daemon)>;

/// How the presence dials a daemon the user asked to connect to (remote::interactiveTransport()).
using InteractiveDial = std::function<std::expected<std::shared_ptr<vthost::ITransportFactory>, DialRefusal>(
    window::DaemonRef const& daemon, vtworkspace::WorkspaceId askingWindow)>;

/// Everything the presence needs of the application. Every member is called on the GUI thread.
struct PresenceDeps
{
    /// Finds the local daemons; run off the GUI thread. Shared with the Connect picker.
    std::shared_ptr<cli::IDaemonDiscovery> discovery;
    /// The configuration's saved hosts and the GUI state's listing choices
    /// (config::GuiState::sidebarDaemons).
    std::function<ListingSettings()> settings;
    /// Dials an observer: without asking anyone anything, and without starting a daemon.
    ObserverDial observerTransport;
    /// Dials what the user asked for, its prompts shown in the asking window.
    InteractiveDial interactiveTransport;
    /// Makes an observer (Config::role Observe, resumeFallback Nothing).
    ObserverFactory makeObserver;
    /// The attached window links of this process, pulled whenever the presence needs them.
    std::function<std::vector<WindowLinkView>()> windowLinks;
    /// Fires the periodic refresh and an observer's background redial; each fire is guarded, so one after
    /// the presence is gone does nothing.
    SwitchTimer refreshTimer;
    /// Whether a window is visible: the periodic refresh re-arms only while one is.
    std::function<AppVisibility()> visibility;
    /// Starts a local daemon that is not running, off the GUI thread, and refreshes the presence once done.
    std::function<void(window::DaemonRef const&)> startDaemon;
    /// Changes the user's listing choices kept in the GUI state, in one read-modify-write of it, so that a
    /// choice another process made meanwhile is kept.
    std::function<void(std::function<void(config::SidebarDaemons&)> const& change)> updateListing;
    /// What an observer that holds its daemon's raise role decides and raises what it was handed with.
    std::function<ElsewhereNotices()> elsewhereNotices;
    /// Rings the bell, and alerts, on the window the user focused last, as the bell says.
    std::function<void(config::Bell const&)> ringLastFocusedWindow;
    /// The tabs that wait for the user in the daemons the observers feed, counted once each: told whenever
    /// the count changed.
    std::function<void(unsigned)> observedWaitingChanged;
    /// How long an observer waits before each redial: what the presence schedules its redials by, and what
    /// each observer it makes reconnects by.
    vthost::client::BackoffSchedule redialBackoff = ObserverRedialBackoff;
};

/// What a test replaces of the production presence the application builds; null members keep production's.
struct PresenceOverrides
{
    ObserverFactory makeObserver; ///< Makes the observers.
    SwitchTimer refreshTimer;     ///< Fires the refreshes and the redials.
    /// Finds the local daemons, in place of this machine's runtime directory: a test lists none of its own.
    std::shared_ptr<cli::IDaemonDiscovery> discovery;
    /// The saved hosts and listing choices, in place of the configuration's and the GUI state's.
    std::function<ListingSettings()> settings;
    ObserverDial observerTransport; ///< Dials an observer, in place of the configuration's way to a daemon.
    /// The window links, in place of the application's own: a test's links live outside it.
    std::function<std::vector<WindowLinkView>()> windowLinks;
};

/// @param daemon A daemon.
/// @return What its section is called when nothing better is known: a local daemon's label, a saved host's
///         name, or "link <id>".
[[nodiscard]] std::string daemonNameOf(window::DaemonRef const& daemon);

/// Names alike the window links of one daemon: a link reached by its own address (a `link:` daemon) that
/// is on the same run of a daemon as another link -- by the instance the daemon drew -- is named as that one
/// is, preferring a local daemon's or a host's name, so that one daemon is one section, fed by any of them.
/// @param links The attached window links.
/// @return @p links, so named.
[[nodiscard]] std::vector<WindowLinkView> mergedLinks(std::vector<WindowLinkView> links);

/// The application's presence of every listed daemon.
///
/// A listed daemon's section is fed by an attached window link to it if there is one, else by the daemon's
/// observer -- started only then, and stopped once a window link is attached, once the daemon is no longer
/// listed (removed, hidden, or no window is on any daemon), and as the presence is destroyed. An observer
/// dials only through PresenceDeps::observerTransport; a dial that was refused, or that failed for a reason
/// the user must deal with first (NeedsInput, NotRunning, TooOld, Failed), is never redialed by the timer --
/// only connect() redials it, interactively and never starting the daemon, and start() once the daemon runs;
/// a local daemon a discovery did not find serving is dialled again once a later one does; and a local one
/// found serving that reads TooOld or NotRunning is dialled again at every periodic refresh, by the discovery
/// it asks, as a unix socket dial asks nothing and starts nothing. An observer whose daemon stopped reads
/// NotRunning and dials it no more by itself (FailureRecordingTransport). A dial that failed for a
/// reason that may pass is redialed in the background by PresenceDeps::redialBackoff, while a window is
/// visible. A controller handed to a window (adoptObserver()) is the window's: the presence never stops it.
///
/// The listing settings are read once and kept: settingsChanged() reads them again, as does the periodic
/// refresh, so that a choice another process made is seen.
///
/// Threading: GUI thread only. The observers' signals are taken on it.
class DaemonPresence final: public window::IDaemonSections, public IObserverPool
{
  public:
    /// @param deps What the presence needs of the application. Nothing is called before the first refresh()
    ///        or link notice.
    explicit DaemonPresence(PresenceDeps deps);
    /// Stops every observer it holds.
    ~DaemonPresence() override;
    DaemonPresence(DaemonPresence const&) = delete;
    DaemonPresence(DaemonPresence&&) = delete;
    DaemonPresence& operator=(DaemonPresence const&) = delete;
    DaemonPresence& operator=(DaemonPresence&&) = delete;

    /// Starts a discovery of the local daemons (one at a time), decides again what is listed, starts and
    /// stops observers accordingly, and tells the subscribers. Arms the next periodic refresh while a window
    /// is visible.
    void refresh();

    /// The configuration's saved hosts or the GUI state's listing choices may have changed: they are read
    /// again, and the presence refreshed.
    void settingsChanged();

    /// Stops every observer and waits for each to end, and starts none from now on: what the application
    /// calls as it shuts down, while what the observers reach is still there. Idempotent.
    void shutdown();

    /// A link was started, or links ended: what is listed is decided again, which may start or stop
    /// observers. A started link feeds its daemon's section once it is attached, and the daemon's observer
    /// is stopped then.
    void linkSetChanged();

    /// A window link changed (its connection or its directory): the subscribers are told at once.
    void linkChanged();

    /// Tells every observer whether this process has the user's focus, and so the daemon which process
    /// raises for it while no window is on it: the one focused last. An observer started later is told the
    /// focus as it starts, and states it again on every reconnect.
    /// @param focus The process's focus now.
    void processFocusChanged(window::WindowFocusState focus);

    /// @return The local daemons the last discovery found; empty until one answered.
    [[nodiscard]] std::vector<cli::DiscoveredDaemon> localDaemons() const;

    /// @return The listed daemons, in tree order, as last decided.
    [[nodiscard]] std::vector<window::DaemonRef> listed() const;

    /// Hands @p daemon's observer to a window that lands on it, so that no second connection is dialled; the
    /// daemon is marked as handed over until the adopting link is attached, or gives the connection back.
    /// Meanwhile its section reads Connecting, keeping the run and workspaces the observer last held, and
    /// neither connect() nor start() dials it.
    /// @param daemon The daemon.
    /// @param instance The run of it the window's target names; 0 for one it does not know.
    /// @return The observer, which the window owns from now on, and what its give-back names; none unless it
    ///         is Connected, and of run @p instance.
    [[nodiscard]] Adoption adoptObserver(window::DaemonRef const& daemon,
                                         window::DaemonInstance instance) override;

    /// Takes back a connection a window left (demoted to an observer), and clears the hand-off mark @p
    /// handOff names.
    /// @param daemon The daemon it reaches.
    /// @param handOff The hand-over it ends; 0 for one the presence never marked (a link that left with End).
    /// @param demoted The connection, which must bind no pane: its window unbinds them first, as stopping it
    ///        would end them under the window. Kept as the daemon's observer -- dialling as one from now on
    ///        -- only if it is Connected, an observer, no window link is on the daemon, the daemon is still
    ///        listed and has no observer yet; stopped otherwise. Null: nothing comes back, and the daemon is
    ///        observed again as any listed one is -- decided again only if a mark was cleared, so that a link
    ///        stopping as the application quits dials nothing.
    void giveBack(window::DaemonRef const& daemon,
                  HandOffId handOff,
                  std::unique_ptr<NativeController> demoted) override;

    /// Marks @p daemon's connection as coming back from link @p from: until it is given back, the daemon's
    /// section reads what it last read through the link, and no observer of it is dialled.
    /// @return What the link's give-back names.
    [[nodiscard]] HandOffId expectGiveBack(window::DaemonRef const& daemon, DaemonLinkId from) override;
    [[nodiscard]] ObserverWanted wantsObserver(window::DaemonRef const& daemon,
                                               DaemonLinkId leaving) const override;
    [[nodiscard]] std::optional<window::DaemonSection> listedSection(
        window::DaemonRef const& daemon) const override;

    // window::IDaemonSections
    [[nodiscard]] std::vector<window::DaemonSection> sections() const override;
    void connect(window::DaemonRef const& daemon, vtworkspace::WorkspaceId askingWindow) override;
    void disconnect(window::DaemonRef const& daemon) override;
    void start(window::DaemonRef const& daemon) override;
    void remove(window::DaemonRef const& daemon) override;
    /// Keeps the user's choice in the GUI state. Listed takes the daemon's Hide back, and writes a Show only
    /// where the configuration would not list it (a saved host with `show_in_sidebar: false`, or one no
    /// configuration names): a local daemon, or a host the configuration lists, merely loses its Hide, so a
    /// Hide and its Undo leave the GUI state as it was. A `link:` daemon keeps no choice.
    void setListed(window::DaemonRef const& daemon, window::SidebarListingChoice choice) override;
    [[nodiscard]] std::vector<window::HiddenDaemon> hiddenDaemons() const override;

    /// @param daemon A daemon the switcher lists.
    /// @return Whether the user and the configuration chose to list it (remote::chosenListing()), for the
    ///         switcher's "Show in Sidebar" -- what was chosen, whether or not a sidebar shows it now;
    ///         Unlisted for one removed this run. None where the user has no choice to make: a `link:`
    ///         daemon, or one a window of this process is on, which is listed whatever the user chose.
    [[nodiscard]] std::optional<window::SidebarListingChoice> listingOf(window::DaemonRef const& daemon);
    void renameWorkspace(window::DaemonRef const& daemon,
                         window::DaemonInstance instance,
                         uint64_t workspace,
                         std::string name) override;
    void renameTab(window::DaemonRef const& daemon,
                   window::DaemonInstance instance,
                   window::TabRef tab,
                   std::string name) override;
    void setWorkspaceColor(window::DaemonRef const& daemon,
                           window::DaemonInstance instance,
                           uint64_t workspace,
                           std::optional<vtbackend::RGBColor> color) override;
    void closeTab(window::DaemonRef const& daemon,
                  window::DaemonInstance instance,
                  window::TabRef tab) override;
    void setTabColor(window::DaemonRef const& daemon,
                     window::DaemonInstance instance,
                     window::TabRef tab,
                     std::optional<vtbackend::RGBColor> color) override;
    void closePane(window::DaemonRef const& daemon,
                   window::DaemonInstance instance,
                   window::PaneKey pane) override;
    void closeWorkspace(window::DaemonRef const& daemon,
                        window::DaemonInstance instance,
                        uint64_t workspace) override;
    void markSeen(window::DaemonRef const& daemon,
                  window::DaemonInstance instance,
                  window::PaneKey pane,
                  window::PaneSeen seen) override;
    [[nodiscard]] window::SectionsSubscription onChanged(std::function<void()> changed) override;

  private:
    /// Whether a background redial is scheduled: a failure told twice schedules one.
    enum class RedialState : uint8_t
    {
        Idle = 0,
        Scheduled,
    };

    /// What an observer knows of its local daemon having gone, which a discovery that finds it serving
    /// answers by dialling it again.
    enum class LocalLiveness : uint8_t
    {
        Unknown = 0,  ///< Never connected since its last dial, and no discovery missed it since.
        Served,       ///< Connected since its last dial.
        GaveUpAbsent, ///< Gave up finding the daemon gone after it served, or a discovery missed it since.
    };

    /// Whether the user ended an observer's connect: then only Connect dials it again, never a redial or a
    /// refresh, whatever its daemon does meanwhile.
    enum class Hold : uint8_t
    {
        None = 0,         ///< Dialled again in the background as its state asks.
        UserDisconnected, ///< Cancelled by the user: holds nothing until Connect.
    };

    /// The connection this process keeps to a listed daemon no window of it is on, or why it keeps none.
    struct Observer
    {
        std::unique_ptr<NativeController> controller; ///< Null while refused, or awaiting a redial.
        /// What it dials through, which remembers how the last dial failed; null for a connection given back.
        std::shared_ptr<FailureRecordingTransport> recorder;
        std::optional<DialRefusal> refusal; ///< Why nothing was dialled; none once a dial was made.
        /// Takes the controller's signals; destroyed with the controller it listens to.
        std::unique_ptr<QObject> receiver;
        uint32_t redials = 0;                   ///< The background redials since it was last connected.
        RedialState redial = RedialState::Idle; ///< Whether a background redial of the controller is due.
        LocalLiveness liveness = LocalLiveness::Unknown; ///< Whether its daemon is known to have gone.
        Hold hold = Hold::None; ///< Whether the user cancelled it, which only Connect lifts.
    };

    /// Whether a discovery runs now: a refresh meanwhile starts none.
    enum class Discovery : uint8_t
    {
        Idle = 0,
        Running,
    };

    /// Whether the periodic refresh is armed.
    enum class RefreshArm : uint8_t
    {
        Idle = 0,
        Armed,
    };

    /// Why a daemon's connection is in no observer entry for now, while a window holds it.
    enum class HandOff : uint8_t
    {
        Adopting = 0, ///< Handed to a window landing on it, whose link is not attached yet.
        Returning,    ///< Coming back from a window leaving it, once its panes let it go.
    };

    /// One hand-over under way of a daemon's connection: what keeps the daemon from being dialled, and what
    /// its section reads meanwhile, for neither a window link nor an observer feeds it.
    struct HandOffMark
    {
        window::DaemonRef daemon;         ///< The daemon whose connection a window holds.
        HandOff kind = HandOff::Adopting; ///< Which way it goes.
        /// The daemon's section as it last read, kept while the mark stands: Connecting while adopted, the
        /// link's last reading while it comes back.
        window::DaemonSection lastKnown;
    };

    /// Whether the presence still observes anything.
    enum class Lifecycle : uint8_t
    {
        Running = 0,
        ShutDown, ///< shutdown() ran: nothing is dialled any more.
    };

    /// Decides again what is listed, and starts and stops observers to match.
    void reconcile();
    /// Decides again what is listed from @p links.
    void reconcile(std::vector<WindowLinkView> const& links);
    /// Dials @p daemon's observer in the background, keeping its refusal if it is refused.
    void startObserver(window::DaemonRef const& daemon, Observer& observer);
    /// Stops @p observer's controller, if any, and keeps @p refusal as why nothing was dialled.
    void refuse(Observer& observer, DialRefusal refusal);
    /// Tells @p observer's controller the process's focus, once there is a controller and a focus to tell.
    void announceFocus(Observer const& observer) const;
    /// Starts @p observer's controller on @p transport, replacing any it had.
    void launch(window::DaemonRef const& daemon,
                Observer& observer,
                std::shared_ptr<vthost::ITransportFactory> transport);
    /// Listens to @p observer's controller.
    void watch(window::DaemonRef const& daemon, Observer& observer);
    /// An observer's connection changed state.
    void observerStateChanged(window::DaemonRef const& daemon, NativeController const* controller);
    /// Redials an observer whose dial failed for a reason that may pass, if it still waits for that.
    void redial(window::DaemonRef const& daemon, NativeController const* controller);
    /// Schedules @p observer's next redial, unless one is, or no window is visible.
    void scheduleRedial(window::DaemonRef const& daemon, Observer& observer);
    /// Stops listening to @p observer's controller, and retires it (retire()).
    void detach(Observer& observer);
    /// Stops @p controller without waiting, and keeps it until it is stopped and binds no pane.
    void retire(std::unique_ptr<NativeController> controller);
    /// Destroys the retired controllers that are stopped and bind no pane.
    void pruneRetired();
    void startDiscovery();
    void armRefresh();
    /// Raises what @p controller, holding @p daemon's raise role, was handed, from the window focused last.
    void raiseFor(window::DaemonRef const& daemon, NativeController& controller) const;
    /// Counts again the tabs that wait in the observers' daemons, and tells the application if that changed.
    void recountObservedWaiting();
    /// Tells the application how many tabs wait in the observed daemons, and every subscriber that the
    /// sections changed.
    void notify();

    /// @return The listing settings, read once until settingsChanged() or the periodic refresh.
    [[nodiscard]] ListingSettings const& settings();
    /// @return The attached window links, those on one daemon named alike (mergedLinks()).
    [[nodiscard]] std::vector<WindowLinkView> windowLinks() const;

    /// Notes what the discovery just made says of each local daemon whose observer dials nothing more by
    /// itself: one it missed has gone; one it found that had gone is dialled again, as is one it found whose
    /// observer reads Too old or Not running, while a window is visible (isRedialledByRefresh()).
    /// @param stuck The local daemons whose observers read Too old or Not running as the periodic refresh
    ///        asked this discovery (@ref _stuckAtRefresh): only those are dialled again, never one dialled
    ///        since, nor at a discovery a giving-up observer asked for.
    void redialRestartedLocals(std::span<window::DaemonRef const> stuck);
    /// @return The local daemons whose observers a discovery would dial again (isRedialledByRefresh()).
    [[nodiscard]] std::vector<window::DaemonRef> stuckLocals() const;
    /// @return Whether @p daemon is a local daemon the last discovery found serving.
    [[nodiscard]] bool isRunningLocal(window::DaemonRef const& daemon) const;
    /// @return Whether @p daemon is listed, as last decided.
    [[nodiscard]] bool isListed(window::DaemonRef const& daemon) const;
    /// @return Whether the configuration alone lists @p daemon: a local daemon (while known) is; a saved
    ///         host by its `show_in_sidebar`; any other host is not.
    [[nodiscard]] window::SidebarListingChoice configuredListing(window::DaemonRef const& daemon);
    /// @return The newest hand-off mark of @p daemon; null while a window holds none of its connection.
    [[nodiscard]] HandOffMark const* handOffOf(window::DaemonRef const& daemon) const;
    /// Marks @p daemon's connection as held by a window, its section reading @p lastKnown meanwhile.
    /// @return What the give-back names.
    [[nodiscard]] HandOffId markHandOff(window::DaemonRef const& daemon,
                                        HandOff kind,
                                        window::DaemonSection lastKnown);
    /// @return Whether @p observer's dial failed for a reason that may pass.
    [[nodiscard]] static bool isRetryable(Observer const& observer);
    /// @return Whether a periodic refresh dials @p daemon's @p observer again: a local daemon discovery finds
    ///         serving, whose observer stopped at Too old or Not running -- the daemon may have been upgraded
    ///         or started since, and a unix socket dial asks nothing and starts nothing.
    [[nodiscard]] bool isRedialledByRefresh(window::DaemonRef const& daemon, Observer const& observer) const;
    /// @return The controller a verb on @p daemon's run @p instance goes to; null for none.
    [[nodiscard]] NativeController* controllerFor(window::DaemonRef const& daemon,
                                                  window::DaemonInstance instance) const;
    /// @return @p daemon's section, fed by @p links or its observer.
    [[nodiscard]] window::DaemonSection sectionOf(ListedDaemon const& daemon,
                                                  std::vector<WindowLinkView> const& links) const;

    PresenceDeps _deps;
    Lifecycle _lifecycle = Lifecycle::Running;
    std::optional<ListingSettings> _settings; ///< The listing settings as last read; none to read them again.
    /// What the last discovery found; none until one answered.
    std::optional<std::vector<cli::DiscoveredDaemon>> _discovered;
    Discovery _discovery = Discovery::Idle;
    /// The local daemons whose observers read Too old or Not running as refresh() asked the running discovery
    /// (stuckLocals()): what its result dials again (redialRestartedLocals()), and nothing else.
    std::vector<window::DaemonRef> _stuckAtRefresh;
    RefreshArm _refreshArm = RefreshArm::Idle;
    std::set<std::string> _seenLocal;           ///< The local labels listed earlier in this run.
    std::set<std::string> _removed;             ///< The daemons removed for this run, by DaemonRef value.
    std::set<window::DaemonRef> _awaitingStart; ///< Started by start(), not yet found running.
    std::set<window::DaemonRef> _windowsOn;     ///< The daemons a window link was on at the last decision.
    std::vector<ListedDaemon> _listed;          ///< What is listed, as last decided.
    std::vector<window::HiddenDaemon> _hidden;  ///< What the Hidden row names, as last decided.
    std::map<window::DaemonRef, Observer> _observers; ///< By daemon; only listed daemons no window is on.
    /// The hand-overs under way, by what their give-back names: reconcile() dials no observer of their
    /// daemons. Each is cleared by its own giveBack(), and an Adopting one by a window link to its daemon
    /// being attached.
    std::map<HandOffId, HandOffMark> _handOffs;
    uint64_t _nextHandOff = 1; ///< What the next hand-over is named.
    /// Stopped (or stopping) controllers kept until they are stopped and bind no pane.
    std::vector<std::unique_ptr<NativeController>> _retired;
    std::optional<vthost::proto::WindowFocus> _focus; ///< The process's focus as last told.
    unsigned _observedWaiting = 0; ///< The observed daemons' waiting tabs, as last told the application.
    window::SectionsSubscribers _subscribers;
    /// Alive while the presence is: a timer's fire after its destruction finds it gone and does nothing.
    std::shared_ptr<int> _alive = std::make_shared<int>(0);
    /// The discoveries under way; destroyed (joined) first, so none answers into a presence going away.
    platform::OffThreadJobs _jobs;
};

} // namespace contour::remote
