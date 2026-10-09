// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/OwnedFd.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <system_error>

namespace vtpty
{

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
    ParkedChild(int pid, OwnedFd pidfd, OwnedFd gate) noexcept;
    ParkedChild(ParkedChild&& other) noexcept = default;
    ParkedChild& operator=(ParkedChild&&) = delete;
    ParkedChild(ParkedChild const&) = delete;
    ParkedChild& operator=(ParkedChild const&) = delete;
    ~ParkedChild();

    /// @return The child's process id.
    [[nodiscard]] int pid() const noexcept { return _pid; }

    /// @return A pidfd for the child, or -1 when there is none.
    [[nodiscard]] int pidfd() const noexcept { return _pidfd.get(); }

    /// @return Whether the child has already exited -- killed while parked, say, and perhaps reaped,
    ///         so that its pid may name another process by now. False where there is no pidfd to ask.
    [[nodiscard]] bool hasExited() const noexcept;

    /// Lets the child go on to exec(). Idempotent, and never raises SIGPIPE, even when the child has
    /// already died.
    void release() noexcept;

  private:
    int _pid;
    OwnedFd _pidfd;
    OwnedFd _gate;
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
