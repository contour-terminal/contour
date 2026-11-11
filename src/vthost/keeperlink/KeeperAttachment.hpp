// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>

namespace vthost::keeperlink
{

/// How long the handshake with the keeper may take before the daemon gives up on it.
inline constexpr std::chrono::seconds AttachBound { 30 };

/// A daemon's link to its keeper as the hidden verb hands it to runDaemon(): past `Hello` and `Launch`, and
/// for a Recovered start past the inventory too. Built before runDaemon(), which builds the KeeperLink on it
/// once its loop exists.
struct KeeperAttachment
{
    KeeperChannel channel;                                                 ///< Past Hello and Launch.
    keeper::LaunchContinuity continuity = keeper::LaunchContinuity::Fresh; ///< How this start began.
    std::vector<uint8_t> options; ///< The Launch's options record, opaque to the keeper.
    uint32_t version = 0;         ///< The channel version both speak.
    int64_t keeperProcessId = 0;  ///< The keeper: the daemon's parent.
    /// Recovered only: InventoryBegin … InventoryEnd, in order, each with its descriptors.
    std::vector<ReceivedMessage> inventory;
};

/// Why the handshake with the keeper failed.
enum class AttachError : uint8_t
{
    Closed = 0, ///< The keeper closed the channel.
    Broken,     ///< The channel broke, or the keeper sent what the handshake does not allow.
    TimedOut,   ///< The keeper did not answer within the bound.
    /// The descriptor is no keeper's channel (not an AF_UNIX stream socket): nothing was written to it, as
    /// when `keeper-serve` is run by hand.
    NotAChannel,
};

/// @param error Why the handshake failed.
/// @return The reason, as a sentence fragment.
[[nodiscard]] std::string_view describe(AttachError error) noexcept;

/// The daemon's side of the handshake, synchronously, before anything else of the daemon runs: says Hello,
/// reads the keeper's Hello and Launch, and for a Recovered start asks for the inventory (Adopt) and reads
/// it to its end. Anything else the keeper sent before the Launch (what a new image of a live upgrade finds
/// queued for its predecessor) stays on the channel, for the link.
/// @param socket The channel (`--keeper-channel`).
/// @param clock The clock @p bound is measured on.
/// @param bound How long the keeper may take.
/// @return The attachment; or why there is none.
[[nodiscard]] std::expected<KeeperAttachment, AttachError> attachToKeeper(keeper::UniqueFd socket,
                                                                          core::platform::IClock const& clock,
                                                                          std::chrono::milliseconds bound);

} // namespace vthost::keeperlink
