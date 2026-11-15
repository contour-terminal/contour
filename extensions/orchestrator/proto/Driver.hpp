// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>

#include <orchestrator/core/Config.hpp>
#include <orchestrator/core/Engine.hpp>
#include <orchestrator/proto/Control.hpp>
#include <orchestrator/proto/RunChanges.hpp>
#include <orchestrator/proto/Spawn.hpp>
#include <orchestrator/proto/StepQueue.hpp>
#include <orchestrator/proto/Store.hpp>

namespace orchestrator::proto
{

/// Whether the driver may go on.
enum class DriverHealth : uint8_t
{
    Healthy,     ///< Every effect was carried out.
    StoreFailed, ///< A store write failed: carrying on would run a command its run's state was not stored
                 ///< for.
};

/// What the driver works with, fixed for its lifetime.
struct DriverSetup
{
    HostInfo host;                        ///< The resolved paths.
    std::string configJson;               ///< The configuration, as JSON.
    FileStore store;                      ///< The extension's store.
    ControlApi control;                   ///< Contour's control API.
    std::vector<std::string> environment; ///< What commands inherit; it holds no `CONTOUR_*` variable.
};

/// Drives the engine against the real world, on one thread: every effect of a step is carried out, in order,
/// before the next input is fed; the inputs those effects produce are fed after it, in the order they
/// arose; and the panes a Wake asks for are listed only once every queued input was fed (see StepQueue).
///
/// It validates the workflow only: it neither enforces the roots nor asks for consent, so nothing it does
/// says the host's enforcement works. Its pane listing is PaneListing::OwnRecordsOnly, built from the panes
/// its runs recorded, since Contour's listing carries no tag.
class Driver
{
  public:
    /// Starts the engine from the store and the panes listed now, and carries out its first effects.
    /// @param setup What the driver works with.
    /// @return The driver; or how carrying out the first effects failed.
    [[nodiscard]] static std::expected<Driver, DriverHealth> start(DriverSetup setup);

    /// Feeds an input, and everything its effects lead to.
    /// @param input What happened.
    /// @return Whether the driver may go on.
    [[nodiscard]] DriverHealth feed(Input input);

    /// Appends what the loop must poll for the running commands.
    /// @param fds The poll set.
    void addPollFds(std::vector<pollfd>& fds) const;
    /// @param nowMs Now, in monotonic milliseconds.
    /// @return How long the loop may sleep before the driver has something to do; -1: until input arrives.
    [[nodiscard]] int pollTimeoutMs(uint64_t nowMs) const;
    /// Collects the commands' output, finishes the ones that ended or are overdue, and wakes the engine when
    /// it asked to be woken.
    /// @param fds What poll(2) returned.
    /// @return Whether the driver may go on.
    [[nodiscard]] DriverHealth service(std::span<pollfd const> fds);

    Driver(Driver const&) = delete;
    Driver& operator=(Driver const&) = delete;
    Driver(Driver&&) = default;
    Driver& operator=(Driver&&) = default;
    /// Kills every running command's process group and reaps it, on every way out: no command outlives the
    /// driver, and its run learns at the next start that the call was lost.
    ~Driver() { stopCommands(); }

    /// @return The engine, for the prompt to read.
    [[nodiscard]] Engine const& engine() const noexcept { return _engine; }
    /// @return The validated configuration; std::nullopt when the engine reports it as a problem.
    [[nodiscard]] std::optional<Config> const& config() const noexcept { return _config; }

  private:
    Driver(Engine engine, DriverSetup setup);

    [[nodiscard]] DriverHealth drain();
    void stopCommands() noexcept;
    [[nodiscard]] DriverHealth carryOut(std::vector<Effect> const& effects);
    [[nodiscard]] std::expected<Child, StartResult> startCommand(RunCommand const& command) const;
    void runCommand(RunCommand const& command);
    [[nodiscard]] std::expected<std::string, std::string> openTabFor(OpenPane const& open) const;
    void openPane(OpenPane const& open);
    [[nodiscard]] std::optional<PanesListed> listPanes() const;

    Engine _engine;
    DriverSetup _setup;
    std::optional<Config> _config;
    std::vector<std::pair<std::string, Child>> _children; ///< The running commands, by tag.
    StepQueue _queue; ///< The inputs the effects produced, in order, and the listing a Wake asks for.
    std::optional<uint64_t> _wakeAtMs;
    std::string _publishedShort;
    ChangeReport _report; ///< Says each run's changes, a kept worktree's re-check by its outcome only.
};

} // namespace orchestrator::proto
