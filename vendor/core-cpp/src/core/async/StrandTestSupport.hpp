// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Helpers the async module's strand cases share (`Strand_test.cpp`, `KeyedStrands_test.cpp`).
/// Test-only: in no `FILE_SET`, so never installed.

#include <core/async/DetachedTask.hpp>

#include <coroutine>

namespace core::async::test
{

/// Records the awaiting coroutine's handle and stays suspended.
struct ParkSelf
{
    std::coroutine_handle<>* self;
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const noexcept { *self = handle; }
    void await_resume() const noexcept {}
};

/// A detached chain that parks at once, carrying @p sentinel: its frame is its root, and the
/// sentinel says when that frame is freed.
///
/// The statement after the `co_await` is load-bearing under MSVC's `cl` at /O2 (19.51): a frame
/// destroyed at a suspension point with nothing after it in the body there never destroyed its
/// by-value parameters, so the sentinel read "not freed" for a frame that was (core-cpp#54).
/// @tparam Sentinel A type that records its own destruction.
/// @param self Where the chain's handle is recorded.
/// @param sentinel Held by the frame for its life.
template <typename Sentinel>
DetachedTask parkDetached(std::coroutine_handle<>* self, Sentinel sentinel)
{
    (void) sentinel;
    co_await ParkSelf { self };
    *self = {};
}

} // namespace core::async::test
