// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where a daemon hears that its process is asked to end: SIGINT/SIGTERM on POSIX, a console-control event
/// on Windows. Behind an interface because those are process-wide: a daemon run inside a test process must
/// not take that process's signals, nor raise one into it.

#include <functional>
#include <memory>

namespace vthost
{

/// A source of "the process is asked to end" events.
class ShutdownSignals
{
  public:
    /// Watching for the event, for as long as it lives.
    class Watch
    {
      public:
        Watch() = default;
        Watch(Watch const&) = delete;
        Watch& operator=(Watch const&) = delete;
        Watch(Watch&&) = delete;
        Watch& operator=(Watch&&) = delete;
        /// Stops watching. Once it returns, @c onSignal is never called again.
        virtual ~Watch() = default;
    };

    ShutdownSignals() = default;
    ShutdownSignals(ShutdownSignals const&) = delete;
    ShutdownSignals& operator=(ShutdownSignals const&) = delete;
    ShutdownSignals(ShutdownSignals&&) = delete;
    ShutdownSignals& operator=(ShutdownSignals&&) = delete;
    virtual ~ShutdownSignals() = default;

    /// Starts watching.
    /// @param onSignal Called at most once, on a thread of the source's own, when the process is asked to
    ///        end. It must only post: it runs outside the daemon's loop.
    /// @return The watch, which watches until destroyed.
    [[nodiscard]] virtual std::unique_ptr<Watch> watch(std::function<void()> onSignal) = 0;
};

/// The process's own: SIGINT/SIGTERM taken by a sigwait thread (POSIX), or the console-control handler
/// (Windows). One daemon per process may watch them at a time.
/// @return The source.
[[nodiscard]] std::shared_ptr<ShutdownSignals> processShutdownSignals();

/// A source that never fires: for a daemon run inside a process whose signals are not its own (a test).
class NoShutdownSignals final: public ShutdownSignals
{
  public:
    [[nodiscard]] std::unique_ptr<Watch> watch(std::function<void()> onSignal) override;
};

} // namespace vthost
