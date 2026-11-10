// SPDX-License-Identifier: Apache-2.0
#include <vthost/SshTransport.hpp>

#include <core/Overloaded.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/WithTimeout.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <vthost/Logging.hpp>
#include <vthost/StdioPreamble.hpp>

namespace vthost
{

namespace
{
    using namespace std::string_view_literals;
    using ConnectResult = std::expected<std::unique_ptr<Connection>, TransportError>;

    /// How long an ended ssh is given to report its exit code and finish its stderr.
    constexpr auto ExitCodeWait = std::chrono::milliseconds { 500 };

    /// How much one read of the preamble asks for.
    constexpr auto PreambleReadChunk = std::size_t { 4096 };

    /// One row of the message table: what a failure is called and what the user reads.
    struct SshFailureMessage
    {
        SshFailure failure;
        TransportErrorKind kind;
        /// A std::format string over: {0} the target, {1} the command to run once, {2} the host as
        /// known_hosts names it, {3} the detail, {4} the exit code.
        std::string_view format;
        /// The words when Contour answered ssh's prompts; empty: @c format serves both modes.
        std::string_view askpassFormat = {};
    };

    /// What a login that needs a prompt batch mode cannot give says.
    constexpr auto PromptNeeded = std::string_view {
        "ssh could not log in to {0} without a prompt (password, passphrase, 2FA or an unknown "
        "host key). Run this once in a terminal, then attach again:\n    {1}"
    };

    /// Appended to a Batch-mode failure when the local ssh is too old to be asked for passwords: {0} the
    /// version found.
    constexpr auto OldSshAdvice = std::string_view {
        "\nContour can only ask for passwords with OpenSSH 8.4 or newer (found {0}); on Windows install the "
        "current Win32-OpenSSH release."
    };

    /// Every SshFailure's kind and words, one row each.
    constexpr auto SshFailureMessages = std::to_array<SshFailureMessage>({
        { .failure = SshFailure::HostKeyUnknown,
          .kind = TransportErrorKind::HostKeyUnknown,
          .format = PromptNeeded,
          .askpassFormat = "The host key of {0} was not accepted: {3}" },
        { .failure = SshFailure::HostKeyChanged,
          .kind = TransportErrorKind::HostKeyUnknown,
          .format = "The host key of {0} has changed. If you expected that, remove the old key with "
                    "`ssh-keygen -R {2}` and run `{1}` once in a terminal." },
        { .failure = SshFailure::AuthenticationRequired,
          .kind = TransportErrorKind::AuthenticationRequired,
          .format = PromptNeeded,
          .askpassFormat = "ssh could not log in to {0}: {3}" },
        { .failure = SshFailure::HostUnreachable,
          .kind = TransportErrorKind::Unreachable,
          .format = "ssh could not reach {0}: {3}" },
        { .failure = SshFailure::RemoteContourMissing,
          .kind = TransportErrorKind::RemoteProgramMissing,
          .format = "Contour was not found on {0} by a non-interactive ssh login. Install it there, or set "
                    "remote_command: for this host to the full path, e.g. \"/opt/contour/bin/contour daemon "
                    "--stdio\"." },
        { .failure = SshFailure::RemoteContourTooOld,
          .kind = TransportErrorKind::RemoteProgramMissing,
          .format =
              "Contour on {0} is too old for ssh attach (it has no `contour daemon --stdio`). Upgrade it "
              "there." },
        { .failure = SshFailure::RemoteRelayError,
          .kind = TransportErrorKind::Refused,
          .format = "{0}: {3}" },
        // Refused, so Permanent: retrying cannot quiet a login script.
        { .failure = SshFailure::RemoteShellNoise,
          .kind = TransportErrorKind::Refused,
          .format = "{0} printed more than 64 KiB before Contour answered. Its login shell prints output on "
                    "non-interactive logins (check .bashrc / profile scripts)." },
        { .failure = SshFailure::NoPreamble,
          .kind = TransportErrorKind::Unreachable,
          .format = "ssh to {0} ended before Contour answered (exit code {4}): {3}" },
        { .failure = SshFailure::Cancelled,
          .kind = TransportErrorKind::Cancelled,
          .format = "The ssh login to {0} was cancelled." },
    });

    /// @return @p host in brackets when it is an IPv6 address, as a URL spells it.
    [[nodiscard]] std::string bracketed(std::string_view host)
    {
        if (host.contains(':'))
            return std::format("[{}]", host);
        return std::string { host };
    }

    /// @return `[user@]host`, what ssh takes as its destination.
    [[nodiscard]] std::string destinationOf(SshTarget const& target)
    {
        if (target.user.empty())
            return target.host;
        return std::format("{}@{}", target.user, target.host);
    }

    /// @return The host as known_hosts names it: `[host]:port` for a port of its own, quoted for a shell.
    [[nodiscard]] std::string knownHostsName(SshTarget const& target)
    {
        if (target.port)
            return std::format("'[{}]:{}'", target.host, *target.port);
        return target.host;
    }

    /// @return The last line of @p text with anything but blanks in it, trimmed.
    [[nodiscard]] std::string_view lastNonEmptyLine(std::string_view text)
    {
        constexpr auto Blanks = " \t\r"sv;
        auto last = std::string_view {};
        for (auto const piece: text | std::views::split('\n'))
        {
            auto line = std::string_view { piece.begin(), piece.end() };
            auto const first = line.find_first_not_of(Blanks);
            if (first == std::string_view::npos)
                continue;
            line.remove_prefix(first);
            line.remove_suffix(line.size() - line.find_last_not_of(Blanks) - 1);
            last = line;
        }
        return last;
    }

    /// Reads @p socket until the preamble is decided.
    /// @return What scanPreamble found; PreambleIncomplete when the stream ended (EOF or a read error)
    ///         before it could decide.
    core::async::Task<PreambleScan> readPreamble(core::net::ISocket* socket)
    {
        auto received = std::string {};
        auto buffer = std::vector<std::byte>(PreambleReadChunk);
        while (true)
        {
            auto const read = co_await socket->read(buffer);
            if (!read.has_value() || *read == 0)
                co_return PreambleIncomplete {};
            auto const bytes = std::span { buffer }.first(*read);
            for (auto const byte: bytes)
                received.push_back(static_cast<char>(byte));
            auto scan = scanPreamble(received);
            if (!std::holds_alternative<PreambleIncomplete>(scan))
                co_return scan;
        }
    }

    /// Ends @p child and fails with @p error.
    [[nodiscard]] ConnectResult failWith(IStdioChild& child, TransportError error)
    {
        child.terminate();
        return std::unexpected(std::move(error));
    }
} // namespace

std::string describeSshTarget(SshTarget const& target)
{
    auto const user = target.user.empty() ? std::string {} : std::format("{}@", target.user);
    auto const port = target.port ? std::format(":{}", *target.port) : std::string {};
    return std::format("ssh://{}{}{}", user, bracketed(target.host), port);
}

std::optional<OpenSshVersion> parseOpenSshVersion(std::string_view sshDashV)
{
    constexpr auto Marker = "OpenSSH_"sv;
    auto const at = sshDashV.find(Marker);
    if (at == std::string_view::npos)
        return std::nullopt;
    auto rest = sshDashV.substr(at + Marker.size());
    // The Windows build says "OpenSSH_for_Windows_8.1p1".
    constexpr auto WindowsInfix = "for_Windows_"sv;
    if (rest.starts_with(WindowsInfix))
        rest.remove_prefix(WindowsInfix.size());
    auto const number = [&rest]() -> std::optional<int> {
        auto value = 0;
        auto const [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), value);
        if (error != std::errc {} || end == rest.data())
            return std::nullopt;
        rest.remove_prefix(static_cast<std::size_t>(end - rest.data()));
        return value;
    };
    auto const major = number();
    if (!major || !rest.starts_with('.'))
        return std::nullopt;
    rest.remove_prefix(1);
    auto const minor = number();
    if (!minor)
        return std::nullopt;
    return OpenSshVersion { .major = *major, .minor = *minor };
}

SshPromptMode promptModeFor(std::optional<OpenSshVersion> found)
{
    return found && *found >= AskpassMinimumOpenSsh ? SshPromptMode::Askpass : SshPromptMode::Batch;
}

std::vector<std::string> sshArguments(SshTarget const& target, SshPromptMode mode)
{
    auto argv = std::vector<std::string> { "ssh", "-T" };
    switch (mode)
    {
        case SshPromptMode::Batch: argv.insert(argv.end(), { "-o", "BatchMode=yes" }); break;
        case SshPromptMode::Askpass: argv.insert(argv.end(), { "-o", "NumberOfPasswordPrompts=3" }); break;
    }
    argv.insert(argv.end(),
                { "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3" });
    if (target.port)
    {
        argv.emplace_back("-p");
        argv.push_back(std::to_string(*target.port));
    }
    argv.emplace_back("--");
    argv.push_back(destinationOf(target));
    argv.push_back(target.remoteCommand);
    return argv;
}

std::string interactiveSshCommand(SshTarget const& target)
{
    if (target.port)
        return std::format("ssh -p {} {}", *target.port, destinationOf(target));
    return std::format("ssh {}", destinationOf(target));
}

SshFailure classifySshFailure(std::string_view stderrTail, std::optional<int> exitCode)
{
    for (auto const& rule: SshFailureRules)
        if (stderrTail.contains(rule.needle))
            return rule.failure;
    if (exitCode == ShellCommandNotFound)
        return SshFailure::RemoteContourMissing;
    return SshFailure::NoPreamble;
}

TransportError sshTransportError(SshFailure failure, SshFailureContext const& context)
{
    auto const& target = context.target;
    auto const row = std::ranges::find(SshFailureMessages, failure, &SshFailureMessage::failure);
    // Every SshFailure has a row; a missing one is a programming error the table test catches.
    if (row == SshFailureMessages.end())
        return TransportError { .kind = TransportErrorKind::Refused,
                                .message = std::format("ssh to {} failed", describeSshTarget(target)) };
    auto const described = describeSshTarget(target);
    auto const command = interactiveSshCommand(target);
    auto const knownHost = knownHostsName(target);
    auto const code = context.exitCode && *context.exitCode != ExitStatusLost
                          ? std::to_string(*context.exitCode)
                          : std::string { "unknown" };
    auto const useAskpassWords = context.mode == SshPromptMode::Askpass && !row->askpassFormat.empty();
    auto message = std::vformat(useAskpassWords ? row->askpassFormat : row->format,
                                std::make_format_args(described, command, knownHost, context.detail, code));
    auto const oldSsh = context.mode == SshPromptMode::Batch && context.foundVersion
                        && *context.foundVersion < AskpassMinimumOpenSsh;
    if (oldSsh && !row->askpassFormat.empty())
    {
        auto const found = std::format("{}.{}", context.foundVersion->major, context.foundVersion->minor);
        message += std::vformat(OldSshAdvice, std::make_format_args(found));
    }
    return TransportError { .kind = row->kind, .message = std::move(message) };
}

SshTransportFactory::SshTransportFactory(SshTarget target,
                                         std::shared_ptr<IStdioSpawner> spawner,
                                         std::chrono::milliseconds connectTimeout):
    _target(std::move(target)), _spawner(std::move(spawner)), _connectTimeout(connectTimeout)
{
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> SshTransportFactory::connect(
    core::net::EventLoop& loop)
{
    return connectOn(&loop);
}

core::async::Task<std::expected<std::unique_ptr<Connection>, TransportError>> SshTransportFactory::connectOn(
    core::net::EventLoop* loop)
{
    auto const argv = sshArguments(_target, SshPromptMode::Batch);
    auto spawned = _spawner->spawn(*loop, argv);
    if (!spawned)
    {
        if (spawned.error().kind == SpawnFailure::ProgramNotFound)
            co_return std::unexpected(
                TransportError { .kind = TransportErrorKind::ProgramNotFound,
                                 .message = "ssh was not found on PATH; install the OpenSSH client" });
        // The operating system refused a step (descriptors, memory): that may pass.
        co_return std::unexpected(
            TransportError { .kind = TransportErrorKind::Unreachable, .message = spawned.error().message });
    }
    auto socket = std::move(spawned->transport);
    auto child = std::shared_ptr<IStdioChild> { std::move(spawned->process) };

    auto scanned = co_await core::net::withTimeout(loop, readPreamble(socket.get()), _connectTimeout);
    if (!scanned)
    {
        auto const seconds = (_connectTimeout.count() + 999) / 1000;
        co_return failWith(
            *child,
            TransportError { .kind = TransportErrorKind::Unreachable,
                             .message =
                                 std::format("{} did not answer within {} seconds", describe(), seconds) });
    }

    co_return std::visit(
        core::Overloaded {
            [&](PreambleReady const& ready) -> ConnectResult {
                if (ready.skipped != 0)
                    clientLog()(
                        "ssh: {} printed {} bytes before the stdio preamble", describe(), ready.skipped);
                // The child is the connection's lifetime: ssh is reaped only once its socket is gone.
                return std::make_unique<Connection>(
                    Connection { .lifetime = std::move(child), .socket = std::move(socket), .token = {} });
            },
            [&](PreambleIncomplete const&) -> ConnectResult {
                socket.reset();
                auto const exitCode = child->exitCode(ExitCodeWait);
                auto const tail = child->stderrTail();
                return failWith(
                    *child,
                    sshTransportError(
                        classifySshFailure(tail, exitCode),
                        { .target = _target, .detail = lastNonEmptyLine(tail), .exitCode = exitCode }));
            },
            [&](PreambleRefused const& refused) -> ConnectResult {
                return failWith(*child,
                                sshTransportError(SshFailure::RemoteRelayError,
                                                  { .target = _target, .detail = refused.message }));
            },
            [&](PreambleOverflow const&) -> ConnectResult {
                return failWith(*child,
                                sshTransportError(SshFailure::RemoteShellNoise, { .target = _target }));
            },
            [&](PreambleTrailing const&) -> ConnectResult {
                return failWith(
                    *child,
                    TransportError { .kind = TransportErrorKind::Protocol,
                                     .message = std::format(
                                         "unexpected bytes after the stdio preamble from {}", describe()) });
            },
        },
        *scanned);
}

std::string SshTransportFactory::describe() const
{
    return describeSshTarget(_target);
}

} // namespace vthost
