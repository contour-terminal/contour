// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ChildProcess.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <vector>

#include <vthost/keeper/Messages.hpp>
#include <vthost/keeper/UniqueFd.hpp>
#include <vthost/keeperlink/KeeperChannel.hpp>

namespace core::net
{
class EventLoop;
}

namespace vthost::keeperlink
{

/// How long spawn() waits for the keeper's answer before it gives up on it.
inline constexpr std::chrono::seconds SpawnReplyBound { 10 };

/// How long the link, as it goes, waits for the keeper to take what is still queued (a Shutdown).
inline constexpr std::chrono::seconds FinalFlushBound { 1 };

/// What spawn() hands back: the keeper's Spawned, and the child's process handle (its pidfd) if it sent one.
struct SpawnReply
{
    keeper::Spawned spawned;
    keeper::UniqueFd processHandle;
};

/// The daemon's link to its PTY keeper, on the daemon's event loop.
///
/// It reads the channel on the loop and hands each message to its handler there; it writes through one
/// queue that any thread may post to. Frames go out in order; one that carries descriptors is sent with a
/// `sendmsg` of its own, its descriptors on its first byte, as keeper::FrameAssembler requires, while the
/// frames between such frames are gathered into one send.
///
/// The keeper's end of the link is the keeper's alone to close: when it closes (the keeper ended, or dropped
/// a daemon that would not read), the link reports the loss once, through `onLost`, and the daemon shuts
/// down. A daemon that only shuts down its read side (`shutdown(SHUT_RD)`) is not lost to the keeper: the
/// keeper's link to it stays write-closed while the daemon runs, so the daemon's heartbeats still arrive and
/// what the keeper sends it is dropped. Nothing here does that; it is noted so that nothing starts to.
///
/// Loop thread, except post(); built and destroyed on the thread that drives the loop.
class KeeperLink
{
  public:
    /// @param loop The daemon's loop (not owned; outlives the link).
    /// @param channel The channel, past the handshake (@see attachToKeeper()).
    /// @param channelVersion The channel version the handshake negotiated (KeeperAttachment::version).
    /// @param onLost Called once, on the loop, when the keeper's end is gone or the channel broke.
    KeeperLink(core::net::EventLoop& loop,
               KeeperChannel channel,
               uint32_t channelVersion,
               std::function<void()> onLost);
    KeeperLink(KeeperLink const&) = delete;
    KeeperLink& operator=(KeeperLink const&) = delete;
    KeeperLink(KeeperLink&&) = delete;
    KeeperLink& operator=(KeeperLink&&) = delete;
    /// Sends what is still queued, for at most FinalFlushBound, and stops reading.
    ~KeeperLink();

    /// Asks the keeper to fork a child, and waits for its answer: the one channel round trip a session's
    /// creation may add. Loop thread. Every frame queued before goes first. The frames that arrive meanwhile
    /// and are not the answer are queued, and dispatched after this returns, never inside it.
    /// @param request What to spawn; its `request` number is assigned here.
    /// @param fds The descriptors the Spawn carries; they stay the caller's.
    /// @return The answer; or why there is none (SpawnError::Unknown for a lost link or a late keeper).
    [[nodiscard]] std::expected<SpawnReply, keeper::SpawnError> spawn(keeper::Spawn request,
                                                                      std::vector<int> fds);

    /// Tells the keeper the daemon is alive, sending at once what the socket takes: from the thread that
    /// drives the loop, also before the loop runs, where the loop's own heartbeat flow cannot.
    void beat();

    /// Queues @p message for the keeper. Any thread; one writer queue.
    /// @param message The message.
    /// @param fds Descriptors that ride with it; duplicated before this returns, so they stay the caller's.
    void post(keeper::Message const& message, std::vector<int> const& fds = {});

    /// Sets what an Exited is handed to. Loop thread. The session layer that spawns through the keeper sets
    /// it once it exists, which is after the link: the two know each other (cyclic wiring).
    /// @param handler Called on the loop with the session and how its child ended.
    void onExited(std::function<void(keeper::KeeperSessionId, vtpty::ExitStatus)> handler);

    /// @return The channel's descriptor: what a live upgrade's exec keeps across, for the new image.
    [[nodiscard]] int channelHandle() const noexcept;

    /// @return The channel version both ends speak: what gates a message, enum value or descriptor-carrying
    ///         field that a later version adds (@see keeper::Hello).
    [[nodiscard]] uint32_t channelVersion() const noexcept;

    /// @return Whether the keeper's end is gone.
    [[nodiscard]] bool lost() const noexcept;

  private:
    struct State;
    std::shared_ptr<State> _state;
};

} // namespace vthost::keeperlink
