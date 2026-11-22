// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/BenchSupport.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <algorithm>
#include <cerrno>
#include <format>
#include <fstream>
#include <memory>
#include <ostream>
#include <random>
#include <system_error>
#include <thread>
#include <utility>

#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/latency/Report.hpp>
#include <vthost/latency/Stats.hpp>

namespace vthost::latency
{

namespace
{
    /// The size of the pseudo terminal a benchmark child runs on; nothing reads what it shows.
    constexpr auto ChildPageSize =
        vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) };
    /// How much of a child's terminal output one read takes.
    constexpr auto DrainBufferSize = std::size_t { 4096 };
    /// How long a read of a child's terminal waits before it asks whether it is to stop.
    constexpr auto DrainPoll = std::chrono::milliseconds { 50 };
    /// How often waitFor() asks whether the process is still there.
    constexpr auto WaitPoll = std::chrono::milliseconds { 2 };
    /// How long a daemon has to drop its sessions after a run.
    constexpr auto EmptyDaemonTimeout = std::chrono::seconds { 5 };

    /// How much of a child's newest output is kept for a failure message.
    constexpr auto TailCapacity = std::size_t { 2048 };

    /// Reads what @p child prints on its terminal, keeping the newest @p TailCapacity bytes in @p tail,
    /// until @p stop is requested or the terminal ends.
    void drainTerminal(vtpty::Process& child,
                       std::stop_token const& stop,
                       std::mutex& tailMutex,
                       std::string& tail)
    {
        auto pool = crispy::BufferObjectPool<char> { DrainBufferSize };
        while (!stop.stop_requested())
        {
            auto const storage = pool.allocateBufferObject();
            // Clear what this thread left behind: a stale EAGAIN must not pass for the poll running out.
            errno = 0;
            auto const read = child.read(*storage, DrainPoll, DrainBufferSize);
            // Nothing within the poll is no end; an empty read is the end of the output, and any other
            // failure a closed terminal.
            if ((!read && errno != EAGAIN && errno != EINTR) || (read && read->data.empty()))
                return;
            if (read)
            {
                auto const lock = std::scoped_lock { tailMutex };
                tail.append(read->data);
                if (tail.size() > TailCapacity)
                    tail.erase(0, tail.size() - TailCapacity);
            }
        }
    }
} // namespace

std::expected<ScratchDirectory, std::string> ScratchDirectory::create()
{
    auto const path = std::filesystem::temp_directory_path()
                      / std::format("contour-bench-latency-{}", std::random_device {}());
    auto error = std::error_code {};
    if (!std::filesystem::create_directory(path, error))
        return std::unexpected(std::format("cannot create {}", path.string()));
    std::filesystem::permissions(path, std::filesystem::perms::owner_all, error);
    return ScratchDirectory { path };
}

ScratchDirectory::ScratchDirectory(ScratchDirectory&& other) noexcept:
    _path { std::exchange(other._path, {}) }
{
}

ScratchDirectory::~ScratchDirectory()
{
    if (_path.empty())
        return;
    auto error = std::error_code {};
    std::filesystem::remove_all(_path, error);
}

BenchProcess::BenchProcess(std::unique_ptr<vtpty::Process> process):
    _process { std::move(process) },
    _drain { [this](std::stop_token const& stop) { drainTerminal(*_process, stop, _tailMutex, _tail); } }
{
}

vtpty::Process::ExecInfo benchExecInfo(std::filesystem::path const& program,
                                       std::vector<std::string> arguments)
{
    return vtpty::Process::ExecInfo {
#ifdef _WIN32
        // Quoted: the command line takes the program as it is given, and a path with a space in it would
        // name another program.
        .program = "\"" + program.string() + "\"",
#else
        .program = program.string(),
#endif
        .arguments = std::move(arguments),
        .workingDirectory = std::filesystem::temp_directory_path(),
        .env = {},
        .removedEnvironment = { "LOG" },
    };
}

std::expected<std::unique_ptr<BenchProcess>, std::string> BenchProcess::start(
    std::filesystem::path const& program, std::vector<std::string> arguments)
{
    auto process = std::make_unique<vtpty::Process>(benchExecInfo(program, std::move(arguments)),
                                                    vtpty::createPty(ChildPageSize, std::nullopt),
                                                    vtpty::SandboxEscape::Contained,
                                                    std::make_shared<vtpty::NoPlacement>());
    if (auto const started = process->start(); !started)
        return std::unexpected(std::format("cannot start {}", program.string()));
    return std::unique_ptr<BenchProcess>(new BenchProcess(std::move(process)));
}

BenchProcess::~BenchProcess()
{
    kill();
    // The thread reads the process: it ends before the process goes.
    _drain.request_stop();
    if (_drain.joinable())
        _drain.join();
}

bool BenchProcess::isRunning() const
{
    return _process->alive();
}

std::optional<std::int64_t> BenchProcess::processId() const
{
    if (auto const child = _process->handoverChild())
        return child->pid;
    return std::nullopt;
}

std::optional<vtpty::Process::ExitStatus> BenchProcess::waitFor(std::chrono::milliseconds timeout)
{
    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (_process->alive() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(WaitPoll);
    return _process->checkStatus();
}

void BenchProcess::kill()
{
    if (_process->alive())
        _process->terminate(vtpty::Process::TerminationHint::Kill);
    static_cast<void>(_process->wait());
}

std::string BenchProcess::outputTail() const
{
    auto const lock = std::scoped_lock { _tailMutex };
    return _tail;
}

std::string describeExit(std::optional<vtpty::Process::ExitStatus> const& status)
{
    if (!status)
        return "it did not end";
    if (auto const* normal = std::get_if<vtpty::NormalExit>(&*status))
        return std::format("it exited with status {}", normal->exitCode);
    return std::format("it was killed by signal {}", std::get<vtpty::SignalExit>(*status).signum);
}

std::string describeChildFailure(std::string_view reason,
                                 std::optional<vtpty::Process::ExitStatus> const& status,
                                 std::string_view outputTail)
{
    auto message = std::format("{} ({})", reason, describeExit(status));
    if (!outputTail.empty())
        message += std::format("; its output ended with:\n{}", outputTail);
    return message;
}

std::expected<void, std::string> judgeProbe(ProbeResult const& result)
{
    switch (result.phase)
    {
        case ProbePhase::Finished: return {};
        case ProbePhase::PromptTimedOut:
            return std::unexpected(
                std::format("probe timed out waiting for cursor column {} (the prompt)", PromptColumns));
        case ProbePhase::EchoTimedOut:
            return std::unexpected(
                std::format("probe timed out waiting for cursor column {}", result.awaitedColumn));
        case ProbePhase::FocusTimedOut:
            return std::unexpected(
                std::format("probe timed out waiting for the keyboard focus to type towards cursor column {}",
                            result.awaitedColumn));
        case ProbePhase::AwaitingPrompt:
        case ProbePhase::Pausing:
        case ProbePhase::AwaitingEcho: break;
    }
    // A probe left in a waiting phase ended without its own verdict: the GUI quit while it waited.
    return std::unexpected(std::format("probe did not settle: it stopped while {}, at cursor column {}",
                                       probePhaseName(result.phase),
                                       result.awaitedColumn));
}

bool awaitEmptyDaemon(std::filesystem::path const& socketPath)
{
    auto const control =
        client::makeDaemonControl(std::make_shared<UnixTransportFactory>(socketPath, nullptr));
    auto const deadline = std::chrono::steady_clock::now() + EmptyDaemonTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (auto const status = control->status(); status && status->sessions == 0)
            return true;
        std::this_thread::sleep_for(WaitPoll);
    }
    return false;
}

double medianOf(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    std::ranges::sort(values);
    return values[values.size() / 2];
}

std::expected<void, std::string> publish(BenchReport const& report,
                                         std::optional<std::filesystem::path> const& record,
                                         std::ostream& out)
{
    auto recordText = std::string {};
    out << formatTableHeader();
    for (auto const& series: report.series)
    {
        auto const summary = summarize(series.samples);
        out << formatTableRow(series.metric, series.mode, summary);
        recordText += formatRecord(series.metric, series.mode, summary);
    }
    for (auto const& scalar: report.scalars)
    {
        auto const median = medianOf(scalar.values);
        out << std::format(
            "{:<15} {:<12} {:>5} {:>10.1f}\n", scalar.metric, scalar.mode, scalar.values.size(), median);
        recordText += formatScalarRecord(scalar.metric, scalar.mode, scalar.values.size(), median);
    }
    if (report.gate)
    {
        auto line = formatGateLine(Budgets {}, *report.gate);
        if (!report.gateNote.empty())
            line += " " + report.gateNote;
        out << line << '\n';
        recordText += line + '\n';
    }
    out.flush();
    if (!record)
        return {};
    auto file = std::ofstream(*record, std::ios::binary | std::ios::trunc);
    if (!(file << recordText))
        return std::unexpected(std::format("cannot write {}", record->string()));
    return {};
}

} // namespace vthost::latency
