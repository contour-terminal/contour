// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The browser as a host: `emscripten_async_call`, which is `setTimeout`.
///
/// No `#ifdef __EMSCRIPTEN__` guards this file. CMake's `SOURCES_EMSCRIPTEN` list is
/// what selects it, as it does for every other platform directory (Ruling R42), and
/// it is private: a program gets a host-driven backend from @c makeDefaultBackend().

#include <core/net/IHostScheduler.hpp>

#include <chrono>

namespace core::net
{

/// An @c IHostScheduler over the browser's timer.
///
/// Stateless, because `emscripten_async_call` is: every request carries its own
/// callback and its own state, and there is nothing to cancel — which is exactly why
/// @c HostDrivenBackend coalesces on its own side rather than retracting a timer, and
/// why what it hands this as state is a ticket that outlives the backend rather than the
/// backend's own address. Nothing here outlives a request, so nothing here can dangle.
class EmscriptenHostScheduler final: public IHostScheduler
{
  public:
    /// Asks the browser to call @p fn with @p state after @p delay, through
    /// `emscripten_async_call`. A delay of zero becomes a `setTimeout(…, 0)`, which
    /// is the next turn of the browser's loop and not a synchronous call — which is
    /// what @c IHostScheduler::callAfter requires.
    /// @param delay How long to wait.
    /// @param fn The callback.
    /// @param state Passed to @p fn untouched.
    void callAfter(std::chrono::milliseconds delay, HostCallback fn, void* state) override;
};

} // namespace core::net
