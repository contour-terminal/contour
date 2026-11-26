// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A thread whose stack size is chosen: what std::thread and std::jthread cannot do, and what a lane executor
/// needs, since the guest calls it nests run on its stack.

#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <system_error>

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
    /// @return The running thread, once it entered @p body: its start-up is behind it, so that a sandbox
    ///         installed afterwards never meets it; why the platform could not start it, @p body then never
    ///         running.
    [[nodiscard]] static std::expected<ExecutorThread, std::error_code> start(std::size_t stackBytes,
                                                                              std::function<void()> body);

    ExecutorThread(ExecutorThread&&) noexcept;
    ExecutorThread& operator=(ExecutorThread&&) noexcept;
    ExecutorThread(ExecutorThread const&) = delete;
    ExecutorThread& operator=(ExecutorThread const&) = delete;
    /// Joins the thread, unless it was joined or moved from.
    ~ExecutorThread();

    /// Waits until the thread finished. Idempotent.
    void join() noexcept;

  private:
    explicit ExecutorThread(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> _state;
};

/// Starts a thread of a chosen stack, as ExecutorThread::start does: the operating system's threads, but in a
/// test one that records or fails each start.
using ExecutorStarter =
    std::function<std::expected<ExecutorThread, std::error_code>(std::size_t, std::function<void()>)>;

} // namespace vtextension::wasm::host
