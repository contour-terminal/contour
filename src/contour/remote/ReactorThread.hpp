// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ReactorThread` — a core::net::EventLoop running on its own thread, beside Qt's.
///
/// The GUI's remote controllers (native protocol, tmux mirroring) do all their
/// socket and protocol work on this reactor; the ONLY thread-safe entry into
/// it is post(). Data flowing the other way needs no Qt marshaling either:
/// it lands in a thread-safe vtpty::ChannelPty::feed, and the session's own
/// parser thread does the rest — the identical threading a local session has.

#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace contour::remote
{

/// @brief Owns a core::net::EventLoop running on a dedicated thread.
///
/// Two-phase initialization (default-construct then start()) is necessary because
/// the root coroutine task is supplied by the owning RemoteController at connect
/// time, not at construction time — the reactor is allocated before the concrete
/// controller knows its connection type.
///
/// @note The owner must ensure the root task finishes (or requestStop() is posted
///       into the loop) before destruction, or the destructor blocks forever.
class ReactorThread
{
  public:
    ReactorThread() = default;

    /// Joins; the owner must have made the root task finish first (post a
    /// shutdown into the loop), or this blocks forever by design.
    ~ReactorThread() { join(); }

    ReactorThread(ReactorThread const&) = delete;
    ReactorThread& operator=(ReactorThread const&) = delete;
    ReactorThread(ReactorThread&&) = delete;
    ReactorThread& operator=(ReactorThread&&) = delete;

    /// Starts the thread, running @p rootTask to completion on the loop.
    /// A cancellation unwind (requestStop) ends the task silently.
    /// @param rootTask The coroutine factory that produces the root task to run
    ///        on the loop (e.g. NativeClient::runClient or
    ///        TmuxClientModel::runControlMode).
    /// @param onFailure Invoked ON THE REACTOR THREAD, once, with the reason when the root task
    ///        ends by an exception other than cancellation — so the owner can settle its own
    ///        connection state and wake whoever waits on it. Without it a dead reactor is
    ///        indistinguishable from a slow one until the caller's timeout expires. Optional.
    void start(std::function<core::async::Task<void>(core::net::EventLoop*)> rootTask,
               std::function<void(std::string const&)> onFailure = {})
    {
        _thread =
            std::thread { [this, rootTask = std::move(rootTask), onFailure = std::move(onFailure)]() mutable {
                // EVERYTHING is caught here, because an exception leaving a std::thread's function is
                // unconditional std::terminate: no unwind, no handler, the whole Contour process gone —
                // every local, non-daemon tab in every window with it. And the reactor is where remote
                // input is decoded: PDU frames up to proto::MaxFrameSize, vectors and strings grown to
                // peer-named lengths, image data allocated from a daemon announcement. bad_alloc and
                // length_error are therefore reachable from the wire. The daemon side already isolates
                // each flow for precisely this reason (@see ConnectionAcceptor::superviseConnection);
                // one malformed peer must not take the GUI down either.
                try
                {
                    _loop.blockOn(rootTask(&_loop));
                }
                catch (core::async::OperationCancelled const&)
                {
                    // requestStop() unwound the root task — the intended shutdown.
                    _cancelled.store(true, std::memory_order_release);
                }
                catch (std::exception const& e)
                {
                    recordFailure(e.what(), onFailure);
                }
                catch (...)
                {
                    recordFailure("unknown exception", onFailure);
                }
                {
                    auto const lock = std::lock_guard { _exitMutex };
                    _exited = true;
                }
                _exitCondition.notify_all();
            } };
    }

    /// Waits until the root task has ended, for at most @p bound. Thread-safe.
    ///
    /// What lets a detach finish in order before anything is cancelled: a detach posted onto the
    /// loop ends the root task by itself, closing the connection properly on the way out, and a stop
    /// requested in the same breath would unwind that close half-done.
    /// @param bound How long to wait.
    /// @return Whether the root task ended within @p bound (true too if it had already ended, or
    ///         was never started).
    [[nodiscard]] bool waitForExit(std::chrono::milliseconds bound)
    {
        if (!_thread.joinable())
            return true; // never started (or already joined): there is nothing to wait for
        auto lock = std::unique_lock { _exitMutex };
        return _exitCondition.wait_for(lock, bound, [this] { return _exited; });
    }

    /// @return Whether the root task ended by cancellation (requestStop)
    ///         rather than running to completion.
    [[nodiscard]] bool wasCancelled() const noexcept { return _cancelled.load(std::memory_order_acquire); }

    /// @return Why the root task ended abnormally, or empty when it did not. Thread-safe.
    [[nodiscard]] std::string failure() const
    {
        auto const lock = std::lock_guard { _failureMutex };
        return _failure;
    }

    /// Marshals @p fn onto the loop thread. Thread-safe.
    /// @param fn The work to marshal onto the reactor thread.
    void post(std::function<void()> fn) { _loop.post(std::move(fn)); }

    /// Cancels every flow on the loop (marshaled onto it), so a task parked
    /// in connect/read unwinds and the thread becomes joinable.
    void requestStop()
    {
        _loop.post([this] { _loop.requestStop(); });
    }

    /// Waits for the root task (and thus the thread) to finish. Idempotent.
    void join()
    {
        if (_thread.joinable())
            _thread.join();
    }

  private:
    /// Records @p reason and hands it to @p onFailure. Runs on the reactor thread, from the
    /// catch arms above, so it must not throw its way out of them — the sink is invoked inside a
    /// nested try for that reason alone.
    /// @param reason What ended the root task.
    /// @param onFailure The owner's sink (may be empty).
    void recordFailure(char const* reason, std::function<void(std::string const&)> const& onFailure) noexcept
    {
        {
            auto const lock = std::lock_guard { _failureMutex };
            _failure = reason;
        }
        if (!onFailure)
            return;
        try
        {
            onFailure(reason);
        }
        catch (...)
        {
            // Nothing may leave this function — it runs from the thread-body catch arms, where an
            // escape IS std::terminate. Record the double fault rather than swallowing it.
            auto const lock = std::lock_guard { _failureMutex };
            _failure += " (the failure sink threw as well)";
        }
    }

    std::unique_ptr<core::net::IoBackend> _backend = core::net::makeDefaultBackend();
    core::net::EventLoop _loop { *_backend };
    std::thread _thread;
    std::atomic<bool> _cancelled { false };
    std::mutex _exitMutex;                  ///< Guards _exited.
    std::condition_variable _exitCondition; ///< Signalled once the root task has ended.
    bool _exited = false;                   ///< Whether the thread body has finished its root task.
    mutable std::mutex _failureMutex;       ///< Guards _failure (written on the reactor, read by the owner).
    std::string _failure;
};

} // namespace contour::remote
