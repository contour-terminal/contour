// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/Readiness.hpp>

#include <vtbackend/state/StateCodec.hpp>

#include <vtpty/CommandCapture.hpp>
#ifdef _WIN32
    #include <vtpty/WindowsCommandLine.hpp>
#endif

#include <core/Utf8Path.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/ResumeOn.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#ifdef _WIN32
    #include <vector>
#endif

#include <vtcontrol/Json.hpp>
#include <vthost/Logging.hpp>
#include <vthost/handover/HandoverImage.hpp>

namespace vthost::handover
{

namespace
{
    /// Read leniently about what a later release adds, strictly about what this one needs.
    constexpr auto ReadOptions = glz::opts { .error_on_unknown_keys = false, .error_on_missing_keys = true };

    /// @return Whether @p range names a non-empty range.
    [[nodiscard]] constexpr bool isRange(std::array<uint32_t, 2> const& range) noexcept
    {
        return range[0] <= range[1];
    }

    /// @return Whether @p range holds @p version.
    [[nodiscard]] constexpr bool reads(std::pair<uint32_t, uint32_t> const& range, uint32_t version) noexcept
    {
        return range.first <= version && version <= range.second;
    }

    /// @return @p adoption as the wire spells it (detail::ReadinessJson::adoptsPseudoconsoles).
    [[nodiscard]] constexpr bool adoptsOf(PseudoconsoleAdoption adoption) noexcept
    {
        return adoption == PseudoconsoleAdoption::Able;
    }

    /// @return What the wire's @p adopts says (detail::ReadinessJson::adoptsPseudoconsoles).
    [[nodiscard]] constexpr PseudoconsoleAdoption adoptionOf(bool adopts) noexcept
    {
        return adopts ? PseudoconsoleAdoption::Able : PseudoconsoleAdoption::Unable;
    }

#ifndef _WIN32
    /// @return @p text as one word of the POSIX shell, whatever it holds.
    [[nodiscard]] std::string shellQuoted(std::string_view text)
    {
        auto quoted = std::string { "'" };
        for (auto const c: text)
        {
            if (c == '\'')
                quoted += R"('\'')";
            else
                quoted += c;
        }
        quoted += '\'';
        return quoted;
    }
#endif
} // namespace

ReadinessReport ownReadiness(std::string release, std::optional<PseudoconsoleAdoption> pseudoconsoles)
{
    return ReadinessReport {
        .release = std::move(release),
        .handoverFormats = { OldestReadableHandoverFormat, HandoverFormatVersion },
        .terminalStateFormats = { vtbackend::OldestReadableTerminalStateFormat,
                                  vtbackend::TerminalStateFormatVersion },
        .configProblem = std::nullopt,
        .pseudoconsoleAdoption = pseudoconsoles,
    };
}

std::string encodeReadiness(ReadinessReport const& report)
{
    return vtcontrol::encodeJson(detail::ReadinessJson {
        .release = report.release,
        .handoverFormats = { report.handoverFormats.first, report.handoverFormats.second },
        .terminalStateFormats = { report.terminalStateFormats.first, report.terminalStateFormats.second },
        .configProblem = report.configProblem,
        .adoptsPseudoconsoles = report.pseudoconsoleAdoption.transform(adoptsOf),
    });
}

std::optional<ReadinessReport> decodeReadiness(std::string_view text)
{
    auto wire = detail::ReadinessJson {};
    if (text.empty() || glz::read<ReadOptions>(wire, text) || !isRange(wire.handoverFormats)
        || !isRange(wire.terminalStateFormats))
        return std::nullopt;
    return ReadinessReport {
        .release = std::move(wire.release),
        .handoverFormats = { wire.handoverFormats[0], wire.handoverFormats[1] },
        .terminalStateFormats = { wire.terminalStateFormats[0], wire.terminalStateFormats[1] },
        .configProblem = std::move(wire.configProblem),
        .pseudoconsoleAdoption = wire.adoptsPseudoconsoles.transform(adoptionOf),
    };
}

std::expected<void, UpgradeRefusal> checkReadable(ReadinessReport const& target)
{
    if (!reads(target.handoverFormats, HandoverFormatVersion)
        || !reads(target.terminalStateFormats, vtbackend::TerminalStateFormatVersion))
        return std::unexpected(UpgradeRefusal::FormatUnreadable);
    return {};
}

std::expected<void, UpgradeRefusal> checkAdoptsPseudoconsoles(ReadinessReport const& target)
{
    if (target.pseudoconsoleAdoption == PseudoconsoleAdoption::Unable)
        return std::unexpected(UpgradeRefusal::TargetCannotAdoptTerminals);
    return {};
}

std::expected<void, UpgradeRefusal> checkConfigured(ReadinessReport const& target)
{
    if (target.configProblem)
        return std::unexpected(UpgradeRefusal::ConfigUnreadable);
    return {};
}

std::string readinessCommand(ReadinessQuery const& query)
{
#ifdef _WIN32
    // The target's own command line, run with no shell between (vtpty::ProgramCommandCapture): a release
    // build is a GUI-subsystem program that attaches to its parent's console, which under cmd.exe takes its
    // standard output from the capture. In the daemon's own environment, unchanged, which the successor
    // inherits too.
    auto arguments = std::vector<std::string> { "daemon", "handover-info" };
    if (!query.configFile.empty())
        arguments.insert(arguments.end(), { "--config", core::pathToUtf8(query.configFile) });
    if (!query.profile.empty())
        arguments.insert(arguments.end(), { "--profile", query.profile });
    return vtpty::buildWindowsCommandLine(core::pathToUtf8(query.executable), arguments);
#else
    // `exec`, so that the timeout's kill reaches the target itself. In the daemon's own environment,
    // unchanged: the exec'd image inherits it too, so the probe must see the world the target will run in --
    // an AppImage's or Nix's paths, $LD_LIBRARY_PATH, $HOME and $XDG_*, which decide whether it starts and
    // what it loads.
    auto command =
        std::format("exec {} daemon handover-info", shellQuoted(core::pathToUtf8(query.executable)));
    if (!query.configFile.empty())
        command += std::format(" --config {}", shellQuoted(core::pathToUtf8(query.configFile)));
    if (!query.profile.empty())
        command += std::format(" --profile {}", shellQuoted(query.profile));
    return command;
#endif
}

core::async::Task<std::expected<ReadinessReport, UpgradeRefusal>> CommandReadinessProbe::probe(
    ReadinessQuery query)
{
    // Awaited where nothing runs tasks: there is no loop to keep serving, nor one to come back to.
    auto* const home = core::async::currentExecutor();
    if (home == nullptr)
        co_return ask(query);

    co_await core::async::ResumeOn { _worker };
    auto answer = std::expected<ReadinessReport, UpgradeRefusal> { std::unexpected(
        UpgradeRefusal::TargetUnresponsive) };
    // Nothing may unwind on the worker: the awaiting flow would go on there, off its loop.
    try
    {
        answer = ask(query);
    }
    catch (std::exception const& failure)
    {
        errorLog()("live upgrade: the readiness probe failed: {}", failure.what());
    }
    co_await core::async::ResumeOn { *home };
    co_return answer;
}

std::expected<ReadinessReport, UpgradeRefusal> CommandReadinessProbe::ask(ReadinessQuery const& query) const
{
    auto const answer = _capture.run(readinessCommand(query), _timeout, core::async::StopToken {});
    if (!answer)
        return std::unexpected(UpgradeRefusal::TargetUnresponsive);
    auto report = decodeReadiness(*answer);
    if (!report)
        return std::unexpected(UpgradeRefusal::FormatUnreadable);
    return *std::move(report);
}

} // namespace vthost::handover
