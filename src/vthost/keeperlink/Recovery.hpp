// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A daemon restarted after a crash, rebuilt from what its PTY keeper kept: the structure the dead daemon
/// pushed, and per session the PTY master and the child the keeper holds.
///
/// The result is a handover image, as a live upgrade's is, adopted the same way
/// (SessionHostConfig::handover): the same instance, the same ids, every session bound to its keeper child
/// again. What a live upgrade carries and a crash cannot -- terminal state, input in flight -- is absent;
/// each terminal starts fresh at its PTY's size, seeded from the workspace snapshot, and its program is asked
/// to redraw.

#include <vtpty/ImageSize.hpp>
#include <vtpty/OwnedFd.hpp>
#include <vtpty/Pty.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <vthost/SessionHost.hpp>
#include <vthost/handover/HandoverImage.hpp>
#include <vthost/handover/HandoverMemory.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>
#include <vthost/keeperlink/KeeperChild.hpp>

namespace vthost::keeperlink
{

/// What of an inventory could not be recovered as it was.
enum class RecoveryError : uint8_t
{
    /// The keeper held no structure: every session comes back in a tab of its own, under a fresh id.
    NoStructure = 0,
    /// The structure did not read (a newer release wrote it, or it is damaged): as NoStructure.
    StructureUnreadable,
    /// A session came without its PTY master: it cannot be served, and is released.
    SessionWithoutPty,
};

/// @param error What could not be recovered.
/// @return It, as a sentence fragment.
[[nodiscard]] std::string_view describe(RecoveryError error) noexcept;

/// A child the keeper held, as a recovery binds it again.
struct RecoveredChild
{
    KeptChild kept;               ///< The child.
    vtpty::OwnedFd gate;          ///< Its placement gate, for a child never placed (Parked); none otherwise.
    vtpty::OwnedFd processHandle; ///< Its pidfd, where the keeper sent one.
};

/// One thing of an inventory that could not be recovered as it was.
struct RecoveryProblem
{
    RecoveryError error = RecoveryError::NoStructure;
    uint64_t session = 0; ///< The keeper's name for the session it is about; 0 for the structure.
};

/// What a daemon restarted after a crash starts from.
struct Recovery
{
    /// The structure, every session in it bound to the PTY the keeper kept, without terminal state; a session
    /// the keeper kept that the structure lacks is in it too, unnamed (id 0: it gets a fresh id and a tab of
    /// its own). The listeners are the keeper's, each with the TLS identity the structure names for it.
    handover::HandoverImage image;
    /// The children, by the keeper's name for each.
    std::map<uint64_t, RecoveredChild> children;
    /// The sessions the keeper kept that cannot be served: to be released.
    std::vector<keeper::KeeperSessionId> unbound;
    /// What could not be recovered as it was.
    std::vector<RecoveryProblem> problems;
};

/// Rebuilds a daemon from its keeper's inventory. Takes every descriptor out of @p inventory: what the result
/// does not name is closed with it.
/// @param inventory InventoryBegin … InventoryEnd, each with its descriptors (KeeperAttachment::inventory).
/// @param memory Reads the structure's memory object.
/// @return The recovery.
[[nodiscard]] Recovery imageFromInventory(std::span<ReceivedMessage> inventory,
                                          handover::HandoverMemory& memory);

/// @param master A PTY master.
/// @return The terminal size the master states (TIOCGWINSZ); nullopt where it states none.
[[nodiscard]] std::optional<vtpty::PageSize> pageSizeOf(int master);

/// @param master A PTY master.
/// @return The terminal size in pixels the master states (TIOCGWINSZ); nullopt where it states none.
[[nodiscard]] std::optional<vtpty::ImageSize> pixelSizeOf(int master);

/// Asks the program in the foreground of @p pty to redraw: SIGWINCH to its process group, which a resize to
/// the size it has already would not send (Linux and XNU skip it).
/// @param pty A PTY with a master (a Unix one).
void nudgeRedraw(vtpty::Pty const& pty);

/// @param restore A workspace snapshot, as read; its panes' histories move into the seeder, the file stays
///        where it is.
/// @return What seeds a recovered session's fresh terminal with the history the snapshot holds for its pane
///         (matched by the pane's UUID), behind the snapshot's divider; nothing for a pane it does not hold.
[[nodiscard]] RecoveredSeeder snapshotSeederOf(WorkspaceRestore restore);

} // namespace vthost::keeperlink
