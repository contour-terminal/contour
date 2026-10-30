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
#include <optional>
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
    ///        ends by an exception other than cancellation, or as soon as a posted callback throws
    ///        (@see post) — so the owner can settle its own
    ///        connection state and wake whoever waits on it. Without it a dead reactor is
    ///        indistinguishable from a slow one until the caller's timeout expires. Optional.
    void start(std::function<core::async::Task<void>(core::net::EventLoop*)> rootTask,
               std::function<void(std::string const&)> onFailure = {})
    {
        auto* const loop = currentLoop();
        _onFailure = std::move(onFailure);
        _thread = std::thread { [this, loop, rootTask = std::move(rootTask)]() mutable {
            // EVERYTHING is caught here, because an exception leaving a std::thread's function is
            // unconditional std::terminate: no unwind, no handler, the whole Contour process gone —
            // every local, non-daemon tab in every window with it. And the reactor is where remote
            // input is decoded: PDU frames up to proto::MaxFrameSize, vectors and strings grown to
            // peer-named lengths, image data allocated from a daemon announcement. bad_alloc and
            // length_error are therefore reachable from the wire. The daemon side already isolates
            // each flow for precisely this reason (@see ConnectionAcceptor::superviseConnection);
            // one malformed peer must not take the GUI down either.
            auto failure = std::optional<std::string> {};
            auto cancelled = false;
            try
            {
                loop->blockOn(rootTask(loop));
            }
            catch (core::async::OperationCancelled const&)
            {
                // requestStop() unwound the root task — the intended shutdown, unless a posted
                // callback's throw is what asked for it (below).
                cancelled = true;
            }
            catch (std::exception const& e)
            {
                failure = e.what();
            }
            catch (...)
            {
                failure = "unknown exception";
            }
            // A posted callback that threw stopped the loop and has already been reported: that throw
            // is why the run ended, not the cancellation it caused.
            if (_postFailure)
                _postFailure.reset();
            else if (failure)
                recordFailure(*failure);
            else if (cancelled)
                _cancelled.store(true, std::memory_order_release);
            {
                auto const lock = std::lock_guard { _exitMutex };
                _exited = true;
            }
            _exitCondition.notify_all();
        } };
    }

    /// Runs @p rootTask on a fresh loop after the last one ended: joins the finished thread, replaces the
    /// cancelled loop (a stopped loop cannot run again) and starts the thread again.
    ///
    /// Only for a root task that has ended or is about to (a connection that failed or was stopped): it joins
    /// first, so a task still running blocks the call. Posts that race with the replacement go to whichever
    /// loop is current; one that reached the old loop is dropped with it.
    /// @param rootTask The coroutine factory that produces the root task to run on the new loop.
    /// @param onFailure As for start().
    void restart(std::function<core::async::Task<void>(core::net::EventLoop*)> rootTask,
                 std::function<void(std::string const&)> onFailure = {})
    {
        join();
        {
            auto const lock = std::lock_guard { _loopMutex };
            _loop.reset(); // before its backend
            _backend = core::net::makeDefaultBackend();
            _loop = std::make_unique<core::net::EventLoop>(*_backend);
        }
        {
            auto const lock = std::lock_guard { _exitMutex };
            _exited = false;
        }
        _cancelled.store(false, std::memory_order_release);
        _postFailure.reset(); // the joined thread was its last user
        {
            auto const lock = std::lock_guard { _failureMutex };
            _failure.clear();
        }
        start(std::move(rootTask), std::move(onFailure));
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
    ///
    /// A throw out of @p fn ends the run as a failure of the root task does -- recorded, and handed to
    /// start()'s onFailure, there and then -- but in order: it is caught inside the turn and the loop is
    /// stopped, so every flow unwinds by cancellation. Let out of the turn instead, it ended blockOn with the
    /// root task still suspended, and destroying that root freed frames the same turn had queued to resume;
    /// the loop's teardown then resumed them, a use-after-free and a double free.
    /// @param fn The work to marshal onto the reactor thread.
    void post(std::function<void()> fn)
    {
        auto const lock = std::lock_guard { _loopMutex };
        auto* const loop = _loop.get();
        _loop->post([this, loop, fn = std::move(fn)] {
            try
            {
                fn();
            }
            catch (std::exception const& e)
            {
                failFromPost(*loop, e.what());
            }
            catch (...)
            {
                failFromPost(*loop, "unknown exception");
            }
        });
    }

    /// Cancels every flow on the loop (marshaled onto it), so a task parked
    /// in connect/read unwinds and the thread becomes joinable.
    void requestStop()
    {
        auto const lock = std::lock_guard { _loopMutex };
        auto* const loop = _loop.get();
        loop->post([loop] { loop->requestStop(); });
    }

    /// Waits for the root task (and thus the thread) to finish. Idempotent.
    void join()
    {
        if (_thread.joinable())
            _thread.join();
    }

  private:
    /// Reports the first posted callback's failure and stops @p loop, so the root task unwinds by
    /// cancellation. Reported before the unwind, so the owner hears of the failure ahead of whatever the
    /// cancelled flows report on their way out. Reactor thread only, inside a turn.
    /// @param loop The loop the throwing callback ran on.
    /// @param reason What the callback threw.
    void failFromPost(core::net::EventLoop& loop, char const* reason)
    {
        if (!_postFailure)
        {
            _postFailure = reason;
            recordFailure(*_postFailure);
        }
        loop.requestStop();
    }

    /// Records @p reason and hands it to start()'s onFailure. Runs on the reactor thread -- in the thread
    /// body after its catch arms, or in a turn on behalf of a posted callback -- where nothing may escape;
    /// the sink is invoked inside a nested try for that reason alone.
    /// @param reason What ended the root task.
    void recordFailure(std::string const& reason) noexcept
    {
        {
            auto const lock = std::lock_guard { _failureMutex };
            _failure = reason;
        }
        if (!_onFailure)
            return;
        try
        {
            _onFailure(reason);
        }
        catch (...)
        {
            // Nothing may leave this function — it runs in the thread body, where an escape IS
            // std::terminate. Record the double fault rather than swallowing it.
            auto const lock = std::lock_guard { _failureMutex };
            _failure += " (the failure sink threw as well)";
        }
    }

    /// @return The loop posts go to now. The pointer is stable until restart(), which only runs once the
    ///         thread of the previous one has been joined.
    [[nodiscard]] core::net::EventLoop* currentLoop()
    {
        auto const lock = std::lock_guard { _loopMutex };
        return _loop.get();
    }

    mutable std::mutex _loopMutex; ///< Guards the _loop pointer against restart().
    std::unique_ptr<core::net::IoBackend> _backend = core::net::makeDefaultBackend();
    std::unique_ptr<core::net::EventLoop> _loop = std::make_unique<core::net::EventLoop>(*_backend);
    std::thread _thread;
    std::atomic<bool> _cancelled { false };
    std::mutex _exitMutex;                  ///< Guards _exited.
    std::condition_variable _exitCondition; ///< Signalled once the root task has ended.
    bool _exited = false;                   ///< Whether the thread body has finished its root task.
    mutable std::mutex _failureMutex;       ///< Guards _failure (written on the reactor, read by the owner).
    std::string _failure;
    /// start()'s onFailure. Written before the thread starts, read only by it.
    std::function<void(std::string const&)> _onFailure;
    /// What a posted callback threw, the first time one did. Reactor thread only; restart() resets it once
    /// that thread has been joined.
    std::optional<std::string> _postFailure;
};

} // namespace contour::remote
