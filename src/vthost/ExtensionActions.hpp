// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What invoking a daemon-scoped extension's palette row does, by where the extension stands, and the queue
/// of invoked actions an extension takes with `actions-take`, each with the connection it came from.

#include <core/platform/Clock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/ConnectionId.hpp>
#include <vthost/ExtensionHosting.hpp>

namespace vthost
{

/// What invoking one of a daemon-scoped extension's palette rows does.
enum class InvocationEffect : uint8_t
{
    Deliver = 0,      ///< The action is queued for the extension, which is told (ServedHint::Actions).
    StartThenDeliver, ///< The extension is started on demand first, then the action is queued.
    AskAgain,         ///< The invoking window is sent the extension's pending grant question again.
    Restart,          ///< The disabled extension is started again, with a fresh fault window.
    Ignore,           ///< Nothing: the extension was refused, or cannot run. Logged.
};

/// One row of what an invocation does, by the extension's state.
struct InvocationRow
{
    DaemonExtensionState state; ///< Where the extension stands.
    InvocationEffect effect;    ///< What invoking one of its rows does then.
};

/// What an invocation does, one row per DaemonExtensionState.
inline constexpr auto InvocationRows = std::array {
    InvocationRow { .state = DaemonExtensionState::Running, .effect = InvocationEffect::Deliver },
    InvocationRow { .state = DaemonExtensionState::Stopped, .effect = InvocationEffect::StartThenDeliver },
    InvocationRow { .state = DaemonExtensionState::Pending, .effect = InvocationEffect::AskAgain },
    InvocationRow { .state = DaemonExtensionState::Disabled, .effect = InvocationEffect::Restart },
    InvocationRow { .state = DaemonExtensionState::Denied, .effect = InvocationEffect::Ignore },
    InvocationRow { .state = DaemonExtensionState::Failed, .effect = InvocationEffect::Ignore },
};

/// The most invoked actions queued per extension; the oldest is dropped first.
inline constexpr std::size_t MaxQueuedActions = 32;

/// @param state Where an extension stands.
/// @return What invoking one of its rows does.
[[nodiscard]] constexpr InvocationEffect invocationEffectOf(DaemonExtensionState state) noexcept
{
    for (auto const& row: InvocationRows)
        if (row.state == state)
            return row.effect;
    return InvocationEffect::Ignore;
}

/// The production IExtensionActions: the actions the user invoked, queued per extension, the origin each
/// connection is given, and which extension each origin was handed to.
///
/// originOf(), forget() and deliver() are for the host's loop thread; take(), connectionOf() and
/// claimReveal() for any thread. One mutex guards the queues and the origin table.
class ExtensionActions final: public IExtensionActions
{
  public:
    /// @param raise Tells an extension why it is to be called: ServedHint::Actions at each delivery. Called
    ///        on the loop thread, holding no lock.
    /// @param clock The host loop's steady clock: when an action was invoked, and how long ago that was when
    ///        a reveal claims it; thread-safe, and it must outlive this.
    ExtensionActions(std::function<void(std::string_view extension, ServedHint hint)> raise,
                     core::platform::IClock const& clock);

    /// @param connection A connection attached now.
    /// @return Its origin: minted at its first call, the same at every later one, never reused.
    [[nodiscard]] Origin originOf(ConnectionId const& connection);

    /// Forgets @p connection, at its detach: its origin then answers nullopt from connectionOf() and
    /// claimReveal().
    /// @param connection The connection that detached.
    void forget(ConnectionId const& connection);

    /// Queues @p action for @p extension, and raises ServedHint::Actions: on every delivery, as the runner
    /// coalesces them, so that one an extension missed (it faulted before it took them) is raised again.
    /// Beyond MaxQueuedActions the oldest is dropped, and logged. The user invoked it now: @p origin may
    /// reveal once for @p extension, for ServedInputGrace (claimReveal()).
    /// @param extension What the daemon's configuration calls it.
    /// @param action The declared action's id.
    /// @param origin The connection it came from (originOf()).
    void deliver(std::string_view extension, std::string_view action, Origin origin);

    /// @param extension What the daemon's configuration calls an extension.
    /// @return Whether actions wait for it to take them. From any thread.
    [[nodiscard]] bool holdsActionsFor(std::string_view extension) const;

    [[nodiscard]] std::vector<std::pair<std::string, Origin>> take(std::string_view extension) override;
    [[nodiscard]] std::optional<ConnectionId> connectionOf(std::string_view extension,
                                                           Origin origin) const override;
    [[nodiscard]] std::optional<ConnectionId> claimReveal(std::string_view extension, Origin origin) override;

  private:
    std::function<void(std::string_view, ServedHint)> _raise;
    core::platform::IClock const& _clock;
    mutable std::mutex _mutex;
    /// The queued actions, per extension. Guarded by _mutex.
    std::map<std::string, std::deque<std::pair<std::string, Origin>>, std::less<>> _queues;
    /// The attached connections' origins. Guarded by _mutex.
    std::vector<std::pair<ConnectionId, Origin>> _origins;
    /// The origins take() handed each extension, while their connection is attached. Guarded by _mutex.
    std::map<std::string, std::vector<Origin>, std::less<>> _handedOut;
    /// Per extension, the origins an action was invoked through that no reveal claimed yet, with when that
    /// was: one entry per attached origin at most. Guarded by _mutex.
    std::map<std::string, std::vector<std::pair<Origin, core::platform::SteadyTimePoint>>, std::less<>>
        _unclaimed;
    Origin _nextOrigin = 1; ///< Guarded by _mutex.
};

} // namespace vthost
