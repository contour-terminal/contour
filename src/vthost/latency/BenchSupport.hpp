// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the benchmark legs share: child processes, and the table, gate line and record they publish.

#include <vtpty/Process.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/latency/Gate.hpp>
#include <vthost/latency/KeystrokeProbe.hpp>

namespace vthost::latency
{

/// @param program The executable.
/// @param arguments Its arguments.
/// @return How a benchmark child is started: with the benchmark's own environment less `LOG`, whose output
///         would otherwise land on the terminal the child's own output is read from.
[[nodiscard]] vtpty::Process::ExecInfo benchExecInfo(std::filesystem::path const& program,
                                                     std::vector<std::string> arguments);

/// A directory for a benchmark run, owner-only, removed again when this goes.
class ScratchDirectory
{
  public:
    /// Creates the directory under the system's temporary directory.
    /// @return The directory, or why it could not be created.
    [[nodiscard]] static std::expected<ScratchDirectory, std::string> create();

    /// Removes the directory with everything in it.
    ~ScratchDirectory();

    ScratchDirectory(ScratchDirectory&& other) noexcept;
    ScratchDirectory(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory const&) = delete;
    ScratchDirectory& operator=(ScratchDirectory&&) = delete;

    /// @return The directory's path.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    explicit ScratchDirectory(std::filesystem::path path): _path { std::move(path) } {}

    std::filesystem::path _path;
};

/// A child process of the benchmark, ended when this goes.
class BenchProcess
{
  public:
    /// Starts @p program with @p arguments on a pseudo terminal of its own, with the benchmark's own
    /// environment less `LOG`, whose output would otherwise land on that terminal.
    /// @param program The executable.
    /// @param arguments Its arguments.
    /// @return The running process, or why it did not start.
    [[nodiscard]] static std::expected<std::unique_ptr<BenchProcess>, std::string> start(
        std::filesystem::path const& program, std::vector<std::string> arguments);

    /// Kills the process if it still runs.
    ~BenchProcess();

    BenchProcess(BenchProcess const&) = delete;
    BenchProcess& operator=(BenchProcess const&) = delete;
    BenchProcess(BenchProcess&&) = delete;
    BenchProcess& operator=(BenchProcess&&) = delete;

    /// @return Whether the process still runs.
    [[nodiscard]] bool isRunning() const;

    /// Waits for the process to end.
    /// @param timeout How long to wait before giving up.
    /// @return How it ended; nullopt if it still ran after @p timeout.
    [[nodiscard]] std::optional<vtpty::Process::ExitStatus> waitFor(std::chrono::milliseconds timeout);

    /// Ends the process outright and waits for it.
    void kill();

  private:
    explicit BenchProcess(std::unique_ptr<vtpty::Process> process);

    std::unique_ptr<vtpty::Process> _process;
};

/// Whether a probe has said all it will.
/// @param phase The probe's phase.
/// @return True for Finished and both timeouts.
[[nodiscard]] constexpr bool isSettled(ProbePhase phase) noexcept
{
    return phase == ProbePhase::Finished || phase == ProbePhase::PromptTimedOut
           || phase == ProbePhase::EchoTimedOut;
}

/// @param result A settled probe.
/// @return Nothing for a finished one; otherwise a sentence naming the column it waited for in vain.
[[nodiscard]] std::expected<void, std::string> judgeProbe(ProbeResult const& result);

/// One measurement series, named for the table and the record.
struct MetricSeries
{
    std::string metric; ///< What was measured, such as `startup`.
    std::string mode;   ///< Which mode measured it, such as `daemon`.
    std::vector<core::platform::SteadyDuration> samples;
};

/// One figure per run, named for the record.
struct ScalarSeries
{
    std::string metric; ///< What was measured, such as `flood-mibps`.
    std::string mode;   ///< Which mode measured it.
    std::vector<double> values;
};

/// What a benchmark run publishes.
struct BenchReport
{
    std::vector<MetricSeries> series; ///< The tables' rows, in order.
    std::vector<ScalarSeries> scalars;
    /// The verdicts; the gate line is printed (and recorded) only for a report that has some.
    std::optional<GateResult> gate;
    /// Printed after the gate line, such as `(informational)`.
    std::string gateNote;
};

/// Prints the table (and the scalars, and the gate line) and, if asked, writes the record.
/// @param report What was measured.
/// @param record Where to write the TSV record; nullopt for none.
/// @param out Where the table goes.
/// @return Nothing, or why the record could not be written.
[[nodiscard]] std::expected<void, std::string> publish(BenchReport const& report,
                                                       std::optional<std::filesystem::path> const& record,
                                                       std::ostream& out);

/// Waits until the daemon at @p socketPath hosts no session, so the next launch attaches to an empty one.
/// @param socketPath The daemon's control socket.
/// @return Whether it was empty within a few seconds.
[[nodiscard]] bool awaitEmptyDaemon(std::filesystem::path const& socketPath);

/// @param values Figures, in any order.
/// @return Their median; zero for none.
[[nodiscard]] double medianOf(std::vector<double> values);

} // namespace vthost::latency
