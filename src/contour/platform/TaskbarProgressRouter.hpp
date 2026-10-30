// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/ProgressState.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <utility>

namespace contour::platform
{

/// What a taskbar backend should be told to show.
///
/// Deliberately NOT vtbackend::Progress: this is what the desktop indicator shows, aggregated
/// across a window's sessions on Windows or the application's sessions on other desktops.
struct TaskbarProgress
{
    vtbackend::ProgressState state = vtbackend::ProgressState::Inactive;
    uint8_t percentage = 0;
    bool operator==(TaskbarProgress const&) const = default;
};

/// How many tabs wait for the user, as a taskbar backend is told it.
struct TaskbarAttention
{
    unsigned waiting = 0; ///< Tabs whose program is blocked on the user (OSC 7501); none shows no badge.
    bool operator==(TaskbarAttention const&) const = default;
};

/// The label the Dock tile's badge carries for @p attention.
/// @param attention How many tabs wait for the user.
/// @return The number, or nothing for none, which leaves the tile without a badge of ours.
[[nodiscard]] inline std::string dockBadgeText(TaskbarAttention attention)
{
    return attention.waiting == 0 ? std::string {} : std::to_string(attention.waiting);
}

/// The dependency-free aggregation and staleness policy behind the OS taskbar progress indicator.
///
/// It answers one question -- given what the selected sessions have reported, and when -- what
/// should the taskbar show right now? Keeping that here rather than in the platform backend is what
/// makes it testable without a D-Bus session or a Windows shell: the decisions are
/// resolve-across-sessions and expire-when-stale, and neither needs a taskbar to check.
///
/// The clock is a parameter of every call rather than a member, so a test states the time outright
/// instead of sleeping, and the caller keeps using whatever clock it already has.
class TaskbarProgressRouter
{
  public:
    using SessionKey = uint64_t;

    /// Records what @p session now reports.
    ///
    /// @param session The reporting session.
    /// @param progress What it reported.
    /// @param now When it reported it.
    void onProgress(SessionKey session,
                    vtbackend::Progress progress,
                    std::chrono::steady_clock::time_point now)
    {
        // Inactive is a withdrawal, so the session is forgotten rather than remembered as "at 0%":
        // a remembered zero would keep resolve() reporting a bar nothing is behind.
        if (progress.state == vtbackend::ProgressState::Inactive)
            _reports.erase(session);
        else
            _reports[session] = Report { .progress = progress, .at = now };
    }

    /// Forgets @p session entirely, for a pane that closed.
    void onSessionClosed(SessionKey session) { _reports.erase(session); }

    /// What the taskbar should show for this window at @p now.
    ///
    /// With several sessions reporting at once a window has one taskbar button and therefore one
    /// answer, so they are reduced by severity: any Error wins, then Paused, then Indeterminate,
    /// then Normal -- a failure must not be hidden by a sibling that is merely busy. Among equals
    /// the LEAST complete percentage wins, so the bar reflects the work still outstanding rather
    /// than the most nearly finished part of it.
    ///
    /// @param now The current time, against which stale reports are discarded.
    /// @param timeout How long a report stays valid without an update; zero disables expiry, which
    ///        is the spec-literal behaviour ConEmu and Windows Terminal have (progress persists
    ///        until the application clears it). A non-zero value is the Ghostty-style guard against
    ///        an application that dies mid-operation and strands a bar. A parameter rather than a
    ///        member for the same reason @p now is: the caller owns the configured value, which can
    ///        change on config reload, and a test states it outright.
    /// @return What to show; Inactive means "show nothing".
    [[nodiscard]] TaskbarProgress resolve(std::chrono::steady_clock::time_point now,
                                          std::chrono::milliseconds timeout = {}) const
    {
        return resolveFor([](SessionKey) { return true; }, now, timeout);
    }

    /// Resolves the same policy for sessions belonging to one window, without copying their reports.
    /// @param includes Selects the sessions hosted by that window.
    /// @param now Current monotonic time.
    /// @param timeout Staleness limit; zero disables expiry.
    /// @return The selected sessions' aggregate.
    template <typename Predicate>
    [[nodiscard]] TaskbarProgress resolveFor(Predicate includes,
                                             std::chrono::steady_clock::time_point now,
                                             std::chrono::milliseconds timeout = {}) const
    {
        // Ranked lexicographically on (severity, -percentage): higher severity wins, and among equals
        // the LEAST complete does -- so the bar reflects the work still outstanding rather than the
        // most nearly finished part of it, and a failure is never hidden by a sibling merely busy.
        auto const rankOf = [](vtbackend::Progress const& progress) {
            return std::pair { severityOf(progress.state), -int { progress.percentage } };
        };
        auto winner = TaskbarProgress {};
        auto best = std::pair { -1, 0 }; // below every real rank, so the first live report takes it
        for (auto const& [session, report]: _reports)
        {
            if (!includes(session) || isStale(report, now, timeout))
                continue;
            if (auto const rank = rankOf(report.progress); rank > best)
            {
                best = rank;
                winner = TaskbarProgress { .state = report.progress.state,
                                           .percentage = report.progress.percentage };
            }
        }
        return winner;
    }

    /// Earliest future expiry, for scheduling a single wakeup even while windows are minimized.
    /// @param now Current monotonic time.
    /// @param timeout Staleness limit; zero disables the timer.
    /// @return Next expiry, or nullopt if no live report can expire.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> nextExpiry(
        std::chrono::steady_clock::time_point now, std::chrono::milliseconds timeout) const
    {
        auto next = std::optional<std::chrono::steady_clock::time_point> {};
        if (timeout.count() == 0)
            return next;
        for (auto const& report: _reports | std::views::values)
            if (auto const expiry = report.at + timeout; expiry > now && (!next || expiry < *next))
                next = expiry;
        return next;
    }

  private:
    struct Report
    {
        vtbackend::Progress progress;
        std::chrono::steady_clock::time_point at;
    };

    /// How loudly a state should speak when several sessions report at once. Higher wins.
    ///
    /// A table rather than a comparison on the enumerator values, because the enum is ordered by the
    /// PROTOCOL's numbering (which puts Indeterminate above Error), and that is not the order a user
    /// wants to be told about: a failed build must outrank a busy one.
    [[nodiscard]] static constexpr int severityOf(vtbackend::ProgressState state) noexcept
    {
        switch (state)
        {
            case vtbackend::ProgressState::Inactive: return 0;
            case vtbackend::ProgressState::Normal: return 1;
            case vtbackend::ProgressState::Indeterminate: return 2;
            case vtbackend::ProgressState::Paused: return 3;
            case vtbackend::ProgressState::Error: return 4;
        }
        return 0;
    }

    [[nodiscard]] static constexpr bool isStale(Report const& report,
                                                std::chrono::steady_clock::time_point now,
                                                std::chrono::milliseconds timeout) noexcept
    {
        return timeout.count() != 0 && now - report.at >= timeout;
    }

    std::unordered_map<SessionKey, Report> _reports;
};

} // namespace contour::platform
