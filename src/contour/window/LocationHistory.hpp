// SPDX-License-Identifier: Apache-2.0
#pragma once
/// @file
/// A window's back stack (JumpBack, spec §8): the places it left through a recorded landing. Qt-free.

#include <contour/window/DaemonSection.hpp>
#include <contour/window/SidebarTree.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace contour::window
{

/// How a window came to a place.
enum class HistoryCause : uint8_t
{
    Jump = 0,  ///< A jump, a workspace switch, a sidebar activation, a switcher pick.
    Attention, ///< JumpToNextAttention: a run of them is one entry, the place before the run.
    Back,      ///< JumpBack's own landing: recorded nothing, and ends a run.
};

/// Production capacity (spec §8).
inline constexpr std::size_t LocationHistoryCapacity = 16;

/// What JumpBack says when no place is left to go back to.
inline constexpr auto NowhereToGoBack = std::string_view { "Nowhere to go back to" };

/// @param from Where a window was.
/// @param to Where it landed.
/// @return Whether the landing went nowhere: the same daemon, run and workspace, and the same tab -- or no
/// tab
///         named, as a workspace re-selected without one stays on the tab it shows.
[[nodiscard]] constexpr bool landsWhereItWas(SidebarTarget const& from, SidebarTarget const& to) noexcept
{
    return from.daemon == to.daemon && from.instance == to.instance && from.workspace == to.workspace
           && (!to.tab || to.tab == from.tab);
}

/// @param entry A place of the back stack.
/// @param sections Every daemon the window can go to, its own among them (windowSections()), each with the
///        directory last known of it: a section not connected now counts, as the jump back connects it.
/// @return Whether @p entry is still there: its daemon is listed under the same run, and that run's directory
///         lists its workspace and, if one is named, its tab. A daemon that restarted lists other nodes under
///         the ids of the old ones. As jumpControlIdOf() refuses a jump to a run its section does not state,
///         a daemon listed before it stated its run holds none of its places yet.
[[nodiscard]] bool placeStillExists(SidebarTarget const& entry, std::span<DaemonSection const> sections);

/// A window's back stack of the places it left through a recorded landing.
class LocationHistory
{
  public:
    /// @param capacity The most entries kept; the oldest goes first.
    explicit LocationHistory(std::size_t capacity);

    /// Notes that the window left @p from for @p to. Nothing for no @p from, or a @p to of the same daemon,
    /// run, workspace and tab (landsWhereItWas()). Attention records only a run's first origin; Back records
    /// nothing; any cause but Attention ends a run that moved the window, and Back ends one even where it did
    /// not.
    /// @param from Where the window was; none for nowhere it could go back to.
    /// @param to Where it landed.
    /// @param cause How it came there.
    void landed(std::optional<SidebarTarget> from, SidebarTarget const& to, HistoryCause cause);

    /// Pops entries until one goes somewhere: @p stillExists accepts it, and it is not where the window is.
    /// @param current Where the window is now (a place it reached unrecorded, as a tab-strip click, among
    ///        them); none for nowhere: no entry is skipped for it.
    /// @param stillExists Whether an entry's daemon run, workspace and tab are still there.
    /// @return That entry; none when the stack ran out.
    [[nodiscard]] std::optional<SidebarTarget> back(
        std::optional<SidebarTarget> const& current,
        std::function<bool(SidebarTarget const&)> const& stillExists);

    /// @param current Where the window is now; none for nowhere.
    /// @param stillExists Whether an entry's daemon run, workspace and tab are still there.
    /// @return Whether back() would go somewhere: an entry is still there and is not @p current.
    [[nodiscard]] bool canGoBack(std::optional<SidebarTarget> const& current,
                                 std::function<bool(SidebarTarget const&)> const& stillExists) const;

    /// Puts back @p place, which back() returned, as the newest entry: the jump back to it failed or was
    /// cancelled, so it is still a place to go back to.
    /// @param place The entry back() returned.
    void restore(SidebarTarget place);

    /// @return How many entries it holds.
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    /// @return Whether @p entry goes somewhere from @p current: it is not where the window is.
    [[nodiscard]] static bool goesSomewhere(std::optional<SidebarTarget> const& current,
                                            SidebarTarget const& entry) noexcept;

    /// Whether a run of attention jumps is under way: its origin is recorded, its steps are not.
    enum class AttentionRun : uint8_t
    {
        None = 0, ///< No run: the next attention jump records where it starts.
        Open,     ///< A run: the next attention jump records nothing.
    };

    std::size_t _capacity;
    std::deque<SidebarTarget> _entries;
    AttentionRun _run = AttentionRun::None;
};

} // namespace contour::window
