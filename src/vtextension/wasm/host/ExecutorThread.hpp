// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A thread whose stack size is chosen: what std::thread and std::jthread cannot do, and what a lane executor
/// needs, since the guest calls it nests run on its stack.

#include <cstddef>
#include <functional>
#include <memory>

namespace vtextension::wasm::host
{

/// A thread running one function on a stack of a chosen size, joined when it is destroyed. Move-only.
class ExecutorThread
{
  public:
    /// The platform's handle and the function it runs, at a stable address the thread is handed; defined in
    /// ExecutorThread.cpp.
    struct State;

    /// Starts @p body on a new thread.
    /// @param stackBytes The least stack the thread gets; the platform's minimum if that is larger.
    /// @param body What the thread runs. Must not throw.
    /// @throws std::system_error if the thread could not be started, as std::thread does.
    ExecutorThread(std::size_t stackBytes, std::function<void()> body);
    ExecutorThread(ExecutorThread&&) noexcept;
    ExecutorThread& operator=(ExecutorThread&&) noexcept;
    ExecutorThread(ExecutorThread const&) = delete;
    ExecutorThread& operator=(ExecutorThread const&) = delete;
    /// Joins the thread, unless it was joined or moved from.
    ~ExecutorThread();

    /// Waits until the thread finished. Idempotent.
    void join() noexcept;

  private:
    std::unique_ptr<State> _state;
};

} // namespace vtextension::wasm::host
