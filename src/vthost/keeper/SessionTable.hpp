// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/codec/Fields.hpp>
#include <core/platform/Clock.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>

namespace vthost::keeper
{

/// How many kinds of descriptor the keeper holds per session.
inline constexpr std::size_t KeptDescriptorCount = core::codec::EnumWire<KeptDescriptor>::Values.size();

/// @return Where descriptors of @p role sit in a SessionTable::Descriptors.
[[nodiscard]] constexpr std::size_t slotOf(KeptDescriptor role) noexcept
{
    return static_cast<std::size_t>(std::to_underlying(role));
}

/// What became of a session whose child was reaped.
enum class ReapedFate : uint8_t
{
    Kept = 0,  ///< The daemon still holds the session: its exit is recorded for the daemon to learn.
    Forgotten, ///< The daemon had released it: the keeper has let it go.
};

/// A reaped session.
struct ReapedSession
{
    KeeperSessionId id {};
    ReapedFate fate = ReapedFate::Kept;
};

/// One session of the inventory: the message and the descriptors that ride with it, in its order.
struct InventoryEntry
{
    InventorySession message;
    std::vector<int> fds; ///< Owned by the table.
};

/// The sessions the keeper holds, by the id it minted for each: the child's pid, the descriptors kept for
/// it, and its exit while the daemon has not let it go.
///
/// A session is forgotten only when both sides are done with it: the daemon released it and its child was
/// reaped. Releasing closes the kept descriptors (the hang-up) but the child's pidfd, which the keeper's
/// event source watches the child by until it is reaped (EventSource::watchPid()), and names when the child
/// is killed if it has not ended by then.
class SessionTable
{
  public:
    /// The descriptors of one session, by KeptDescriptor.
    using Descriptors = std::array<UniqueFd, KeptDescriptorCount>;

    /// Holds a new session.
    /// @param pid The child. @param descriptors What the keeper keeps for it.
    /// @return Its id; never one used before.
    [[nodiscard]] KeeperSessionId add(int pid, Descriptors descriptors);

    /// The daemon placed the session's pane: closes the gate. Unknown ids are ignored.
    void place(KeeperSessionId id);

    /// The daemon let the session go: closes its descriptors but the pidfd; the child is killed at @p killAt
    /// unless it was reaped by then. Unknown ids are ignored.
    void release(KeeperSessionId id, core::platform::SteadyTimePoint killAt);

    /// Releases every session, as release() does, killing none earlier than it would already be.
    void releaseAll(core::platform::SteadyTimePoint killAt);

    /// @param id A session.
    /// @return Its child, while the daemon holds the session and the child was not reaped: the one time it
    /// may
    ///         be signalled. Nothing otherwise.
    [[nodiscard]] std::optional<int> signallableChild(KeeperSessionId id) const;

    /// A child ended.
    /// @param pid The child. @param status How it ended.
    /// @return The session it belonged to and what became of it, or nothing for a pid the table does not
    /// hold.
    [[nodiscard]] std::optional<ReapedSession> reap(int pid, WireExitStatus status);

    /// @param now The time.
    /// @return The children whose kill is due; each is handed out once.
    [[nodiscard]] std::vector<int> takeDueKills(core::platform::SteadyTimePoint now);

    /// @return When the next kill is due, if one is pending.
    [[nodiscard]] std::optional<core::platform::SteadyTimePoint> nextKill() const;

    /// @return The sessions the daemon holds, for an inventory, in id order.
    [[nodiscard]] std::vector<InventoryEntry> inventory() const;

    /// @return Whether no session is held.
    [[nodiscard]] bool empty() const noexcept { return _sessions.empty(); }

  private:
    /// Whether the daemon still holds a session.
    enum class Hold : uint8_t
    {
        Held = 0,
        Released,
    };

    struct Session
    {
        int pid = 0;
        Descriptors descriptors;
        std::optional<WireExitStatus> exited;
        Hold hold = Hold::Held;
        std::optional<core::platform::SteadyTimePoint> killAt;
    };

    std::map<KeeperSessionId, Session> _sessions;
    uint64_t _nextId = 1;
};

} // namespace vthost::keeper
