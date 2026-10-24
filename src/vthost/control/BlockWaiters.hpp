// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The calls waiting for a command block to end: `block.wait` and `pane.send` with a wait.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <unordered_map>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/ControlApi.hpp>
#include <vtcontrol/ControlError.hpp>
#include <vtcontrol/ObjectRef.hpp>
#include <vtcontrol/Uuid.hpp>

namespace vthost::control
{

/// Completes waiters as blocks end. Loop thread only, like the EventHub: what ends a block is a host signal
/// that has already reached the loop.
///
/// A waiter is completed exactly once: by the block it waits for ending, by its pane closing, or never, when
/// its sink goes first (forget()), whose connection is gone and has nobody to answer.
///
/// A sink waits for at most a fixed number of blocks at once, so that a connection pipelining waits without
/// reading costs the daemon bounded memory. A wait past that share is refused: its completion is answered
/// `LimitReached` at once, and the same error is returned, so that the caller does not start what it would
/// have waited for.
class BlockWaiters
{
  public:
    /// @param maxPerSink The most waits one sink may have pending at once.
    explicit BlockWaiters(std::size_t maxPerSink): _maxPerSink(maxPerSink) {}

    /// Waits for one block.
    /// @param sink The connection waiting. Borrowed: forget() it before it goes.
    /// @param block The block.
    /// @param done Completed with the block once it is Finished or Superseded.
    /// @return Nothing, or `LimitReached` -- @p done answered with it -- when @p sink waits for its share.
    std::expected<void, vtcontrol::ControlError> waitFor(vtcontrol::EventSink& sink,
                                                         vtcontrol::BlockRef block,
                                                         vtcontrol::Completion<vtcontrol::BlockInfo> done);

    /// Waits for the first block of @p pane minted after another one to end.
    /// @param sink The connection waiting. Borrowed: forget() it before it goes.
    /// @param pane The pane.
    /// @param afterSerial The serial of the newest block at the time of asking; zero for none.
    /// @param done Completed with the block once it is Finished or Superseded.
    /// @return Nothing, or `LimitReached` -- @p done answered with it -- when @p sink waits for its share.
    std::expected<void, vtcontrol::ControlError> waitForNext(
        vtcontrol::EventSink& sink,
        vtcontrol::Uuid pane,
        uint64_t afterSerial,
        vtcontrol::Completion<vtcontrol::BlockInfo> done);

    /// Completes the waiters of @p block.
    /// @param block A block that is now Finished or Superseded.
    void blockEnded(vtcontrol::BlockInfo const& block);

    /// Completes every waiter of @p pane with `NotFound` "pane … closed".
    /// @param pane The pane that closed.
    void paneClosed(vtcontrol::Uuid pane);

    /// Drops every waiter of @p sink without completing it.
    /// @param sink The sink.
    void forget(vtcontrol::EventSink& sink);

    /// @return How many calls wait.
    [[nodiscard]] std::size_t size() const noexcept { return _waiters.size(); }

    /// @param sink A sink.
    /// @return How many of its calls wait.
    [[nodiscard]] std::size_t countOf(vtcontrol::EventSink const& sink) const noexcept;

  private:
    /// A call waiting for the first block of its pane whose serial lies in [@ref from, @ref to] to end.
    struct Waiter
    {
        vtcontrol::EventSink* sink = nullptr;
        vtcontrol::Uuid pane;
        uint64_t from = 0;
        uint64_t to = std::numeric_limits<uint64_t>::max();
        vtcontrol::Completion<vtcontrol::BlockInfo> done;
    };

    /// Adds @p waiter, or answers it `LimitReached` when its sink waits for its share.
    std::expected<void, vtcontrol::ControlError> add(Waiter waiter);

    /// Takes the waiters @p matches accepts out of the list, so that completing them may add waiters.
    template <typename Matches>
    [[nodiscard]] std::vector<Waiter> take(Matches const& matches);

    std::size_t _maxPerSink;
    std::vector<Waiter> _waiters;
    std::unordered_map<vtcontrol::EventSink const*, std::size_t> _perSink; ///< Waiters per sink.
};

} // namespace vthost::control
