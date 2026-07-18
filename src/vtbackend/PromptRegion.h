// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/LineFlags.h>
#include <vtbackend/primitives.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace vtbackend
{

/// The semantic marks a prompt-region scan reads off one LOGICAL line.
struct LogicalLineMarks
{
    LineFlags flags;                  ///< The line's flags.
    ColumnOffset promptEndOffset {};  ///< Meaningful only with LineFlag::PromptEnd.
    ColumnOffset commandEndOffset {}; ///< Meaningful only with LineFlag::CommandEnd.
};

/// Supplies the LOGICAL lines a live-prompt scan walks, indexed backwards from the cursor's line.
///
/// The dependency-injection seam, mirroring CommandBlockLineSource: the scan below is a pure function of
/// the marks it is handed, so it can be exercised against a plain vector — with no Grid, no Screen and no
/// terminal behind it.
///
/// Logical, not physical, for the same reason: the shell's marks name the line it wrote, and a window
/// resize re-chops the physical pieces without moving a single mark.
class PromptRegionLineSource
{
  public:
    PromptRegionLineSource() = default;
    PromptRegionLineSource(PromptRegionLineSource&&) = default;
    PromptRegionLineSource(PromptRegionLineSource const&) = default;
    PromptRegionLineSource& operator=(PromptRegionLineSource&&) = default;
    PromptRegionLineSource& operator=(PromptRegionLineSource const&) = default;
    virtual ~PromptRegionLineSource() = default;

    /// Whether there is a logical line at @p index — 0 being the cursor's, counting upwards.
    ///
    /// A predicate rather than a count, for the reason CommandBlockLineSource gives: counting would walk
    /// the whole scrollback before the scan has looked at a single flag.
    [[nodiscard]] virtual bool hasLineAt(size_t index) const = 0;

    /// The marks of the logical line @p index lines ABOVE the cursor's (0 being the cursor's).
    [[nodiscard]] virtual LogicalLineMarks marksAt(size_t index) const = 0;
};

/// Where the shell's live prompt sits, in LOGICAL lines counted upwards from the cursor's line.
struct PromptRegion
{
    /// How far above the cursor's line the prompt began (the line carrying OSC 133;A).
    size_t startIndex = 0;

    /// The logical column at which the user's input begins — where OSC 133;B landed.
    ///
    /// Absent when the shell marks prompt starts but not prompt ends: plenty of setups emit only ;A. The
    /// region is still perfectly usable then, it simply cannot separate the prompt's own text from what
    /// the user has typed into it.
    std::optional<ColumnOffset> inputBegin {};

    /// Which logical line @ref inputBegin is a column of. Meaningless when that is absent.
    size_t inputBeginIndex = 0;
};

/// Why there is no live prompt to report.
enum class PromptRegionError : uint8_t
{
    /// No semantic marks at all within reach: this shell has no OSC 133 integration. A caller may
    /// reasonably stop asking.
    NoPromptMark,

    /// A command is running: OSC 133;C sits between the prompt and the cursor, so the cursor is standing
    /// in that command's output rather than in a prompt.
    InCommandOutput,

    /// The prompt exists but could not be reached — it scrolled out of the scrollback, or the scan hit
    /// its budget. Transient: worth asking again after the next cursor move.
    OutOfReach,
};

/// The live (unfinished) prompt the cursor is standing in.
///
/// The exact complement of scanCommandBlocksBackward(): that one reconstructs FINISHED blocks, which a
/// CommandEnd opens — so it can never see the prompt the user is typing at right now, which by definition
/// has no CommandEnd yet. This one is anchored at the cursor, requires the block to still be OPEN, and
/// returns POSITIONS rather than text.
///
/// The walk starts at the cursor's own logical line and climbs: an OutputStart reached before any prompt
/// start means a command is running; a PromptEnd records the prompt/input border; a Marked ends the walk
/// successfully.
///
/// @param lines The logical lines, the cursor's first.
/// @param maxScanLines How far up to look before giving up. A prompt is a handful of lines, and the walk
///                     runs under the terminal lock, so this bound is load-bearing rather than defensive.
/// @return The region, or why there is none.
[[nodiscard]] std::expected<PromptRegion, PromptRegionError> findLivePromptRegion(
    PromptRegionLineSource const& lines, size_t maxScanLines);

/// How far a live-prompt scan climbs before giving up.
///
/// A prompt is a handful of lines even when it is a lavish multi-line one, and the scan runs under the
/// terminal lock on the GUI thread, so this is a real bound rather than a defensive one.
constexpr inline size_t MaxPromptScanLines = 64;

/// Where the shell's live prompt sits in the GRID: physical lines, and the column input begins at.
///
/// The grid-coordinate form of @ref PromptRegion, which deals only in logical-line indices.
struct LivePromptSpan
{
    LineOffset firstLine;                   ///< The first physical line of the prompt.
    LineOffset lastLine;                    ///< Its last physical line.
    std::optional<ColumnOffset> inputBegin; ///< Logical column where the user's input begins, if known.
};

} // namespace vtbackend
