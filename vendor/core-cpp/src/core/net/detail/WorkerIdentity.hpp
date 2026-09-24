// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Who is driving a loop, if anyone.
///
/// Two facts, held together because neither answers anything alone: whether a thread is
/// currently dequeuing for this loop, and which thread that is. A caller about to destroy
/// something the loop owns needs both, and needs them as ONE observation — "is a worker
/// running" and "am I it" read separately can straddle the moment the loop stops.
///
/// Ported from fastcached's `Async/ReactorWorkerIdentity.hpp` at `0708dd54`. Origin:
/// [fastcached#668](https://github.com/LASTRADA-Software/fastcached/issues/668).

#include <atomic>
#include <thread>

namespace core::net::detail
{

/// Which thread is inside a loop's turn, if any.
///
/// **Two variables rather than one**, and that is not redundancy: a default-constructed
/// `std::thread::id` is a legal value to compare against, so a single id field cannot tell
/// "nobody is running" from "some thread whose id happens to compare equal to the empty one".
class WorkerIdentity
{
  public:
    /// Marks the calling thread as this loop's worker for the scope of one drive.
    ///
    /// RAII rather than a matched pair of calls, for the reason every other guard in this tree
    /// is: a loop has more than one way out — the loop condition, an early return, a throw from
    /// a resumed coroutine — and the exit that gets forgotten leaves the loop claiming a worker
    /// that has gone, which is the FALSE-SAFE direction. A teardown would then be told it was on
    /// the worker thread when no thread is dequeuing at all.
    ///
    /// **Re-entrant by design.** A loop is driven through `run()`, `runOnce()` and `blockOn()`,
    /// and `run()` is a `runOnce()` in a loop — so the claim nests, and only the outermost scope
    /// releases it. A non-nesting claim would make the first inner turn release the outer one and
    /// leave `run()` reporting that nobody is driving.
    class Scope
    {
      public:
        /// @param identity The identity to claim for the calling thread.
        explicit Scope(WorkerIdentity& identity) noexcept:
            _identity { identity }, _outermost { identity._depth == 0 }
        {
            // An inner scope must not re-publish the thread id: it is already this thread's, and
            // a store here would race a reader that is mid-comparison for no gain.
            if (_outermost)
            {
                _identity._workerThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
                _identity._running.store(true, std::memory_order_release);
            }
            ++_identity._depth;
        }

        ~Scope()
        {
            --_identity._depth;
            if (_outermost)
                _identity._running.store(false, std::memory_order_release);
        }

        Scope(Scope const&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope const&) = delete;
        Scope& operator=(Scope&&) = delete;

      private:
        WorkerIdentity& _identity;
        bool _outermost; ///< Whether this scope is the one that claimed, and so the one to release.
    };

    /// Whether a thread is currently driving the loop.
    ///
    /// False before the first turn is entered and after the last one returns, which is the honest
    /// answer: with nothing dequeuing there is no worker thread to be on. A caller that
    /// legitimately tears down outside a running loop asks this first rather than reading
    /// @c isOnWorkerThread()'s false as a violation.
    /// @return True between entry to and return from the outermost drive.
    [[nodiscard]] bool running() const noexcept { return _running.load(std::memory_order_acquire); }

    /// @return True when the calling thread is the one currently driving the loop.
    [[nodiscard]] bool isOnWorkerThread() const noexcept
    {
        return _running.load(std::memory_order_acquire)
               && _workerThread.load(std::memory_order_relaxed) == std::this_thread::get_id();
    }

  private:
    std::atomic<std::thread::id> _workerThread {};
    std::atomic<bool> _running { false };
    /// How many nested drives the worker thread is inside. Touched only by that thread, so it is
    /// a plain counter: a second thread reaching it at all is the G1 violation the loop asserts.
    unsigned _depth = 0;
};

} // namespace core::net::detail
