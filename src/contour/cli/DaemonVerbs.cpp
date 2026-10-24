// SPDX-License-Identifier: Apache-2.0

#include <contour/cli/DaemonVerbs.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iostream>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <vthost/DaemonLifecycle.hpp>

#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

using vthost::client::ControlError;
using vthost::client::ControlErrorCode;
using vthost::client::DaemonControlPort;
using vthost::client::DaemonStatus;

namespace contour::cli
{

namespace
{
    /// One unit of a duration: its suffix and how many seconds it spans.
    struct UptimeUnit
    {
        char suffix;
        std::int64_t seconds;
    };

    constexpr auto UptimeUnits = std::array {
        UptimeUnit { .suffix = 'd', .seconds = 86400 },
        UptimeUnit { .suffix = 'h', .seconds = 3600 },
        UptimeUnit { .suffix = 'm', .seconds = 60 },
        UptimeUnit { .suffix = 's', .seconds = 1 },
    };

    /// The most a tail reads from the end of a log file.
    constexpr auto MaxTailBytes = std::uintmax_t { 262144 }; // 256 KiB

    /// A prompt over a text stream pair, available only when standard input is a terminal.
    class TerminalPrompt final: public ConfirmPrompt
    {
      public:
        TerminalPrompt(std::istream& in, std::ostream& out): _in { in }, _out { out } {}

        [[nodiscard]] Confirmation ask(std::string_view question) override
        {
#ifdef _WIN32
            auto const interactive = _isatty(_fileno(stdin)) != 0;
#else
            auto const interactive = isatty(STDIN_FILENO) != 0;
#endif
            if (!interactive)
                return Confirmation::Unavailable;
            _out << question << std::flush;
            auto answer = std::string {};
            if (!std::getline(_in, answer))
                return Confirmation::Declined;
            auto const lower = answer | std::views::transform([](char c) {
                                   return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
                               })
                               | std::ranges::to<std::string>();
            return lower == "y" || lower == "yes" ? Confirmation::Confirmed : Confirmation::Declined;
        }

      private:
        std::istream& _in;
        std::ostream& _out;
    };

    /// @return What is said of a daemon that answered, but not to daemon control. The detail names its
    ///         release ("release 0.7.0", or "an earlier release" when its hello states none), so it is
    ///         set off as an aside, as ControlError::toString() does, not run into the sentence.
    [[nodiscard]] std::string unsupportedText(ControlError const& error)
    {
        return std::format(
            "the daemon ({}) does not support `contour daemon stop`; stop it with Ctrl+C where "
            "it runs, or end its process",
            error.detail);
    }

    /// Reports a failed control request that is not "no daemon".
    void report(ControlError const& error, std::ostream& err)
    {
        err << std::format("contour daemon: {}\n", error.toString());
    }
} // namespace

std::unique_ptr<ConfirmPrompt> makeTerminalPrompt(std::istream& in, std::ostream& out)
{
    return std::make_unique<TerminalPrompt>(in, out);
}

std::string formatUptime(std::chrono::seconds uptime)
{
    auto remaining = std::max<std::int64_t>(uptime.count(), 0);
    auto parts = std::vector<std::string> {};
    auto started = false;
    for (auto const& unit: UptimeUnits)
    {
        auto const count = remaining / unit.seconds;
        remaining %= unit.seconds;
        if (!started && count == 0 && unit.suffix != 's')
            continue;
        if (parts.size() == 3)
            break;
        parts.push_back(started ? std::format("{:02}{}", count, unit.suffix)
                                : std::format("{}{}", count, unit.suffix));
        started = true;
    }
    return parts | std::views::join_with(' ') | std::ranges::to<std::string>();
}

std::string formatDaemonStatus(DaemonStatus const& status)
{
    struct Row
    {
        std::string_view label;
        std::string value;
    };
    auto const rows = std::array {
        Row { "daemon", std::format("running (pid {})", status.processId) },
        Row { "version", status.releaseVersion },
        Row { "uptime", formatUptime(status.uptime) },
        Row { "socket", core::pathToUtf8(status.socketPath) },
        Row { "executable", core::pathToUtf8(status.executablePath) },
        Row { "log",
              status.logFile.empty() ? std::string { "standard error" } : core::pathToUtf8(status.logFile) },
        Row { "sessions", std::to_string(status.sessions) },
        Row { "clients", std::to_string(status.clients) },
        Row { "lifecycle", std::string { vthost::nameOf(status.lifecycle) } },
    };
    auto const width =
        std::ranges::max(rows | std::views::transform([](Row const& r) { return r.label.size(); }));
    auto text = std::string {};
    for (auto const& row: rows)
        text += std::format("{:<{}}  {}\n", std::string { row.label } + ':', width + 1, row.value);
    return text;
}

LogFollower::LogFollower(core::platform::FileSystem& fs, std::filesystem::path file):
    _fs { fs }, _file { std::move(file) }
{
}

std::string LogFollower::tail(std::size_t lines)
{
    auto const size = _fs.fileSize(_file);
    if (!size)
        return {};
    _offset = *size;
    auto const readFrom = *size > MaxTailBytes ? *size - MaxTailBytes : std::uintmax_t { 0 };
    auto stream = _fs.openRead(_file);
    if (!stream)
        return {};
    (*stream)->seekg(static_cast<std::streamoff>(readFrom));
    auto text = std::string { std::istreambuf_iterator<char> { **stream }, {} };
    if (lines == 0)
        return text;

    // Walk back from the end over `lines` newline-terminated lines; `lineEnd` is where the line being
    // walked over ends, `start` where the kept text begins.
    auto lineEnd = text.ends_with('\n') ? text.size() - 1 : text.size();
    auto start = text.size();
    for ([[maybe_unused]] auto const _: std::views::iota(std::size_t { 0 }, lines))
    {
        auto const newline = lineEnd == 0 ? std::string::npos : text.rfind('\n', lineEnd - 1);
        if (newline == std::string::npos)
        {
            start = 0;
            break;
        }
        start = newline + 1;
        lineEnd = newline;
    }
    return text.substr(start);
}

std::string LogFollower::poll()
{
    auto const size = _fs.fileSize(_file);
    if (!size)
        return {};
    if (*size < _offset)
        _offset = 0;
    if (*size == _offset)
        return {};
    auto stream = _fs.openRead(_file);
    if (!stream)
        return {};
    (*stream)->seekg(static_cast<std::streamoff>(_offset));
    auto text = std::string { std::istreambuf_iterator<char> { **stream }, {} };
    _offset += text.size();
    return text;
}

ExitStatus daemonStatusVerb(DaemonControlPort& control,
                            std::filesystem::path const& socket,
                            std::ostream& out,
                            std::ostream& err)
{
    auto const status = control.status();
    if (status)
    {
        out << formatDaemonStatus(*status);
        return ExitStatus::Ok;
    }
    if (status.error().code == ControlErrorCode::NotRunning)
    {
        out << std::format("no daemon is running on {}\n", core::pathToUtf8(socket));
        return ExitStatus::Unreachable;
    }
    report(status.error(), err);
    return ExitStatus::Failed;
}

std::expected<std::string, std::string> mergeSharedOption(std::string_view name,
                                                          std::string const& parent,
                                                          std::string const& child,
                                                          std::string const& unset)
{
    if (parent == unset)
        return child;
    if (child == unset || child == parent)
        return parent;
    return std::unexpected(
        std::format("--{} is given twice with different values ('{}' and '{}')", name, parent, child));
}

ExitStatus daemonStopVerb(DaemonControlPort& control,
                          std::filesystem::path const& socket,
                          StopConsent consent,
                          ConfirmPrompt& prompt,
                          std::ostream& out,
                          std::ostream& err)
{
    auto const fail = [&](ControlError const& error) {
        switch (error.code)
        {
            case ControlErrorCode::NotRunning:
                err << std::format("no daemon is running on {}\n", core::pathToUtf8(socket));
                return ExitStatus::Unreachable;
            case ControlErrorCode::Unsupported:
                err << unsupportedText(error) << '\n';
                return ExitStatus::Failed;
            case ControlErrorCode::Refused:
            case ControlErrorCode::Timeout:
            case ControlErrorCode::Transport: break;
        }
        report(error, err);
        return ExitStatus::Failed;
    };

    auto const status = control.status();
    if (!status)
        return fail(status.error());

    if (status->sessions != 0 && consent == StopConsent::Ask)
    {
        switch (
            prompt.ask(std::format("Stop the daemon and end its {} session(s)? [y/N] ", status->sessions)))
        {
            case Confirmation::Confirmed: break;
            case Confirmation::Declined: err << "contour daemon: not stopped\n"; return ExitStatus::Failed;
            case Confirmation::Unavailable:
                err << "contour daemon: pass --yes to stop a daemon that hosts sessions\n";
                return ExitStatus::Failed;
        }
    }

    if (auto const stopped = control.stop(vthost::proto::StopReason::User); !stopped)
        return fail(stopped.error());
    out << "daemon stopped\n";
    return ExitStatus::Ok;
}

ExitStatus daemonLogsVerb(DaemonControlPort& control,
                          std::filesystem::path const& fallbackLog,
                          LogsOptions options,
                          core::platform::FileSystem& fs,
                          std::function<FollowStep()> const& waitTick,
                          std::ostream& out,
                          std::ostream& err)
{
    auto file = std::filesystem::path {};
    if (auto const status = control.status())
    {
        if (status->logFile.empty())
        {
            err << "contour daemon: the daemon logs to its standard error\n";
            return ExitStatus::Failed;
        }
        file = status->logFile;
    }
    else if (status.error().code == ControlErrorCode::NotRunning)
    {
        file = fallbackLog;
        err << std::format("contour daemon: no daemon is running; showing {}\n", core::pathToUtf8(file));
    }
    else
    {
        report(status.error(), err);
        return ExitStatus::Failed;
    }

    if (!fs.isRegularFile(file))
    {
        err << std::format("contour daemon: cannot read the log {}\n", core::pathToUtf8(file));
        return ExitStatus::Failed;
    }

    auto follower = LogFollower { fs, file };
    out << follower.tail(options.lines) << std::flush;
    if (options.follow == FollowMode::Follow)
        while (waitTick() == FollowStep::Continue)
            out << follower.poll() << std::flush;
    return ExitStatus::Ok;
}

} // namespace contour::cli
