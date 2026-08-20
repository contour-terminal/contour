// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/LineFlags.hpp>
#include <vtbackend/Primitives.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// One foldable region: the output of ONE finished command block, addressed by STABLE line id.
///
/// Stable ids rather than LineOffsets, because a fold outlives the scrolling that moves its lines:
/// Grid::stableLineIdOf() names a physical row across ring rotations, so a fold set keyed by id
/// survives a scroll for free and needs attention only when a row is evicted (Grid::stableRangeFloor(),
/// @see FoldState::prune) or when row identity is destroyed wholesale (Grid::generation(),
/// @see FoldState::clear).
///
/// Ids DECREASE upwards, so @ref firstStableId (the topmost hidden line) is the smallest of the three.
struct FoldRange
{
    int64_t headStableId {};  ///< The prompt line the fold hangs off (the LineFlag::Marked head).
    int64_t firstStableId {}; ///< Topmost hidden line when collapsed.
    int64_t lastStableId {};  ///< Bottom-most hidden line, inclusive.

    [[nodiscard]] bool operator==(FoldRange const&) const noexcept = default;
};

/// Supplies the LOGICAL lines a fold scan walks, indexed backwards from the line it starts at.
///
/// The dependency-injection seam, mirroring CommandBlockLineSource and PromptRegionLineSource: the scan
/// below is a pure function of the flags and ids it is handed, so it can be exercised against a plain
/// vector — with no Grid, no Screen and no terminal behind it.
///
/// Logical, not physical, for the reason its two siblings give: a shell's marks name the line it wrote,
/// and a window resize re-chops the physical pieces without moving a single mark.
class FoldLineSource
{
  public:
    FoldLineSource() = default;
    FoldLineSource(FoldLineSource&&) = default;
    FoldLineSource(FoldLineSource const&) = default;
    FoldLineSource& operator=(FoldLineSource&&) = default;
    FoldLineSource& operator=(FoldLineSource const&) = default;
    virtual ~FoldLineSource() = default;

    /// Whether there is a logical line at @p index — 0 being the one the scan starts at, counting upwards.
    ///
    /// A predicate rather than a count, for the reason its siblings give: counting the logical lines would
    /// walk the whole scrollback before the scan has looked at a single flag.
    [[nodiscard]] virtual bool hasLineAt(size_t index) const = 0;

    /// The flags of the logical line @p index lines ABOVE the starting one (0 being the starting one).
    [[nodiscard]] virtual LineFlags flagsAt(size_t index) const = 0;

    /// The stable id of that LOGICAL line's head. Decreases as @p index grows, the walk going upwards.
    [[nodiscard]] virtual int64_t stableIdAt(size_t index) const = 0;

    /// The stable id of the LAST PHYSICAL row that logical line occupies.
    ///
    /// Equal to stableIdAt() for a line that fits, and larger for one a wrap chopped into several rows.
    /// A fold hides rows, not logical lines, so a range that stopped at the head would leave the
    /// continuations of its bottom-most line on screen -- an orphaned tail below a collapsed block.
    [[nodiscard]] virtual int64_t lastPhysicalStableIdAt(size_t index) const = 0;
};

/// How far a fold scan climbs before giving up.
///
/// The screenful of blocks a user can actually see and act on, not the whole scrollback: the walk runs
/// under the terminal lock, so this is a real bound rather than a defensive one — the same reasoning as
/// MaxPromptScanLines (@see PromptRegion.hpp).
constexpr inline size_t MaxFoldScanLines = 4096;

/// The foldable regions in @p lines, most recent first.
///
/// A backward walk over the line flags a shell's OSC 133 marks leave behind, the exact companion of
/// scanCommandBlocksBackward(): CommandEnd (;D) closes a block, OutputStart (;C) says where its output
/// began, and Marked (;A) begins the prompt the fold hangs off.
///
/// Only FINISHED blocks fold — the walk opens a block on a CommandEnd and on nothing else. That is what
/// keeps the cursor from ever standing inside a collapsed range, and it makes the prompt the user is
/// typing at unfoldable, which is what a user would expect anyway.
///
/// A line carrying Marked is never hidden. A shell's precmd emits ;D and ;A back to back, so the line
/// that closes one command routinely also starts the next prompt; and a shell that emits ;C before the
/// echoed newline lands OutputStart on the command line itself. Both would otherwise fold a prompt away.
/// A block left with no whole output line of its own — `printf hello`, or a command that printed nothing
/// — yields no range: there is nothing to hide.
///
/// A single forward pass: each index is asked about once, in increasing order, and the ids a range needs
/// are taken as the walk passes them rather than looked up again afterwards. That is what lets a source
/// climb the grid lazily and keep no more state than the line it is standing on.
///
/// @param lines The lines to walk, newest first.
/// @param maxScanLines How far up to walk before giving up (@see MaxFoldScanLines).
/// @return The ranges, most recent first; empty when the lines hold no foldable output.
[[nodiscard]] std::vector<FoldRange> computeFoldRanges(FoldLineSource const& lines, size_t maxScanLines);

/// Whether a fold shows its body or hides it.
///
/// The two outcomes a toggle selects between, named rather than left to a bool: `true` at a call site
/// says nothing about which of them it means, and a third state would gain an enumerator here instead
/// of rewriting every signature that reports one.
enum class FoldVisibility : uint8_t
{
    Expanded = 0, ///< The block's output is shown.
    Collapsed,    ///< The block's output is hidden behind its prompt line.
};

/// Which folds are currently collapsed, by the stable id of the prompt line each hangs off.
///
/// Holds only the COLLAPSED ids, not every fold: a fold's existence is re-derived from the marks by
/// computeFoldRanges() on demand, while whether the user collapsed it is the only thing that cannot be
/// re-derived and therefore the only thing worth storing.
class FoldState
{
  public:
    /// Whether the fold hanging off @p headStableId is collapsed.
    [[nodiscard]] bool isCollapsed(int64_t headStableId) const noexcept
    {
        return _collapsed.contains(headStableId);
    }

    /// Whether nothing at all is collapsed — the fast path every caller checks before projecting.
    [[nodiscard]] bool empty() const noexcept { return _collapsed.empty(); }

    /// Bumped by every mutation below.
    ///
    /// What a cache of anything derived from this state keys on: the projection is rebuilt when the user
    /// collapses something, and a counter says so in one comparison rather than by re-deriving it.
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

    void collapse(int64_t headStableId)
    {
        if (_collapsed.insert(headStableId).second)
            ++_revision;
    }

    void expand(int64_t headStableId)
    {
        if (_collapsed.erase(headStableId) != 0)
            ++_revision;
    }

    /// Collapses @p headStableId if it is expanded, expands it otherwise.
    /// @return What it is afterwards.
    FoldVisibility toggle(int64_t headStableId)
    {
        ++_revision;
        if (auto const it = _collapsed.find(headStableId); it != _collapsed.end())
        {
            _collapsed.erase(it);
            return FoldVisibility::Expanded;
        }
        _collapsed.insert(headStableId);
        return FoldVisibility::Collapsed;
    }

    /// Collapses every fold in @p ranges, leaving those already collapsed alone.
    void collapseAll(std::span<FoldRange const> ranges);

    void expandAll() noexcept
    {
        if (!_collapsed.empty())
            ++_revision;
        _collapsed.clear();
    }

    /// Forgets every fold whose head has been evicted from the scrollback.
    ///
    /// Without it the set grows without bound over a long session, and — worse — an id reused by a later
    /// generation would come back collapsed. Cheap because the set is ordered: one erase of a prefix.
    /// @param floorStableId The oldest still-addressable id (Grid::stableRangeFloor()).
    void prune(int64_t floorStableId)
    {
        auto const end = _collapsed.lower_bound(floorStableId);
        if (end != _collapsed.begin())
            ++_revision;
        _collapsed.erase(_collapsed.begin(), end);
    }

    /// Forgets everything, for a generation bump: reflow destroyed row identity wholesale, so every id
    /// held here now names a different row (or none).
    ///
    /// The same operation expandAll() performs, kept under its own name because the two say different
    /// things about WHY the set is emptied -- a user expanding every fold, versus state the grid can no
    /// longer account for.
    void clear() noexcept { expandAll(); }

  private:
    // Ordered rather than hashed, so prune() is a single prefix erase rather than a full scan.
    std::set<int64_t> _collapsed;
    uint64_t _revision = 0;
};

/// What a row's gutter shows.
///
/// A fold occupies a RUN of rows, not just its head, so the gutter draws a column rather than a single
/// marker: the head says which way the block folds, and the rows below it carry the column that says how
/// far it reaches. That run is also the click target, which is why every row of it has a name here.
enum class FoldMarker : uint8_t
{
    None = 0,  ///< No fold touches this row.
    Expanded,  ///< Head of an expanded fold.
    Collapsed, ///< Head of a collapsed fold.
    Body,      ///< A row inside an expanded fold.
    BodyEnd,   ///< The last row of an expanded fold.

    Count, ///< Not a marker; pins the glyph table below to this enum.
};

/// The glyph @p marker is drawn as, or 0 for FoldMarker::None.
///
/// A table rather than a switch, so a sixth marker is a row here and nothing else.
///
/// The head is a box carrying a minus while the block is open and a plus once it is closed -- the fold
/// control a reader has already met in editors and tree views -- and the body continues the column it
/// hangs from down to a corner at the block's last row.
///
/// The two heads are Contour's own private-use codepoints, because no Unicode character carries both
/// the boxed sign and the stem that ties a head to its body (@see
/// docs/vt-extensions/private-use-glyphs.md). All four are drawn by vtrasterizer's own box-drawing
/// renderer rather than by the font, so they are crisp and cell-proportionate in any font.
[[nodiscard]] constexpr char32_t foldMarkerGlyph(FoldMarker marker) noexcept
{
    constexpr auto Glyphs = std::array {
        U'\0',         // None
        U'\U0010F000', // Expanded:  boxed minus with a stem into the body below it
        U'\U0010F001', // Collapsed: boxed plus, with nothing below to connect to
        U'│',          // Body:      BOX DRAWINGS LIGHT VERTICAL
        U'┕',          // BodyEnd:   BOX DRAWINGS UP LIGHT AND RIGHT HEAVY
    };

    // Otherwise a sixth marker without a sixth row is a silent out-of-bounds read rather than the
    // build break the "one row and nothing else" promise above is worth.
    static_assert(Glyphs.size() == static_cast<size_t>(FoldMarker::Count));

    return Glyphs[static_cast<size_t>(marker)];
}

/// What a targeted Vi-mode jump does when its target sits inside a collapsed fold.
///
/// Plain motions never need this -- they are computed in VISIBLE lines and so cannot land inside a fold
/// at all. It governs only the jumps that name a target directly: a search hit, a mark, jump history.
enum class FoldJumpBehavior : uint8_t
{
    Expand = 0, ///< Open the fold so the cursor lands on the real target, as vim's `foldopen` does.
    Skip,       ///< Leave folds alone and snap the cursor to the nearest visible line.
};

/// Which way a fold-aware step or snap searches.
///
/// Named rather than a signed step, purely so the call sites read: `advanceVisibleLines(line, count,
/// VerticalDirection::Up)` says what it does where `advanceVisibleLines(line, count, -1)` does not,
/// and the eight Vi motions that call it are the code most people will read.
enum class VerticalDirection : uint8_t
{
    Up = 0,
    Down,
};

/// An inclusive range of stable ids hidden by a collapsed fold.
struct HiddenInterval
{
    int64_t first {}; ///< Topmost hidden id.
    int64_t last {};  ///< Bottom-most hidden id, inclusive.

    [[nodiscard]] bool operator==(HiddenInterval const&) const noexcept = default;
};

/// The id intervals @p state hides out of @p ranges, sorted ascending and merged where they touch.
///
/// Merged because the projection below binary-searches them: overlapping intervals — two collapsed
/// blocks whose ranges abut — would otherwise need a linear scan per row.
/// @param ranges The foldable ranges.
/// @param state Which of them are collapsed.
/// @return The hidden intervals, ascending and disjoint.
[[nodiscard]] std::vector<HiddenInterval> hiddenIntervals(std::span<FoldRange const> ranges,
                                                          FoldState const& state);

/// The interval in @p hidden covering @p id, if one does.
///
/// One binary search over the ascending, disjoint runs. The building block the two functions below are
/// written in terms of, and the reason they cost the number of runs CROSSED rather than the distance
/// travelled.
///
/// @param hidden The hidden intervals, ascending and disjoint (@see hiddenIntervals).
/// @param id The stable id to look up.
/// @return The covering interval, or nullopt when @p id is visible.
[[nodiscard]] std::optional<HiddenInterval> hidingInterval(std::span<HiddenInterval const> hidden,
                                                           int64_t id) noexcept;

/// The nearest id at or beyond @p id, searching in @p direction, that no interval in @p hidden covers.
///
/// Returns @p id itself when it is already visible. Used to get a cursor OUT of a region that was
/// collapsed underneath it -- a viewport change, a resize, or a jump the user asked to have snapped
/// rather than expanded.
///
/// @param hidden The hidden intervals, ascending and disjoint.
/// @param id The stable id to snap.
/// @param direction Which way to leave a hidden run: Up towards its head, Down past its end.
/// @return The nearest visible id in that direction.
[[nodiscard]] int64_t snapToVisibleId(std::span<HiddenInterval const> hidden,
                                      int64_t id,
                                      VerticalDirection direction) noexcept;

/// The id @p count VISIBLE rows from @p id in @p direction, skipping each hidden run whole.
///
/// This is what makes `3j` mean three rows the user can SEE rather than three grid rows, two of which
/// might be inside a collapsed block. Clamped to [@p floorId, @p ceilId], so a motion that runs off the
/// end of the scrollback stops there rather than naming a row the grid no longer holds.
///
/// Costs one binary search plus one step per hidden run crossed -- not @p count steps -- so a `1000j`
/// over a folded scrollback is no more expensive than a `1j`.
///
/// @param hidden The hidden intervals, ascending and disjoint.
/// @param id The stable id to start from; may itself be hidden, in which case it is snapped first.
/// @param count How many visible rows to travel; zero snaps without moving.
/// @param direction Which way to travel.
/// @param floorId The lowest addressable id (oldest row still in the scrollback).
/// @param ceilId The highest addressable id (the bottom row).
/// @return The id arrived at: within [@p floorId, @p ceilId] and visible, unless every id in that
///         range is hidden. A bound that is itself hidden stops the walk on the last visible id
///         before it rather than on the bound.
[[nodiscard]] int64_t advanceVisibleId(std::span<HiddenInterval const> hidden,
                                       int64_t id,
                                       int64_t count,
                                       VerticalDirection direction,
                                       int64_t floorId,
                                       int64_t ceilId) noexcept;

/// How many VISIBLE ids lie in the half-open range [@p first, @p last).
///
/// The counterpart to advanceVisibleId(): that one travels a distance, this one measures it. Screen
/// rows ARE visible rows, so this is the quantity anything converting a pair of grid positions into a
/// scroll distance or a row difference needs -- their plain difference counts the hidden rows too, and
/// a scroll asked to travel that far overshoots by exactly what the folds hide.
///
/// Costs one binary search plus one step per hidden run crossed, not one per id.
///
/// @param hidden The hidden intervals, ascending and disjoint (@see hiddenIntervals).
/// @param first The first id of the range, counted when visible.
/// @param last The id one past the range's end, never counted.
/// @return The number of visible ids in between; negative, and symmetric, when @p last precedes
///         @p first.
[[nodiscard]] int64_t visibleDistance(std::span<HiddenInterval const> hidden,
                                      int64_t first,
                                      int64_t last) noexcept;

/// The grid lines a folded viewport shows, top row first.
///
/// Plain grid lines, deliberately: a row carries nothing beyond the line it draws. A fold MARKER is not
/// cached here, because the projection is built only when something is COLLAPSED -- an unfolded viewport
/// short-circuits to an empty projection -- while the gutter must also draw the column of an EXPANDED
/// fold, so a marker held here could answer for only half the rows that need one. Terminal::foldMarkerAt()
/// answers for all of them.
///
/// Walks upwards from @p bottom, skipping every line a collapsed fold hides and pulling further history
/// in to fill the rows those hidden lines freed, and stops at @p top. Fewer than @p rowCount rows come
/// back only when the walk ran out of grid — a short scrollback — which the caller pads at the top
/// exactly as an unfolded viewport already shows blank rows above a short history.
///
/// Pure, and deliberately: this is the one function whose disagreement with Viewport's coordinate
/// translation would misplace a selection, so it is the one that has to be testable on its own.
///
/// @param hidden The id runs collapsed folds hide (@see hiddenIntervals), ascending and disjoint.
/// @param stableBase The stable id of grid line 0 (Grid::stableLineIdOf(LineOffset(0))).
/// @param bottom The last visible grid line, inclusive.
/// @param rowCount How many visible rows to fill.
/// @param top The topmost grid line holding valid data (Grid::addressableTop()).
/// @return The rows, top first; empty when @p rowCount is zero or @p bottom is above @p top.
[[nodiscard]] std::vector<LineOffset> projectRows(std::span<HiddenInterval const> hidden,
                                                  int64_t stableBase,
                                                  LineOffset bottom,
                                                  LineCount rowCount,
                                                  LineOffset top);

/// The screen row a projected row list's FIRST row is drawn on.
///
} // namespace vtbackend

template <>
struct std::formatter<vtbackend::FoldJumpBehavior>: formatter<std::string_view>
{
    auto format(vtbackend::FoldJumpBehavior value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::FoldJumpBehavior::Expand: name = "expand"; break;
            case vtbackend::FoldJumpBehavior::Skip: name = "skip"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::FoldMarker>: formatter<std::string_view>
{
    auto format(vtbackend::FoldMarker value, auto& ctx) const
    {
        string_view name;
        switch (value)
        {
            case vtbackend::FoldMarker::None: name = "None"; break;
            case vtbackend::FoldMarker::Expanded: name = "Expanded"; break;
            case vtbackend::FoldMarker::Collapsed: name = "Collapsed"; break;
            case vtbackend::FoldMarker::Body: name = "Body"; break;
            case vtbackend::FoldMarker::BodyEnd: name = "BodyEnd"; break;
            case vtbackend::FoldMarker::Count: name = "Count"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
