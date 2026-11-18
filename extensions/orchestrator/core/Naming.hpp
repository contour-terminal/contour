// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <orchestrator/core/Model.hpp>

namespace orchestrator
{

/// A wall-clock time to the second, in the host's local time: what a run's names are stamped with.
struct Stamp
{
    uint16_t year = 0;  ///< Such as 2026.
    uint8_t month = 0;  ///< 1 to 12.
    uint8_t day = 0;    ///< 1 to 31.
    uint8_t hour = 0;   ///< 0 to 23.
    uint8_t minute = 0; ///< 0 to 59.
    uint8_t second = 0; ///< 0 to 59.

    [[nodiscard]] constexpr bool operator==(Stamp const&) const noexcept = default;
};

/// The latest instant a Stamp spells with a four-digit year: 9999-12-31 23:59:59.999, local or UTC.
inline constexpr uint64_t MaxStampUnixMs = 253'402'300'799'999;

/// The largest UTC offset stampFromUnixMs honours, in minutes either way: one day. Real offsets lie
/// between -12 and +14 hours.
inline constexpr int32_t MaxUtcOffsetMinutes = 1440;

/// The most bytes a Jira key may have, so that a run id made from it is still a Name.
inline constexpr std::size_t MaxJiraKeyBytes = 32;

/// The local time at a Unix time, computed from the calendar alone: no clock and no time zone is read.
/// @param ms Milliseconds since the Unix epoch.
/// @param utcOffsetMinutes The host's tm_gmtoff / 60; the engine never asks for a time zone. Clamped to
///        ±MaxUtcOffsetMinutes.
/// @return The local time, the milliseconds dropped; a local time later than MaxStampUnixMs counts as
///         MaxStampUnixMs, so that the year always has four digits.
[[nodiscard]] constexpr Stamp stampFromUnixMs(uint64_t ms, int32_t utcOffsetMinutes) noexcept
{
    using namespace std::chrono;
    using LocalMs = sys_time<milliseconds>;
    constexpr auto Latest = LocalMs { milliseconds { static_cast<int64_t>(MaxStampUnixMs) } };
    auto const offset = minutes { std::clamp(utcOffsetMinutes, -MaxUtcOffsetMinutes, MaxUtcOffsetMinutes) };
    auto const local = std::min(
        LocalMs { milliseconds { static_cast<int64_t>(std::min(ms, MaxStampUnixMs)) } } + offset, Latest);
    auto const midnight = floor<days>(local);
    auto const date = year_month_day { midnight };
    auto const time = hh_mm_ss { floor<seconds>(local - midnight) };
    return Stamp { .year = static_cast<uint16_t>(static_cast<int>(date.year())),
                   .month = static_cast<uint8_t>(static_cast<unsigned>(date.month())),
                   .day = static_cast<uint8_t>(static_cast<unsigned>(date.day())),
                   .hour = static_cast<uint8_t>(time.hours().count()),
                   .minute = static_cast<uint8_t>(time.minutes().count()),
                   .second = static_cast<uint8_t>(time.seconds().count()) };
}

/// Whether @p text is a Jira issue key: `[A-Z][A-Z0-9]+-[0-9]+`, at most MaxJiraKeyBytes.
/// @param text The candidate key.
/// @return Whether it is one.
[[nodiscard]] constexpr bool isJiraKey(std::string_view text) noexcept
{
    auto const isUpper = [](char c) {
        return c >= 'A' && c <= 'Z';
    };
    auto const isDigit = [](char c) {
        return c >= '0' && c <= '9';
    };
    auto const dash = text.find('-');
    if (text.size() > MaxJiraKeyBytes || dash == std::string_view::npos || dash < 2)
        return false;
    auto const project = text.substr(0, dash);
    auto const number = text.substr(dash + 1);
    return isUpper(project.front())
           && std::ranges::all_of(project, [&](char c) { return isUpper(c) || isDigit(c); })
           && !number.empty() && std::ranges::all_of(number, isDigit);
}

/// Why a run's names could not be made.
enum class NamingError : uint8_t
{
    NotJiraKey, ///< The item is a JiraRef whose key fails isJiraKey.
};

/// The item's part of a run's names: `pr-<number>`, `issue-<number>`, or the Jira key in lower case.
/// @param item The item.
/// @return The slug, such as "pr-2334", "issue-2301" or "ct-418", or NamingError::NotJiraKey.
[[nodiscard]] std::expected<std::string, NamingError> itemSlug(ItemRef const& item);

class RunId;

/// A run's id: the item's slug and the stamp, such as "pr-2334-20261113-150847".
///
/// It is made from numbers, Jira keys and the stamp only, never from a title or a branch the forge
/// reported, so that every run id is a Name.
/// @param item The item the run works on.
/// @param stamp When the run started, in local time.
/// @return The run id, or why the item makes none.
[[nodiscard]] std::expected<RunId, NamingError> runId(ItemRef const& item, Stamp const& stamp);

/// A run's id, which only runId() makes: it is always a Name, so the names derived from it are a Ref and
/// a Path.
class RunId
{
  public:
    /// The id's text, such as "pr-2334-20261113-150847".
    [[nodiscard]] std::string_view text() const noexcept { return _text; }

    [[nodiscard]] bool operator==(RunId const&) const noexcept = default;

  private:
    explicit RunId(std::string text) noexcept: _text { std::move(text) } {}

    friend std::expected<RunId, NamingError> runId(ItemRef const& item, Stamp const& stamp);

    std::string _text;
};

/// What every run's branch starts with.
inline constexpr std::string_view BranchPrefix = "agent/";

/// A run's branch: `agent/<runId>`, always a Ref.
/// @param runId The run's id.
/// @return The branch name, such as "agent/pr-2334-20261113-150847".
[[nodiscard]] std::string branchName(RunId const& runId);

/// A run's worktree: `<root>/<repo>/<runId>`, a Path whenever @p root is one and @p repo is a Name.
/// @param root The worktree root, absolute and normalised.
/// @param repo The repository's name.
/// @param runId The run's id.
/// @return The worktree's path.
[[nodiscard]] std::string worktreePath(std::string_view root, std::string_view repo, RunId const& runId);

/// The worktree a dispatch under @p root names for a run already made, such as a stored one: what its stored
/// worktree is compared with. It makes no new name, so the id need not be a RunId.
/// @param root The worktree root.
/// @param repo The repository's name.
/// @param runId The stored run's id.
/// @return `<root>/<repo>/<runId>`.
[[nodiscard]] std::string worktreePath(std::string_view root, std::string_view repo, std::string_view runId);

} // namespace orchestrator
