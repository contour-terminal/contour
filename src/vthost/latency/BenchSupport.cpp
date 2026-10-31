// SPDX-License-Identifier: Apache-2.0

#include <vthost/latency/BenchSupport.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <algorithm>
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
    /// How often waitFor() asks whether the process is still there.
    constexpr auto WaitPoll = std::chrono::milliseconds { 2 };
    /// How long a daemon has to drop its sessions after a run.
    constexpr auto EmptyDaemonTimeout = std::chrono::seconds { 5 };
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

BenchProcess::BenchProcess(std::unique_ptr<vtpty::Process> process): _process { std::move(process) }
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
                                                    /*escapeSandbox=*/false,
                                                    std::make_shared<vtpty::NoPlacement>());
    if (auto const started = process->start(); !started)
        return std::unexpected(std::format("cannot start {}", program.string()));
    return std::unique_ptr<BenchProcess>(new BenchProcess(std::move(process)));
}

BenchProcess::~BenchProcess()
{
    kill();
}

bool BenchProcess::isRunning() const
{
    return _process->alive();
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
        case ProbePhase::AwaitingPrompt:
        case ProbePhase::Pausing:
        case ProbePhase::AwaitingEcho: break;
    }
    return std::unexpected("probe did not settle");
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
