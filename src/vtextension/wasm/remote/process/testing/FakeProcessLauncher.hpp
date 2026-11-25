// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A launcher that starts nothing: it records what it was asked to start and hands back processes a test ends
/// by hand. Its wait sets ignore real time -- they return only once a process ended or they were woken -- so
/// that a supervisor driven by a ManualClock is deterministic.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process::testing
{

/// The status a fake process's kill() ends it with: as SIGKILL (9) ends a POSIX process.
inline constexpr auto FakeKilledStatus = ExitStatus { .kind = ExitKind::Signalled, .value = 9 };

/// The id the first fake process gets; each later one the next.
inline constexpr ProcessId FirstFakeProcessId = 1000;

/// Records launches and hands back processes that end when the test says. Thread-safe: every record is
/// guarded by one mutex.
class FakeProcessLauncher final: public IProcessLauncher
{
  public:
    /// What the launcher and every process and set of it share.
    struct State;

    FakeProcessLauncher();
    FakeProcessLauncher(FakeProcessLauncher const&) = delete;
    FakeProcessLauncher(FakeProcessLauncher&&) = delete;
    FakeProcessLauncher& operator=(FakeProcessLauncher const&) = delete;
    FakeProcessLauncher& operator=(FakeProcessLauncher&&) = delete;
    ~FakeProcessLauncher() override;

    /// Records @p spec and the calling thread.
    /// @return A process with the next id, running until exit() or its kill().
    [[nodiscard]] std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
        LaunchSpec const& spec) override;

    /// @return A set that returns only when a watched process ended, or it was woken; it records each
    ///         wait's deadline, and otherwise ignores it.
    [[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
        core::IClock& clock) override;

    /// Ends the @p index-th process launched with @p status, and wakes every set that watches it.
    /// @param index The process, counted from 0 in launch order.
    /// @param status How it ended.
    void exit(std::size_t index, ExitStatus status);

    /// Makes the @p index-th process ignore kill(): it is recorded, and the process runs on until exit(). A
    /// process stuck in the kernel behaves so.
    /// @param index The process, counted from 0 in launch order; it may be launched later.
    void survivesKill(std::size_t index);

    /// Makes the @p index-th process's tryReap() fail with @p error from now on: a process that cannot be
    /// waited for.
    /// @param index The process, counted from 0 in launch order.
    /// @param error What tryReap() fails with.
    void failReap(std::size_t index, LaunchError error);

    /// Makes every watch() of every set fail with @p error from now on.
    void failWatches(LaunchError error);

    /// Makes every wait() of every set fail with @p error from now on, a wait blocked now included.
    void failWaits(LaunchError error);

    /// @return The index of the process each kill() was called on, in call order -- also those that did
    ///         nothing, on a process that had ended already.
    [[nodiscard]] std::vector<std::size_t> kills() const;

    /// @return Every spec launched, in order.
    [[nodiscard]] std::vector<LaunchSpec> launches() const;

    /// @return The thread each launch ran on, in order.
    [[nodiscard]] std::vector<std::thread::id> launchThreads() const;

    /// @return The deadline the latest wait() of any set was given; nothing if it had none, or none waited.
    [[nodiscard]] std::optional<core::SteadyTimePoint> lastDeadline() const;

  private:
    std::shared_ptr<State> _state;
};

} // namespace vtextension::wasm::remote::process::testing
