// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/Uuid.hpp>
#include <orchestrator/guest/GuestClock.hpp>
#include <orchestrator/guest/HostCalls.hpp>
#include <orchestrator/view/Dashboard.hpp>

namespace orchestrator::guest
{

/// The root label the engine's ReadFile effects are read in: the manifest's `root worktrees`.
inline constexpr std::string_view WorktreesRoot = "worktrees";

/// What the driver feeds the engine and what the engine asks for in return: a seam for tests.
class IDriverObserver
{
  public:
    virtual ~IDriverObserver() = default;

    /// The engine is about to start.
    /// @param context What it starts from.
    virtual void starting(StartContext const& context) = 0;

    /// The engine started.
    /// @param effects Its first effects.
    virtual void started(std::span<Effect const> effects) = 0;

    /// The engine stepped an input.
    /// @param input The input.
    /// @param effects What it asked for.
    virtual void stepped(Input const& input, std::span<Effect const> effects) = 0;
};

/// The time the engine last asked to be woken at, and whether the host was asked for it. Each WakeAt is the
/// engine's next wake, replacing the one before, and the host keeps one request only: it is asked again only
/// when that time changes, or when its request was used up.
class WakeTimer
{
  public:
    /// Sets the time, replacing the one before.
    /// @param dueMs When, in monotonic milliseconds.
    void set(uint64_t dueMs) noexcept { _due = dueMs; }

    /// Takes the time if it is due.
    /// @param nowMs Now, in monotonic milliseconds.
    /// @return The time taken; std::nullopt if none is set or it is not due yet.
    [[nodiscard]] std::optional<uint64_t> takeDue(uint64_t nowMs) noexcept;

    /// The host delivered its wake: its request is used up.
    void requestUsed() noexcept { _asked = std::nullopt; }

    /// @param alsoDueMs Another time the guest must wake by, such as a held ESC's deadline; std::nullopt:
    /// none.
    /// @return The earlier of the time set and @p alsoDueMs, if the host was not asked for it yet.
    [[nodiscard]] std::optional<uint64_t> unasked(
        std::optional<uint64_t> alsoDueMs = std::nullopt) const noexcept;

    /// The host was asked for a time.
    /// @param dueMs The time.
    void asked(uint64_t dueMs) noexcept { _asked = dueMs; }

    /// @param dueMs A time.
    /// @param nowMs Now.
    /// @return The delay until @p dueMs, 0 if it is due, at most what a wake request holds.
    [[nodiscard]] static uint32_t delayUntil(uint64_t dueMs, uint64_t nowMs) noexcept;

  private:
    std::optional<uint64_t> _due;   ///< The time asked for and not yet due.
    std::optional<uint64_t> _asked; ///< The time the host's pending request is for.
};

/// The orchestrator's engine, driven through the daemon world's host calls, and its dashboard served in every
/// tab the host lists.
///
/// It keeps the engine's driver contract (Engine::step()): every RunCommand, ReadFile and OpenPane gets
/// exactly one reply, which the driver makes itself when the host refuses the call; replies are queued and
/// stepped after the batch of effects they answer; every input carries the monotonic time read when it
/// arrived; after a Wake's effects, the panes are listed. A finished process's result is released only once
/// the step that consumed it carried out its effects, its store writes included, so that a guest that faults
/// in between finds it again when it is loaded.
///
/// Each served tab has one view::Dashboard, made when the tab is first listed and kept across updates, so
/// that a key whose bytes arrive in two reads is decoded once. Every update ends with a frame of each
/// dashboard, written to its tab when anything changed.
class Driver
{
  public:
    /// Pulls the store, the processes and the panes into a StartContext, starts the engine, carries out its
    /// effects, and releases the results it consumed.
    /// @param host The host's calls; outlives the driver.
    /// @param ids Where the agents' session ids come from; outlives the driver.
    /// @param configJson The extension's configuration, as JSON.
    /// @param hostInfo What the host resolved.
    /// @return The driver.
    [[nodiscard]] static Driver load(IGuestHost& host,
                                     IUuidSource& ids,
                                     std::string_view configJson,
                                     HostInfo hostInfo);

    /// As load(IGuestHost&, IUuidSource&, std::string_view, HostInfo), telling @p observer every start and
    /// step.
    /// @param host The host's calls; outlives the driver.
    /// @param ids Where the agents' session ids come from; outlives the driver.
    /// @param configJson The extension's configuration, as JSON.
    /// @param hostInfo What the host resolved.
    /// @param observer Told what the engine is fed; outlives the driver.
    /// @return The driver.
    [[nodiscard]] static Driver load(IGuestHost& host,
                                     IUuidSource& ids,
                                     std::string_view configJson,
                                     HostInfo hostInfo,
                                     IDriverObserver& observer);

    /// Serves a hint, then draws every served tab: Wake consumes finished processes, steps a due Wake and
    /// lists the panes; Procs consumes finished processes; Tabs follows the listed tabs and applies what was
    /// typed into each; Actions carries out the actions the user invoked. Panes is not served yet.
    /// @param hint Why the host called.
    void update(Hint hint);

    /// @return The engine.
    [[nodiscard]] Engine const& engine() const noexcept { return *_engine; }

    /// @param tab A tab's id.
    /// @return The dashboard served in it; nullptr if the tab is not served.
    [[nodiscard]] view::Dashboard const* dashboard(std::string_view tab) const noexcept;
    /// @param tab A tab's id.
    /// @return The dashboard served in it, to act on as the user would; nullptr if the tab is not served.
    [[nodiscard]] view::Dashboard* dashboard(std::string_view tab) noexcept;

    /// @return How many tabs are served.
    [[nodiscard]] std::size_t servedTabs() const noexcept { return _dashboards.size(); }

    /// Carries out what the dashboard served in @p tab asked for, as its typed input does: steps the engine
    /// input of the same name (a view::DispatchAction as Dispatch, stamped with the time now), or, for
    /// view::RevealAgentAction, asks the host to reveal the run's agent tab
    /// to the window that last opened or revealed that dashboard (origin 0 when none did since the guest
    /// loaded).
    /// @param tab A served tab's id.
    /// @param action What its dashboard asked for.
    void apply(std::string_view tab, view::UiAction const& action);

  private:
    Driver(IGuestHost& host,
           IUuidSource& ids,
           IDriverObserver& observer,
           std::string worktreeRoot,
           Engine engine);

    /// What an action the user invoked does.
    using ActionHandler = void (Driver::*)(uint32_t origin);
    /// What each declared action does, indexed by its ActionKind (actionDecls() holds the ids).
    static std::array<ActionHandler, 3> const ActionHandlers;

    void openDashboard(uint32_t origin);
    void refreshNow(uint32_t origin);
    void showBlocked(uint32_t origin);
    /// @return The tab opened or revealed; std::nullopt when the host refused.
    std::optional<std::string> openTab(uint32_t origin);
    void revealAgent(std::string_view tab, std::string_view run);
    /// Types a message the user wrote on @p tab's message line into its run's agent tab; says on that
    /// dashboard why when it cannot.
    void sendMessage(std::string_view tab, view::SendMessageAction const& message);
    void takeActions();
    void followTabs();
    void readTabs();
    void drawTabs();
    void write(std::string const& tab, std::string_view bytes);

    void consumeFinished();
    void wake();
    void listPanes();
    void step(Input const& input);
    void carryOut(std::span<Effect const> effects);
    void stepReplies();
    void release(uint32_t handle);
    void askForWake();
    [[nodiscard]] uint64_t nowMs();
    [[nodiscard]] ClockReading readClock();
    [[nodiscard]] std::optional<uint64_t> dashboardsDueMs() const;

    IGuestHost& _host;
    IUuidSource& _ids; ///< Handed to every dashboard: the session ids of the agents it dispatches.
    IDriverObserver& _observer;
    std::string _worktreeRoot; ///< What ReadFile paths are made relative to.
    // Held by pointer: the dashboards keep references to both, and the driver moves.
    std::unique_ptr<Engine> _engine;
    std::unique_ptr<GuestClock> _clock; ///< The time last read from the host, which the dashboards read.
    std::map<std::string, std::unique_ptr<view::Dashboard>, std::less<>> _dashboards; ///< By tab id.
    /// By tab id: the origin of the action that last opened or revealed it, which its reveals are sent to.
    std::map<std::string, uint32_t, std::less<>> _origins;
    std::deque<Input> _replies;   ///< The replies the driver made itself, stepped after their batch.
    std::set<uint32_t> _consumed; ///< The finished processes stepped whose release failed.
    WakeTimer _timer;
    /// When the panes must be listed though no hint will announce it: a millisecond after a pane-open was
    /// refused, whose run waits until no tab tagged with it is listed (a listing asked for in the millisecond
    /// the run changed tells it nothing); none while no listing is owed.
    std::optional<uint64_t> _paneListingDueMs;
};

} // namespace orchestrator::guest
