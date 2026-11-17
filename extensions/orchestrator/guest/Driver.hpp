// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/guest/HostCalls.hpp>

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

    /// @return The time, if the host was not asked for it yet.
    [[nodiscard]] std::optional<uint64_t> unasked() const noexcept;

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

/// The orchestrator's engine, driven through the daemon world's host calls.
///
/// It keeps the engine's driver contract (Engine::step()): every RunCommand, ReadFile and OpenPane gets
/// exactly one reply, which the driver makes itself when the host refuses the call; replies are queued and
/// stepped after the batch of effects they answer; every input carries the monotonic time read when it
/// arrived; after a Wake's effects, the panes are listed. A finished process's result is released only once
/// the step that consumed it carried out its effects, its store writes included, so that a guest that faults
/// in between finds it again when it is loaded.
class Driver
{
  public:
    /// Pulls the store, the processes and the panes into a StartContext, starts the engine, carries out its
    /// effects, and releases the results it consumed.
    /// @param host The host's calls; outlives the driver.
    /// @param configJson The extension's configuration, as JSON.
    /// @param hostInfo What the host resolved.
    /// @return The driver.
    [[nodiscard]] static Driver load(IGuestHost& host, std::string_view configJson, HostInfo hostInfo);

    /// As load(IGuestHost&, std::string_view, HostInfo), telling @p observer every start and step.
    /// @param host The host's calls; outlives the driver.
    /// @param configJson The extension's configuration, as JSON.
    /// @param hostInfo What the host resolved.
    /// @param observer Told what the engine is fed; outlives the driver.
    /// @return The driver.
    [[nodiscard]] static Driver load(IGuestHost& host,
                                     std::string_view configJson,
                                     HostInfo hostInfo,
                                     IDriverObserver& observer);

    /// Serves a hint: Wake consumes finished processes, steps a due Wake and lists the panes; Procs
    /// consumes finished processes. Panes, Tabs and Actions are not served yet.
    /// @param hint Why the host called.
    void update(Hint hint);

    /// @return The engine.
    [[nodiscard]] Engine const& engine() const noexcept { return _engine; }

  private:
    Driver(IGuestHost& host, IDriverObserver& observer, std::string worktreeRoot, Engine engine);

    void consumeFinished();
    void wake();
    void listPanes();
    void step(Input const& input);
    void carryOut(std::span<Effect const> effects);
    void stepReplies();
    void release(uint32_t handle);
    void askForWake();
    [[nodiscard]] uint64_t nowMs();

    IGuestHost& _host;
    IDriverObserver& _observer;
    std::string _worktreeRoot; ///< What ReadFile paths are made relative to.
    Engine _engine;
    std::deque<Input> _replies;   ///< The replies the driver made itself, stepped after their batch.
    std::set<uint32_t> _consumed; ///< The finished processes stepped whose release failed.
    WakeTimer _timer;
};

} // namespace orchestrator::guest
