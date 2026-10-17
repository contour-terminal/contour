// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/OwnedFd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

namespace vtpty
{

/// What a process placed in a resource domain of its own is to Contour, as the log and its scope tell.
enum class ProcessRole : std::uint8_t
{
    Session,    ///< A session's shell.
    Program,    ///< A program a session started outside its terminal: a link's opener, a pager, a second
                ///< Contour.
    Daemon,     ///< The daemon `contour client` or `contour daemon --background` started.
    TmuxClient, ///< The `tmux -C` client `contour client --tmux` talks to.
};

/// How a ProcessRole reads: one row per enumerator, in enumerator order.
struct ProcessRoleTraits
{
    ProcessRole role;
    std::string_view noun;        ///< In a log line, before the process id: "Session process 4242".
    std::string_view description; ///< Its scope's description, before "(contour pid N)".
};

inline constexpr auto ProcessRoleTable = std::array {
    ProcessRoleTraits { ProcessRole::Session, "Session process", "Contour session" },
    ProcessRoleTraits { ProcessRole::Program, "Program", "Program a Contour session started" },
    ProcessRoleTraits { ProcessRole::Daemon, "Daemon", "Contour daemon" },
    ProcessRoleTraits { ProcessRole::TmuxClient, "tmux client", "tmux client of a Contour session" },
};

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ProcessRoleTable)
            if (static_cast<std::size_t>(row.role) != index++ || row.noun.empty() || row.description.empty())
                return false;
        return index == static_cast<std::size_t>(ProcessRole::TmuxClient) + 1;
    }(),
    "ProcessRoleTable holds one described row per ProcessRole, in enumerator order");

/// @return The row of ProcessRoleTable describing @p role.
[[nodiscard]] constexpr ProcessRoleTraits const& traitsOf(ProcessRole role) noexcept
{
    return ProcessRoleTable[static_cast<std::size_t>(role)];
}

/// A child released before it was placed, kept so that it can still be placed while it runs.
class UnplacedChild
{
  public:
    /// @param pid   The child.
    /// @param pidfd A pidfd for @p pid, or an empty OwnedFd where there is none.
    /// @param role  What the child is to Contour.
    UnplacedChild(int pid, OwnedFd pidfd, ProcessRole role) noexcept;

    /// @return The child's process id.
    [[nodiscard]] int pid() const noexcept { return _pid; }

    /// @return What the child is to Contour.
    [[nodiscard]] ProcessRole role() const noexcept { return _role; }

    /// @return A pidfd for the child, or -1 when there is none -- and with it no telling whether the
    ///         pid still names this child once it may have exited.
    [[nodiscard]] int pidfd() const noexcept { return _pidfd.get(); }

    /// @return Whether the child has exited, so that its pid may name another process by now. False
    ///         where there is no pidfd to ask.
    [[nodiscard]] bool hasExited() const noexcept;

  private:
    int _pid;
    OwnedFd _pidfd;
    ProcessRole _role;
};

/// A child forked by Process and parked before exec(), waiting on its gate.
///
/// Destroying it releases the child -- on every path, an exception or shutdown included -- so no
/// code path can leave a shell parked for good. Move-constructible only: assigning over a parked
/// child would have to release it as a hidden side effect.
class ParkedChild
{
  public:
    /// @param pid   The parked child.
    /// @param pidfd A pidfd for @p pid, or an empty OwnedFd where the kernel offers none.
    /// @param gate  The parent's end of the child's gate.
    /// @param role  What the child is to Contour.
    ParkedChild(int pid, OwnedFd pidfd, OwnedFd gate, ProcessRole role = ProcessRole::Session) noexcept;
    ParkedChild(ParkedChild&& other) noexcept = default;
    ParkedChild& operator=(ParkedChild&&) = delete;
    ParkedChild(ParkedChild const&) = delete;
    ParkedChild& operator=(ParkedChild const&) = delete;
    ~ParkedChild();

    /// @return The child's process id.
    [[nodiscard]] int pid() const noexcept { return _pid; }

    /// @return What the child is to Contour.
    [[nodiscard]] ProcessRole role() const noexcept { return _role; }

    /// @return A pidfd for the child, or -1 when there is none.
    [[nodiscard]] int pidfd() const noexcept { return _pidfd.get(); }

    /// @return Whether the child has already exited -- killed while parked, say, and perhaps reaped,
    ///         so that its pid may name another process by now. False where there is no pidfd to ask.
    [[nodiscard]] bool hasExited() const noexcept;

    /// Lets the child go on to exec(). Idempotent, and never raises SIGPIPE, even when the child has
    /// already died.
    void release() noexcept;

    /// Lets the child go on to exec() unplaced, and hands over what placing it later takes.
    /// @return The child, with its pidfd.
    [[nodiscard]] UnplacedChild releaseUnplaced() noexcept;

  private:
    int _pid;
    OwnedFd _pidfd;
    OwnedFd _gate;
    ProcessRole _role;
};

#ifndef _WIN32
/// The two ends of the channel a child is parked on.
struct Gate
{
    OwnedFd parentEnd; ///< Becomes the ParkedChild's, which releases it.
    OwnedFd childEnd;  ///< Read by the child until the release arrives.
};

/// Creates a gate: a socket pair, both ends close-on-exec, whose parent end never raises SIGPIPE.
/// @return The gate, or why the socket pair could not be created.
[[nodiscard]] std::expected<Gate, std::error_code> makeGate();
#endif

/// Moves a freshly forked, still-parked child into its own resource domain before it runs.
///
/// The seam exists because a child sharing Contour's cgroup takes Contour down with it when systemd
/// stops a unit over an out-of-memory kill inside it (OOMPolicy=stop). @see SystemdScopePlacement.
class ProcessPlacement
{
  public:
    virtual ~ProcessPlacement() = default;

    /// Takes ownership of @p child, places it, and releases it -- also when placing failed.
    /// Never blocks the caller, and never throws: Process::start() runs on the GUI thread, and
    /// reports its failures as values.
    virtual void placeThenRelease(ParkedChild child) noexcept = 0;

    /// @return The oom_score_adj each child sets for itself before it runs, or nothing to leave it
    ///         as inherited. A higher value than Contour's makes the kernel, in a global
    ///         out-of-memory, pick a session's process before Contour.
    [[nodiscard]] virtual std::optional<int> childOomScoreAdjust() const noexcept = 0;
};

/// Releases every child at once, placing none. Used off Linux, in Flatpak, in builds without systemd
/// support, and wherever a test or tool must not create systemd units.
class NoPlacement final: public ProcessPlacement
{
  public:
    /// @param childOomScoreAdjust The oom_score_adj each child sets for itself, if any.
    explicit NoPlacement(std::optional<int> childOomScoreAdjust = std::nullopt) noexcept:
        _childOomScoreAdjust { childOomScoreAdjust }
    {
    }

    void placeThenRelease(ParkedChild child) noexcept override { child.release(); }

    [[nodiscard]] std::optional<int> childOomScoreAdjust() const noexcept override
    {
        return _childOomScoreAdjust;
    }

  private:
    std::optional<int> _childOomScoreAdjust;
};

/// @return The placement this build and platform support: systemd scopes on a Linux build with
///         CONTOUR_WITH_SYSTEMD (which a Flatpak build turns off), NoPlacement otherwise. Cheap: nothing
///         connects until the first child is placed.
[[nodiscard]] std::shared_ptr<ProcessPlacement> makeDefaultProcessPlacement();

} // namespace vtpty
