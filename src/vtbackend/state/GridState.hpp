// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/state/ImageRefs.hpp>
#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/state/StateWire.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

namespace vtbackend
{

/// Whether the grid reflows its lines when the page width changes.
enum class ReflowOnResize : uint8_t
{
    No = 0,
    Yes,
};

template <>
struct detail::StateEnumWire<ReflowOnResize>
{
    static constexpr auto Values = std::array { ReflowOnResize::No, ReflowOnResize::Yes };
};

/// The widest line a restore builds: the cell index of an image fragment and of a cluster's pool entry are
/// 16 bits wide, so no line a terminal holds is wider.
inline constexpr auto MaxStateLineColumns = static_cast<int>(std::numeric_limits<uint16_t>::max());

/// Everything a Grid holds, as plain data.
///
/// The lines are kept in their compact wire encoding (@see Line::encodeState) rather than as one more copy of
/// their cells: a capture costs about what the stream does, not what the scrollback does.
struct GridState
{
    PageSize pageSize {};                       ///< The main page.
    ReflowOnResize reflow = ReflowOnResize::No; ///< Whether a width change reflows.
    HistoryLimits limits {};                    ///< The scrollback's bounds.
    LineCount linesUsed {};                     ///< History plus page rows.

    uint64_t generation = 0;              ///< @see Grid::generation
    uint64_t stableIdGeneration = 0;      ///< @see Grid::stableIdGeneration
    int64_t stableBase = 0;               ///< Stable id of page row 0.
    int64_t stableFloor = 0;              ///< Oldest addressable stable id.
    uint64_t evictedRowCount = 0;         ///< @see Grid::evictedRowCount
    int64_t dirtyHistoryFloor = 0;        ///< The lowest history id announced since the last finalize.
    int64_t changedHistoryFloor = 0;      ///< The lowest history id a finalize stamped.
    uint64_t changedHistorySeqno = 0;     ///< The seqno it stamped at.
    uint64_t seqno = 0;                   ///< The batch counter revisions draw from.
    int64_t stableBaseAtLastFinalize = 0; ///< Bounds the next finalize scan.

    /// Every rendition a cell of @ref lines wears, once: the lines name renditions by index into it.
    std::vector<GraphicsAttributes> renditions;

    /// The lines, history then page, oldest history first: one frame each in the compact line encoding.
    std::vector<uint8_t> lines;

    bool operator==(GridState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.pageSize);
        visitor(2, self.reflow);
        visitor(3, self.limits);
        visitor(4, self.linesUsed);
        visitor(5, self.generation);
        visitor(6, self.stableIdGeneration);
        visitor(7, self.stableBase);
        visitor(8, self.stableFloor);
        visitor(9, self.evictedRowCount);
        visitor(10, self.dirtyHistoryFloor);
        visitor(11, self.changedHistoryFloor);
        visitor(12, self.changedHistorySeqno);
        visitor(13, self.seqno);
        visitor(14, self.stableBaseAtLastFinalize);
        visitor(15, self.renditions);
        visitor(16, self.lines);
    }
};

/// Files the renditions the lines of one grid wear, each once, as a capture meets them.
class RenditionTableBuilder
{
  public:
    /// @param rendition A cell's rendition.
    /// @return Its index in the table.
    [[nodiscard]] uint64_t indexOf(GraphicsAttributes const& rendition);

    /// @return The table, in the order the renditions were met.
    [[nodiscard]] std::vector<GraphicsAttributes> finish() && { return std::move(_table); }

  private:
    struct Hash
    {
        [[nodiscard]] size_t operator()(GraphicsAttributes const& rendition) const noexcept;
    };
    std::vector<GraphicsAttributes> _table;
    std::unordered_map<GraphicsAttributes, uint64_t, Hash> _indexOf;
};

/// What encoding the lines of one grid shares.
struct LineEncodeContext
{
    RenditionTableBuilder& renditions; ///< Where the renditions are filed.
    ImageFragmentNamer& images;        ///< Where the image fragments are filed.
};

/// What decoding the lines of one grid shares.
struct LineDecodeContext
{
    std::span<GraphicsAttributes const> renditions; ///< The grid's rendition table.
    ImageFragmentResolver const& images;            ///< Resolves the image fragments' ids.

    /// How many more cells the lines may materialize. A line of a few bytes may ask for tens of thousands of
    /// cells, so the grid's lines share a budget: what a grid of page-wide lines holds, and some slack.
    size_t& cellBudget;
};

/// A grid built from its captured state and not yet put in place: what Grid::stageRestore() checked and
/// built, for Grid::commitRestore() to swap in without anything left that can fail.
struct StagedGrid
{
    GridState const* state = nullptr; ///< The state it was built from; outlives the staged grid.
    std::vector<Line> ringLines;      ///< The ring's slots, laid out as Grid::commitRestore() expects.
};

} // namespace vtbackend
