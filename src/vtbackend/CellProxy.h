// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Color.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/LineSoA.h>
#include <vtbackend/SoAClusterWriter.h>
#include <vtbackend/primitives.h>

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

    BasicCellProxy(LineType& line, size_t col) noexcept: _line(&line), _col(col) {}

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

    [[nodiscard]] CellFlags flags() const noexcept { return _line->sgr[_col].flags; }

    [[nodiscard]] bool isFlagEnabled(CellFlags testFlags) const noexcept
    {
        return flags().contains(testFlags);
    }

    [[nodiscard]] Color foregroundColor() const noexcept { return _line->sgr[_col].foregroundColor; }
    [[nodiscard]] Color backgroundColor() const noexcept { return _line->sgr[_col].backgroundColor; }
    [[nodiscard]] Color underlineColor() const noexcept { return _line->sgr[_col].underlineColor; }

    [[nodiscard]] HyperlinkId hyperlink() const noexcept { return _line->hyperlinks[_col]; }

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
        writeCellToSoA(*_line, _col, ch, cellWidth, attrs);
    }

    void write(GraphicsAttributes const& attrs,
               char32_t ch,
               uint8_t cellWidth,
               HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        writeCellToSoA(*_line, _col, ch, cellWidth, attrs, hyperlinkId);
    }

    void writeTextOnly(char32_t ch, uint8_t cellWidth) noexcept
        requires(!IsConst)
    {
        auto const oldClusterSize = _line->clusterSize[_col];
        _line->codepoints[_col] = ch;
        _line->widths[_col] = cellWidth;
        _line->clusterSize[_col] = (ch != 0) ? uint8_t { 1 } : uint8_t { 0 };
        if (oldClusterSize > 1)
            clearClusterExtras(*_line, _col);
    }

    void reset() noexcept
        requires(!IsConst)
    {
        auto const oldClusterSize = _line->clusterSize[_col];
        _line->codepoints[_col] = 0;
        _line->widths[_col] = 1;
        _line->sgr[_col] = GraphicsAttributes {};
        _line->hyperlinks[_col] = {};
        _line->clusterSize[_col] = 0;
        if (oldClusterSize > 1)
            clearClusterExtras(*_line, _col);
        if (_line->imageFragments)
            _line->imageFragments->erase(static_cast<uint16_t>(_col));
    }

    void reset(GraphicsAttributes const& attrs) noexcept
        requires(!IsConst)
    {
        auto const oldClusterSize = _line->clusterSize[_col];
        _line->codepoints[_col] = 0;
        _line->widths[_col] = 1;
        _line->sgr[_col] = attrs;
        _line->hyperlinks[_col] = {};
        _line->clusterSize[_col] = 0;
        if (oldClusterSize > 1)
            clearClusterExtras(*_line, _col);
        if (_line->imageFragments)
            _line->imageFragments->erase(static_cast<uint16_t>(_col));
        invalidateTrivialIfNeeded();
    }

    void reset(GraphicsAttributes const& attrs, HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        reset(attrs);
        _line->hyperlinks[_col] = hyperlinkId;
        invalidateTrivialIfNeeded();
    }

    [[nodiscard]] int appendCharacter(char32_t ch) noexcept
        requires(!IsConst)
    {
        return appendCodepointToCluster(*_line, _col, ch);
    }

    void setCharacter(char32_t ch) noexcept
        requires(!IsConst)
    {
        _line->codepoints[_col] = ch;
        clearClusterExtras(*_line, _col);
        _line->clusterSize[_col] = (ch != 0) ? uint8_t { 1 } : uint8_t { 0 };
        if (ch)
            _line->widths[_col] = static_cast<uint8_t>(std::max(1u, unicode::width(ch)));
        else
            _line->widths[_col] = 1;

        clearReplacedImageFragment(_line->imageFragments, static_cast<uint16_t>(_col));
    }

    void setWidth(uint8_t w) noexcept
        requires(!IsConst)
    {
        _line->widths[_col] = w;
    }

    void resetFlags() noexcept
        requires(!IsConst)
    {
        _line->sgr[_col].flags = CellFlag::None;
        invalidateTrivialIfNeeded();
    }

    void resetFlags(CellFlags f) noexcept
        requires(!IsConst)
    {
        _line->sgr[_col].flags = f;
        invalidateTrivialIfNeeded();
    }

    void setForegroundColor(Color c) noexcept
        requires(!IsConst)
    {
        _line->sgr[_col].foregroundColor = c;
        invalidateTrivialIfNeeded();
    }

    void setBackgroundColor(Color c) noexcept
        requires(!IsConst)
    {
        _line->sgr[_col].backgroundColor = c;
        invalidateTrivialIfNeeded();
    }

    void setUnderlineColor(Color c) noexcept
        requires(!IsConst)
    {
        _line->sgr[_col].underlineColor = c;
        invalidateTrivialIfNeeded();
    }

    void setHyperlink(HyperlinkId hyperlinkId) noexcept
        requires(!IsConst)
    {
        _line->hyperlinks[_col] = hyperlinkId;
        invalidateTrivialIfNeeded();
    }

    void setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage, CellLocation offset)
        requires(!IsConst)
    {
        if (!_line->imageFragments)
            _line->imageFragments.emplace();
        (*_line->imageFragments)[static_cast<uint16_t>(_col)] =
            std::make_shared<ImageFragment>(std::move(rasterizedImage), offset);
        _line->trivial = false; // Images require per-cell rendering (RenderLine has no image support)
    }

    void setGraphicsRendition(GraphicsRendition sgr) noexcept
        requires(!IsConst)
    {
        _line->sgr[_col].flags = CellUtil::makeCellFlags(sgr, _line->sgr[_col].flags);
        invalidateTrivialIfNeeded();
    }

  private:
    /// Invalidate the trivial flag if this cell's SGR or hyperlink differs from another cell.
    /// When at col 0, compare against col 1 (an unwritten cell).
    /// When at col > 0, compare against col 0.
    void invalidateTrivialIfNeeded() noexcept
        requires(!IsConst)
    {
        if (_line->trivial)
        {
            auto const checkCol = (_col > 0) ? size_t(0) : size_t(1);
            if (checkCol < _line->codepoints.size()
                && (_line->sgr[_col] != _line->sgr[checkCol]
                    || _line->hyperlinks[_col] != _line->hyperlinks[checkCol]))
            {
                _line->trivial = false;
            }
        }
    }

    LineType* _line;
    size_t _col;
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
