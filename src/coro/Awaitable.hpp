// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Concepts shared across @c coro awaitables and coroutine promises.

#include <concepts>
#include <coroutine>
#include <utility>

#include <coro/Cancellation.hpp>

namespace coro
{

/// Satisfied by a type that can drive a `co_await` expression directly, i.e. it
/// exposes the awaiter interface (`await_ready`, `await_suspend`, `await_resume`).
///
/// `await_suspend` is intentionally not constrained here because it may legally
/// return `void`, `bool`, or a `std::coroutine_handle<>` (symmetric transfer).
/// @tparam A The candidate awaiter type.
template <typename A>
concept Awaiter = requires(A awaiter, std::coroutine_handle<> continuation) {
    { awaiter.await_ready() } -> std::convertible_to<bool>;
    awaiter.await_suspend(continuation);
    awaiter.await_resume();
};

/// Satisfied by a coroutine promise that carries a cancellation @c StopToken.
///
/// Runtime awaitables read the awaiting coroutine's token through this interface
/// in their templated `await_suspend`, so cancellation propagates down a
/// structured chain of `co_await`s without threading a token through every call.
/// @tparam P The candidate promise type.
template <typename P>
concept HasStopToken = requires(P& promise) {
    { promise.stopToken() } -> std::convertible_to<StopToken>;
};

} // namespace coro
