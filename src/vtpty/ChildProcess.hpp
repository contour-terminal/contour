// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/AdoptedChild.hpp>

#include <core/Overloaded.hpp>

#ifndef _WIN32
    #include <csignal>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace vtpty
{

// clang-format off
/// A child that ended on its own accord.
struct NormalExit { int exitCode; };
/// A child that ended on a signal.
struct SignalExit { int signum; };
// clang-format on

/// How a child ended.
using ExitStatus = std::variant<NormalExit, SignalExit>;

/// How far a child has come towards running its program.
enum class SpawnProgress : uint8_t
{
    Settled = 0, ///< No start in flight: the child has set its terminal up, or there is no child.
    InProgress,  ///< Spawned, but its terminal not yet set up: input written now is discarded by its flush.
};

/// How ChildProcess::terminate() asks the child to end.
enum class TerminationHint : uint8_t
{
    Normal, ///< An ordinary termination (SIGTERM), which the child may catch or ignore.
    Hangup, ///< As a closed terminal would (SIGHUP), which the child may catch or ignore.
    Kill,   ///< Outright (SIGKILL), which no child can refuse.
};

/// What carries a child across a live upgrade of the daemon.
enum class CrossingKind : uint8_t
{
    None = 0,  ///< Nothing: the child does not cross.
    Adoptable, ///< The child stays a child of the daemon across exec, and the successor adopts it.
    Keeper,    ///< A keeper process holds the child; the daemon only names it.
};

/// How a child crosses a live upgrade.
struct ChildCrossing
{
    CrossingKind kind = CrossingKind::None;
    /// The child to adopt, for CrossingKind::Adoptable. Empty for an Adoptable child that is gone (reaped, or
    /// never spawned): there is nothing to hand over, and a handover refuses the session
    /// (HandoverError::UnsupportedSession) rather than carry it as a bare PTY.
    std::optional<AdoptedChild> child;
    uint64_t keeperSession = 0; ///< The keeper's name for the child, for CrossingKind::Keeper.
};

/// How a child's exit status becomes known, measured from its terminal's hangup.
enum class StatusArrival : std::uint8_t
{
    WithHangup = 0, ///< By the hangup, or moments after it: this process reaps the child itself.
    /// When whoever reaps it says so (a PTY keeper), which may be well after the hangup: wait for
    /// ChildProcess::whenStatusKnown() rather than for a moment.
    Announced,
};

/// A shell's process, as the daemon manages it: spawned by this process, or held by someone else.
class ChildProcess
{
  public:
    ChildProcess() = default;
    ChildProcess(ChildProcess const&) = default;
    ChildProcess(ChildProcess&&) = default;
    ChildProcess& operator=(ChildProcess const&) = default;
    ChildProcess& operator=(ChildProcess&&) = default;
    virtual ~ChildProcess() = default;

    /// Whether the child has yet to set its terminal up.
    /// @return InProgress until the child settled; Settled otherwise, and where there is no child.
    [[nodiscard]] virtual SpawnProgress spawnProgress() const = 0;

    /// @return The child's exit status once it has exited; nullopt while it runs, or when there is none.
    [[nodiscard]] virtual std::optional<ExitStatus> checkStatus() const = 0;

    /// @return The child's current working directory; empty when it cannot be told.
    [[nodiscard]] virtual std::string workingDirectory() const = 0;

    /// @return The name of the program leading the terminal's foreground process group; none where it cannot
    ///         be told.
    [[nodiscard]] virtual std::optional<std::string> foregroundProcessName() const = 0;

    /// Asks the child to end, or does nothing when there is no child running.
    /// @param hint How to ask.
    virtual void terminate(TerminationHint hint) = 0;

    /// @return How the child crosses a live upgrade.
    [[nodiscard]] virtual ChildCrossing crossing() const = 0;

    /// @return How the child's exit status becomes known; WithHangup unless someone else reaps it.
    [[nodiscard]] virtual StatusArrival statusArrival() const { return StatusArrival::WithHangup; }

    /// Runs @p then once checkStatus() is final: the status known, or known never to come. At once by
    /// default, as for a child this process reaps; later for a StatusArrival::Announced one. On the thread
    /// that learns it (a daemon's loop).
    /// @param then What to run; at most once.
    virtual void whenStatusKnown(std::function<void()> then)
    {
        auto const run = std::move(then);
        run();
    }
};

#ifndef _WIN32
/// A signal of POSIX and the name it goes by.
struct SignalName
{
    int number;
    std::string_view name;
};

/// The names of the signals a child commonly ends on. A table rather than strsignal(), which is not
/// thread safe.
inline constexpr auto SignalNames = std::array {
    SignalName { SIGHUP, "SIGHUP" },   SignalName { SIGINT, "SIGINT" },   SignalName { SIGQUIT, "SIGQUIT" },
    SignalName { SIGILL, "SIGILL" },   SignalName { SIGTRAP, "SIGTRAP" }, SignalName { SIGABRT, "SIGABRT" },
    SignalName { SIGBUS, "SIGBUS" },   SignalName { SIGFPE, "SIGFPE" },   SignalName { SIGKILL, "SIGKILL" },
    SignalName { SIGUSR1, "SIGUSR1" }, SignalName { SIGSEGV, "SIGSEGV" }, SignalName { SIGUSR2, "SIGUSR2" },
    SignalName { SIGPIPE, "SIGPIPE" }, SignalName { SIGALRM, "SIGALRM" }, SignalName { SIGTERM, "SIGTERM" },
    SignalName { SIGCHLD, "SIGCHLD" }, SignalName { SIGCONT, "SIGCONT" }, SignalName { SIGSTOP, "SIGSTOP" },
    SignalName { SIGTSTP, "SIGTSTP" }, SignalName { SIGTTIN, "SIGTTIN" }, SignalName { SIGTTOU, "SIGTTOU" },
    SignalName { SIGSYS, "SIGSYS" },
};

/// @param signum A signal number.
/// @return Its name, or "unknown signal".
[[nodiscard]] constexpr std::string_view signalName(int signum) noexcept
{
    for (auto const& entry: SignalNames)
        if (entry.number == signum)
            return entry.name;
    return "unknown signal";
}
#endif

} // namespace vtpty

template <>
struct std::formatter<vtpty::ExitStatus>: std::formatter<std::string>
{
    auto format(vtpty::ExitStatus const& status, auto& ctx) const
    {
        auto const text =
            std::visit(core::Overloaded { [&](vtpty::NormalExit exit) {
                                             return std::format("{} (normal exit)", exit.exitCode);
                                         },
                                          [&](vtpty::SignalExit exit) {
#ifdef _WIN32
                                              return std::format("signal number {}", exit.signum);
#else
                                              return std::format("{} (signal number {})",
                                                                 vtpty::signalName(exit.signum),
                                                                 exit.signum);
#endif
                                          } },
                       status);
        return std::formatter<std::string>::format(text, ctx);
    }
};
