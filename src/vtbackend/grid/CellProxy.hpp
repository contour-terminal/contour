// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/grid/LineSoA.hpp>
#include <vtbackend/grid/SoAClusterWriter.hpp>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <memory>
#include <string>
#include <type_traits>

namespace vtbackend
{

/// Transient proxy into SoA arrays that provides the same interface as CompactCell.
///
/// BasicCellProxy is parameterized on const-ness:
///   - BasicCellProxy<true>  (ConstCellProxy) provides read-only access via LineSoA const*.
///   - BasicCellProxy<false> (CellProxy)      provides read-write access via LineSoA*.
///
/// Both are lightweight value types (pointer + index) that read and write
/// into LineSoA arrays. They must NEVER be stored across operations that could
/// reallocate the underlying arrays (e.g., line resize, scroll).
///
/// Size: 2 words = 16 bytes on 64-bit. Cheap to copy and pass by value.
template <bool IsConst>
class BasicCellProxy
{
  public:
    static constexpr uint8_t MaxCodepoints = MaxGraphemeClusterSize;

    using LineType = std::conditional_t<IsConst, LineSoA const, LineSoA>;

    BasicCellProxy(LineType& line, size_t col, bool* dirty = nullptr) noexcept:
        _line(&line), _col(col), _dirty(dirty)
    {
        // CellProxy must never be constructed on a blank (un-materialized) line:
        // every accessor below dereferences the SoA arrays. Callers must materialize
        // first (mutable proxy) or guard with isBlank() (const proxy).
        assert(!line.codepoints.empty());
    }

    /// Implicit conversion from mutable proxy to const proxy.
    operator BasicCellProxy<true>() const noexcept
        requires(!IsConst)
    {
        return BasicCellProxy<true>(*_line, _col);
    }

    // -- Read interface (matches CompactCell API) ---------------------------------

    [[nodiscard]] char32_t codepoint(size_t i = 0) const noexcept
    {
        if (i == 0)
            return _line->codepoints[_col];

        if (_line->clusterSize[_col] <= 1 || i >= _line->clusterSize[_col])
            return 0;

        auto const poolStart = _line->clusterPoolIndex[_col];
        return _line->clusterPool[poolStart + i - 1];
    }

    [[nodiscard]] char32_t operator[](size_t i) const noexcept { return codepoint(i); }

    [[nodiscard]] size_t codepointCount() const noexcept { return _line->clusterSize[_col]; }
    [[nodiscard]] size_t size() const noexcept { return codepointCount(); }

    [[nodiscard]] std::u32string codepoints() const
    {
        std::u32string result;
        result.reserve(codepointCount());
        forEachCodepoint(*_line, _col, [&](char32_t cp) { result.push_back(cp); });
        return result;
    }

    [[nodiscard]] uint8_t width() const noexcept { return _line->widths[_col]; }

    /// Per-cell vertical scale in cells (kitty text sizing protocol, `OSC 66` `s=`); 1 for ordinary
    /// text.
    [[nodiscard]] uint8_t scale() const noexcept { return _line->scales[_col]; }

    /// The full sizing of this cell: the block scale plus the fraction and alignment.
    [[nodiscard]] CellScale textScale() const noexcept
    {
        return unpackTextScale(_line->scales[_col], _line->textScaleExtras[_col]);
    }

    [[nodiscard]] CellFlags flags() const noexcept { return _line->sgr[_col].flags; }

    [[nodiscard]] bool isFlagEnabled(CellFlags testFlags) const noexcept
    {
        return flags().contains(testFlags);
    }

    [[nodiscard]] Color foregroundColor() const noexcept { return _line->sgr[_col].foregroundColor; }
    [[nodiscard]] Color backgroundColor() const noexcept { return _line->sgr[_col].backgroundColor; }
    [[nodiscard]] Color underlineColor() const noexcept { return _line->sgr[_col].underlineColor; }

    [[nodiscard]] HyperlinkId hyperlink() const noexcept { return _line->hyperlinks[_col]; }

    /// The cell's full graphics rendition.
    ///
    /// Needed when a cell must be styled to match ANOTHER cell rather than the current pen -- a wide
    /// cluster's continuation cells, for instance, whose pen may have moved on between the base
    /// codepoint and the variation selector that widened the cluster.
    [[nodiscard]] GraphicsAttributes graphicsAttributes() const noexcept { return _line->sgr[_col]; }

    [[nodiscard]] std::shared_ptr<ImageFragment> imageFragment() const noexcept
    {
        if (_line->imageFragments)
        {
            if (auto const it = _line->imageFragments->find(static_cast<uint16_t>(_col));
                it != _line->imageFragments->end())
                return it->second;
        }
        return {};
    }

    [[nodiscard]] bool empty() const noexcept { return _line->codepoints[_col] == 0 && !imageFragment(); }

    [[nodiscard]] std::string toUtf8() const
    {
        std::string result;
        forEachCodepoint(*_line, _col, [&](char32_t cp) {
            unicode::convert_to<char>(std::u32string_view(&cp, 1), std::back_inserter(result));
        });
        return result;
    }

    // -- Write interface (only available on mutable proxy) ------------------------

    void write(GraphicsAttributes const& attrs, char32_t ch, uint8_t cellWidth) noexcept
        requires(!IsConst)
    {
        markDirty();
        writeCellToSoA(*_line, _col, ch, cellWidth, attrs);
    }

    void write(GraphicsAttributes const& attrs,
               char32_t ch,
               uint8_t cellWidth,
               HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        markDirty();
        writeCellToSoA(*_line, _col, ch, cellWidth, attrs, hyperlinkId);
    }

    void writeTextOnly(char32_t ch, uint8_t cellWidth) noexcept
        requires(!IsConst)
    {
        markDirty();
        auto const oldClusterSize = _line->clusterSize[_col];
        _line->codepoints[_col] = ch;
        _line->widths[_col] = cellWidth;
        // writeCellToSoA sees the flags that say whether a continuation follows a wide head; this entry
        // point cannot, so it assumes one.
        if (cellWidth > 1)
            degradeShape(*_line, LineShape::Spanning);
        _line->clusterSize[_col] = (ch != 0) ? uint8_t { 1 } : uint8_t { 0 };
        if (oldClusterSize > 1)
            clearClusterExtras(*_line, _col);
    }

    void reset() noexcept
        requires(!IsConst)
    {
        resetCell(GraphicsAttributes {}, HyperlinkId {});
        // An empty, ordinary-sized cell in the default pen cannot span; it can only mix the line.
        degradeToMixedIfNeeded();
    }

    void reset(GraphicsAttributes const& attrs) noexcept
        requires(!IsConst)
    {
        resetCell(attrs, HyperlinkId {});
        // The flags can carry a continuation: Screen::clearAndAdvance writes a wide character's this way.
        degradeShapeIfNeeded();
    }

    void reset(GraphicsAttributes const& attrs, HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        resetCell(attrs, hyperlinkId);
        degradeShapeIfNeeded();
    }

    [[nodiscard]] int appendCharacter(char32_t ch,
                                      ClusterWidthPolicy policy = ClusterWidthPolicy::ClusterAware) noexcept
        requires(!IsConst)
    {
        markDirty();
        return appendCodepointToCluster(*_line, _col, ch, policy);
    }

    void setCharacter(char32_t ch) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->codepoints[_col] = ch;
        clearClusterExtras(*_line, _col);
        _line->clusterSize[_col] = (ch != 0) ? uint8_t { 1 } : uint8_t { 0 };
        if (ch)
            _line->widths[_col] = static_cast<uint8_t>(std::max(1u, unicode::width(ch)));
        else
            _line->widths[_col] = 1;
        _line->scales[_col] = 1;
        _line->textScaleExtras[_col] = 0;

        clearReplacedImageFragment(_line->imageFragments, static_cast<uint16_t>(_col));
    }

    /// Sets the per-cell vertical scale (kitty text sizing protocol).
    void setScale(uint8_t s) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->scales[_col] = s;
        degradeShapeIfNeeded();
    }

    /// Sets the whole sizing of this cell: the block scale plus the fraction and alignment.
    void setTextScale(CellScale const& cellScale) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->scales[_col] = cellScale.scale;
        _line->textScaleExtras[_col] = packTextScaleExtras(cellScale);
        degradeShapeForTextScale(*_line, cellScale);
        degradeShapeIfNeeded();
    }

    void setWidth(uint8_t w) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->widths[_col] = w;
        // writeCellToSoA sees the flags that say whether a continuation follows a wide head; this entry
        // point cannot, so it assumes one.
        if (w > 1)
            degradeShape(*_line, LineShape::Spanning);
    }

    void resetFlags() noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->sgr[_col].flags = CellFlag::None;
        degradeShapeIfNeeded();
    }

    void resetFlags(CellFlags f) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->sgr[_col].flags = f;
        degradeShapeIfNeeded();
    }

    void setForegroundColor(Color c) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->sgr[_col].foregroundColor = c;
        degradeToMixedIfNeeded();
    }

    void setBackgroundColor(Color c) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->sgr[_col].backgroundColor = c;
        degradeToMixedIfNeeded();
    }

    void setUnderlineColor(Color c) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->sgr[_col].underlineColor = c;
        degradeToMixedIfNeeded();
    }

    void setHyperlink(HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->hyperlinks[_col] = hyperlinkId;
        degradeToMixedIfNeeded();
    }

    void setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage, CellLocation offset)
        requires(!IsConst)
    {
        markDirty();
        if (!_line->imageFragments)
            _line->imageFragments.emplace();
        (*_line->imageFragments)[static_cast<uint16_t>(_col)] =
            std::make_shared<ImageFragment>(std::move(rasterizedImage), offset);
        // Per-cell rendering (RenderLine has no image support), never bulk-written.
        degradeShape(*_line, LineShape::Spanning);
    }

    /// Removes this cell's image fragment while leaving its text and rendition untouched.
    ///
    /// Deleting an image *placement* is not the same as clearing the cell: the kitty graphics
    /// protocol lets an application drop a placement and keep whatever text shares those cells.
    void clearImageFragment() noexcept
        requires(!IsConst)
    {
        markDirty();
        if (_line->imageFragments)
            _line->imageFragments->erase(static_cast<uint16_t>(_col));
    }

    void setGraphicsRendition(GraphicsRendition sgr) noexcept
        requires(!IsConst)
    {
        markDirty();
        _line->sgr[_col].flags = CellUtil::makeCellFlags(sgr, _line->sgr[_col].flags);
        degradeShapeIfNeeded();
    }

  private:
    void markDirty() noexcept
        requires(!IsConst)
    {
        if (_dirty)
            *_dirty = true;
    }

    /// Clears the cell to an empty one wearing @p attrs and @p hyperlinkId. Leaves the line's shape to
    /// the caller, which knows whether the cell can span.
    void resetCell(GraphicsAttributes const& attrs, HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        markDirty();
        auto const oldClusterSize = _line->clusterSize[_col];
        _line->codepoints[_col] = 0;
        _line->widths[_col] = 1;
        _line->scales[_col] = 1;
        _line->textScaleExtras[_col] = 0;
        _line->sgr[_col] = attrs;
        _line->hyperlinks[_col] = hyperlinkId;
        _line->clusterSize[_col] = 0;
        if (oldClusterSize > 1)
            clearClusterExtras(*_line, _col);
        if (_line->imageFragments)
            _line->imageFragments->erase(static_cast<uint16_t>(_col));
    }

    /// Degrades the line's shape to what this cell now requires. @see degradeShapeForCell
    void degradeShapeIfNeeded() noexcept
        requires(!IsConst)
    {
        degradeShapeForCell(*_line, _col);
    }

    /// Degrades a Uniform line to Mixed when this cell's rendition or hyperlink differs from the rest.
    /// Enough on its own after a write that cannot make the cell span: a colour or a hyperlink.
    /// @see degradeToMixedIfDiffers
    void degradeToMixedIfNeeded() noexcept
        requires(!IsConst)
    {
        degradeToMixedIfDiffers(*_line, _col);
    }

    LineType* _line;
    size_t _col;
    bool* _dirty = nullptr; ///< Optional back-pointer to Line::_dirty for write-through dirtying.
};

/// Mutable proxy — read-write access to a LineSoA cell.
using CellProxy = BasicCellProxy<false>;

/// Const proxy — read-only access to a LineSoA cell.
using ConstCellProxy = BasicCellProxy<true>;

/// Convenience alias: Cell is now CellProxy (was CompactCell).
using Cell = CellProxy;

/// Extract a GraphicsAttributes from a cell proxy (avoids repeated field-by-field extraction).
template <bool IsConst>
[[nodiscard]] inline GraphicsAttributes extractAttributes(BasicCellProxy<IsConst> const& proxy) noexcept
{
    return GraphicsAttributes { .foregroundColor = proxy.foregroundColor(),
                                .backgroundColor = proxy.backgroundColor(),
                                .underlineColor = proxy.underlineColor(),
                                .flags = proxy.flags() };
}

/// Test if a u32string_view starts with the codepoints of a cell proxy.
template <bool IsConst>
[[nodiscard]] inline bool beginsWith(std::u32string_view text, BasicCellProxy<IsConst> const& cell) noexcept
{
    if (cell.codepointCount() == 0)
        return false;

    if (text.size() < cell.codepointCount())
        return false;

    for (size_t i = 0; i < cell.codepointCount(); ++i)
        if (cell.codepoint(i) != text[i])
            return false;

    return true;
}

} // namespace vtbackend
