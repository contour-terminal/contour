/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2022 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <terminal/CellFlags.h>
#include <terminal/CellUtil.h>
#include <terminal/Color.h>
#include <terminal/ColorPalette.h>
#include <terminal/GraphicsAttributes.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>

#include <unicode/convert.h>
#include <unicode/width.h>

#include <string>
#include <utility>

namespace terminal
{

/// Terminal Cell, optimized for use with the alternate screen.
///
/// This means, only a fixed amount of cells should be living without the need
/// of scrollback buffer and ideally fast access to all relevant properties.
class DenseCell: public CellBase<DenseCell>
{
  public:
    explicit DenseCell(GraphicsAttributes attributes, HyperlinkId hyperlink = {}) noexcept;
    DenseCell() = default;
    ~DenseCell() = default;
    DenseCell(DenseCell&&) = default;
    DenseCell(DenseCell const&) = default;
    DenseCell& operator=(DenseCell&&) = default;
    DenseCell& operator=(DenseCell const&) = default;

    void reset();
    void reset(GraphicsAttributes sgr);
    void reset(GraphicsAttributes sgr, HyperlinkId hyperlink);

    void write(GraphicsAttributes sgr, char32_t codepoint, uint8_t width);
    void write(GraphicsAttributes sgr, char32_t codepoint, uint8_t width, HyperlinkId hyperlink);
    void writeTextOnly(char32_t codepoint, uint8_t width);

    [[nodiscard]] std::u32string codepoints() const noexcept;
    [[nodiscard]] char32_t codepoint(size_t index) const noexcept;
    [[nodiscard]] size_t codepointCount() const noexcept;

    void setCharacter(char32_t codepoint);
    int appendCharacter(char32_t codepoint);

    [[nodiscard]] std::string toUtf8() const;

    [[nodiscard]] uint8_t width() const noexcept;
    void setWidth(uint8_t width) noexcept;

    [[nodiscard]] CellFlags flags() const noexcept;
    [[nodiscard]] bool isFlagEnabled(CellFlags flags) const noexcept;
    void resetFlags() noexcept;
    void resetFlags(CellFlags flags) noexcept;
    void setFlags(CellFlags flags, bool enabled);

    void setGraphicsRendition(GraphicsRendition sgr) noexcept;
    void setForegroundColor(Color color) noexcept;
    void setBackgroundColor(Color color) noexcept;
    void setUnderlineColor(Color color) noexcept;
    [[nodiscard]] Color foregroundColor() const noexcept;
    [[nodiscard]] Color backgroundColor() const noexcept;
    [[nodiscard]] Color underlineColor() const noexcept;
    [[nodiscard]] RGBColorPair makeColors(ColorPalette colorPalette,
                                          bool reverseVideo,
                                          bool blink,
                                          bool rapidBlink) const noexcept;

    [[nodiscard]] RGBColor getUnderlineColor(ColorPalette const& palette,
                                             RGBColor defaultColor) const noexcept;

    [[nodiscard]] std::shared_ptr<ImageFragment> imageFragment() const noexcept;
    void setImageFragment(std::shared_ptr<RasterizedImage> image, CellLocation offset);

    [[nodiscard]] HyperlinkId hyperlink() const noexcept;
    void setHyperlink(HyperlinkId hyperlink) noexcept;

  private:
    std::u32string _codepoints {};
    GraphicsAttributes _graphicsAttributes {};
    CellFlags _flags {};
    uint8_t _width = 1;
    HyperlinkId _hyperlink {};
    std::shared_ptr<ImageFragment> _imageFragment {};
};

// {{{ implementation
inline DenseCell::DenseCell(GraphicsAttributes attributes, HyperlinkId hyperlink) noexcept:
    _graphicsAttributes { attributes }, _hyperlink { hyperlink }
{
}

inline void DenseCell::reset()
{
    *this = {};
}

inline void DenseCell::reset(GraphicsAttributes sgr)
{
    *this = {};
    _graphicsAttributes = sgr;
}

inline void DenseCell::reset(GraphicsAttributes sgr, HyperlinkId hyperlink)
{
    *this = {};
    _graphicsAttributes = sgr;
    _hyperlink = hyperlink;
}

inline void DenseCell::write(GraphicsAttributes sgr, char32_t codepoint, uint8_t width)
{
    _graphicsAttributes = sgr;
    _codepoints.clear();
    _codepoints.push_back(codepoint);

    _width = width;
}

inline void DenseCell::write(GraphicsAttributes sgr,
                             char32_t codepoint,
                             [[maybe_unused]] uint8_t width,
                             [[maybe_unused]] HyperlinkId hyperlink)
{
    _graphicsAttributes = sgr;
    _codepoints.clear();
    _codepoints.push_back(codepoint);
    _width = width;
    _hyperlink = hyperlink;
}

inline void DenseCell::writeTextOnly(char32_t codepoint, uint8_t width)
{
    _codepoints.clear();
    _codepoints.push_back(codepoint);
    _width = width;
}

inline std::u32string DenseCell::codepoints() const noexcept
{
    return _codepoints;
}

inline char32_t DenseCell::codepoint(size_t index) const noexcept
{
    if (index < _codepoints.size())
        return _codepoints[index];
    else
        return 0;
}

inline size_t DenseCell::codepointCount() const noexcept
{
    return _codepoints.size();
}

inline void DenseCell::setCharacter(char32_t codepoint)
{
    _codepoints.clear();
    _imageFragment = {};
    if (codepoint)
    {
        _codepoints.push_back(codepoint);
        setWidth(static_cast<uint8_t>(std::max(unicode::width(codepoint), 1)));
    }
    else
        setWidth(1);
}

inline int DenseCell::appendCharacter(char32_t codepoint)
{
    _codepoints.push_back(codepoint);

    auto const diff = CellUtil::computeWidthChange(*this, codepoint);
    if (diff)
        _width = uint8_t(int(_width) + diff);

    return diff;
}

inline std::string DenseCell::toUtf8() const
{
    return unicode::convert_to<char>(std::u32string_view(_codepoints.data(), _codepoints.size()));
}

inline uint8_t DenseCell::width() const noexcept
{
    return _width;
}

inline void DenseCell::setWidth(uint8_t newWidth) noexcept
{
    _width = newWidth;
}

inline CellFlags DenseCell::flags() const noexcept
{
    return _flags;
}

inline bool DenseCell::isFlagEnabled(CellFlags testFlags) const noexcept
{
    return _flags & testFlags;
}

inline void DenseCell::resetFlags() noexcept
{
    _flags = CellFlags::None;
}

inline void DenseCell::resetFlags(CellFlags flags) noexcept
{
    _flags = flags;
}

inline void DenseCell::setFlags(CellFlags flags, bool enabled)
{
    if (enabled)
        _flags = _flags | flags;
    else
        _flags = CellFlags(unsigned(_flags) & ~unsigned(flags));
}

inline void DenseCell::setGraphicsRendition(GraphicsRendition sgr) noexcept
{
    _flags = CellUtil::makeCellFlags(sgr, _flags);
}

inline void DenseCell::setForegroundColor(Color color) noexcept
{
    _graphicsAttributes.foregroundColor = color;
}

inline void DenseCell::setBackgroundColor(Color color) noexcept
{
    _graphicsAttributes.backgroundColor = color;
}

inline void DenseCell::setUnderlineColor(Color color) noexcept
{
    _graphicsAttributes.underlineColor = color;
}

inline Color DenseCell::foregroundColor() const noexcept
{
    return _graphicsAttributes.foregroundColor;
}

inline Color DenseCell::backgroundColor() const noexcept
{
    return _graphicsAttributes.backgroundColor;
}

inline Color DenseCell::underlineColor() const noexcept
{
    return _graphicsAttributes.underlineColor;
}

inline RGBColorPair DenseCell::makeColors(ColorPalette colorPalette,
                                          bool reverseVideo,
                                          bool blink,
                                          bool rapidBlink) const noexcept
{
    return CellUtil::makeColors(
        colorPalette, flags(), reverseVideo, foregroundColor(), backgroundColor(), blink, rapidBlink);
}

inline RGBColor DenseCell::getUnderlineColor(ColorPalette const& colorPalette,
                                             RGBColor defaultColor) const noexcept
{
    return CellUtil::getUnderlineColor(colorPalette, flags(), defaultColor, underlineColor());
}

inline std::shared_ptr<ImageFragment> DenseCell::imageFragment() const noexcept
{
    return _imageFragment;
}

inline void DenseCell::setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage, CellLocation offset)
{
    _imageFragment = std::make_shared<ImageFragment>(rasterizedImage, offset);
}

inline HyperlinkId DenseCell::hyperlink() const noexcept
{
    return _hyperlink;
}

inline void DenseCell::setHyperlink(HyperlinkId hyperlink) noexcept
{
    _hyperlink = hyperlink;
}

// }}}

// {{{ Optimized version for helpers from CellUtil
namespace CellUtil
{
    inline bool beginsWith(std::u32string_view text, DenseCell const& cell) noexcept
    {
        assert(text.size() != 0);
        return text == cell.codepoints();
    }
} // namespace CellUtil
// }}}

} // namespace terminal
