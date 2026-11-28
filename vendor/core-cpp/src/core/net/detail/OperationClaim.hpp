// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `detail::OperationClaim` and `detail::holding` — how an object that allows one operation of a
/// kind in flight marks it taken, and gives it back when the task that carries it is gone.
///
/// Shared by the loop-driven datagram socket (a receive and a send) and the standard streams (a
/// read, or a write), which refuse a second operation with `BadHandle` while the first is out.

#include <core/async/Task.hpp>

#include <memory>
#include <utility>

namespace core::net::detail
{

/// One in-flight operation's hold on a predicate of @p State, given back when the task that
/// carries it is destroyed -- finished, or dropped before it ever ran.
///
/// The state is shared rather than borrowed, because a task may outlive the object that handed
/// it out.
/// @tparam State The owner's record of what it has in flight.
template <typename State>
class OperationClaim
{
  public:
    /// @param state The owner's record.
    /// @param slot Which of its predicates this claim holds; set here.
    OperationClaim(std::shared_ptr<State> state, bool State::* slot) noexcept:
        _state { std::move(state) }, _slot { slot }
    {
        (*_state).*_slot = true;
    }

    ~OperationClaim()
    {
        if (_state != nullptr)
            (*_state).*_slot = false;
    }
    OperationClaim(OperationClaim const&) = delete;
    OperationClaim& operator=(OperationClaim const&) = delete;
    OperationClaim(OperationClaim&& other) noexcept:
        _state { std::exchange(other._state, nullptr) }, _slot { other._slot }
    {
    }
    OperationClaim& operator=(OperationClaim&&) = delete;

  private:
    std::shared_ptr<State> _state;
    bool State::* _slot;
};

/// @tparam State The owner's record, as in @c OperationClaim.
/// @tparam T What @p task answers.
/// @param claim The operation's hold on its slot.
/// @param task The operation.
/// @return @p task, holding @p claim until it finishes. A coroutine parameter, so a task dropped
///         unstarted gives the claim back with its frame; moved into the body once it runs, so
///         a finished task gives it back at once rather than when its owner destroys it.
template <typename State, typename T>
async::Task<T> holding(OperationClaim<State> claim, async::Task<T> task)
{
    [[maybe_unused]] auto const held = std::move(claim);
    co_return co_await std::move(task);
}

} // namespace core::net::detail
