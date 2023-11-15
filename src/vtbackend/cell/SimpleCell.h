// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>

#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <string>
#include <utility>

namespace vtbackend
{

/// Terminal Cell, optimized for use with the alternate screen.
///
/// This means, only a fixed amount of cells should be living without the need
/// of scrollback buffer and ideally fast access to all relevant properties.
class SimpleCell
{
  public:
    explicit SimpleCell(GraphicsAttributes attributes, HyperlinkId hyperlink = {}) noexcept;
    SimpleCell() = default;
    ~SimpleCell() = default;
    SimpleCell(SimpleCell&&) = default;
    SimpleCell(SimpleCell const&) = default;
    SimpleCell& operator=(SimpleCell&&) = default;
    SimpleCell& operator=(SimpleCell const&) = default;

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
    void resetFlags(CellFlags flags = CellFlag::None) noexcept;

    void setGraphicsRendition(GraphicsRendition sgr) noexcept;
    void setForegroundColor(Color color) noexcept;
    void setBackgroundColor(Color color) noexcept;
    void setUnderlineColor(Color color) noexcept;
    [[nodiscard]] Color foregroundColor() const noexcept;
    [[nodiscard]] Color backgroundColor() const noexcept;
    [[nodiscard]] Color underlineColor() const noexcept;

    [[nodiscard]] std::shared_ptr<ImageFragment> imageFragment() const noexcept;
    void setImageFragment(std::shared_ptr<RasterizedImage> image, CellLocation offset);

    [[nodiscard]] HyperlinkId hyperlink() const noexcept;
    void setHyperlink(HyperlinkId hyperlink) noexcept;

    [[nodiscard]] bool empty() const noexcept { return CellUtil::empty(*this); }

  private:
    std::u32string _codepoints {};
    GraphicsAttributes _graphicsAttributes {};
    uint8_t _width = 1;
    HyperlinkId _hyperlink {};
    std::shared_ptr<ImageFragment> _imageFragment {};
};

// {{{ implementation
inline SimpleCell::SimpleCell(GraphicsAttributes attributes, HyperlinkId hyperlink) noexcept:
    _graphicsAttributes { attributes }, _hyperlink { hyperlink }
{
}

inline void SimpleCell::reset()
{
    *this = {};
}

inline void SimpleCell::reset(GraphicsAttributes sgr)
{
    *this = {};
    _graphicsAttributes = sgr;
}

inline void SimpleCell::reset(GraphicsAttributes sgr, HyperlinkId hyperlink)
{
    *this = {};
    _graphicsAttributes = sgr;
    _hyperlink = hyperlink;
}

inline void SimpleCell::write(GraphicsAttributes sgr, char32_t codepoint, uint8_t width)
{
    _graphicsAttributes = sgr;
    _codepoints.clear();
    _codepoints.push_back(codepoint);

    _width = width;
}

inline void SimpleCell::write(GraphicsAttributes sgr,
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

inline void SimpleCell::writeTextOnly(char32_t codepoint, uint8_t width)
{
    _codepoints.clear();
    _codepoints.push_back(codepoint);
    _width = width;
}

inline std::u32string SimpleCell::codepoints() const noexcept
{
    return _codepoints;
}

inline char32_t SimpleCell::codepoint(size_t index) const noexcept
{
    if (index < _codepoints.size())
        return _codepoints[index];
    else
        return 0;
}

inline size_t SimpleCell::codepointCount() const noexcept
{
    return _codepoints.size();
}

inline void SimpleCell::setCharacter(char32_t codepoint)
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

inline int SimpleCell::appendCharacter(char32_t codepoint)
{
    _codepoints.push_back(codepoint);

    auto const diff = CellUtil::computeWidthChange(*this, codepoint);
    if (diff)
        _width = uint8_t(int(_width) + diff);

    return diff;
}

inline std::string SimpleCell::toUtf8() const
{
    return unicode::convert_to<char>(std::u32string_view(_codepoints.data(), _codepoints.size()));
}

inline uint8_t SimpleCell::width() const noexcept
{
    return _width;
}

inline void SimpleCell::setWidth(uint8_t newWidth) noexcept
{
    _width = newWidth;
}

inline CellFlags SimpleCell::flags() const noexcept
{
    return _graphicsAttributes.flags;
}

inline bool SimpleCell::isFlagEnabled(CellFlags testFlags) const noexcept
{
    return _graphicsAttributes.flags.contains(testFlags);
}

inline void SimpleCell::resetFlags(CellFlags flags) noexcept
{
    _graphicsAttributes.flags = flags;
}

inline void SimpleCell::setGraphicsRendition(GraphicsRendition sgr) noexcept
{
    CellUtil::applyGraphicsRendition(sgr, *this);
}

inline void SimpleCell::setForegroundColor(Color color) noexcept
{
    _graphicsAttributes.foregroundColor = color;
}

inline void SimpleCell::setBackgroundColor(Color color) noexcept
{
    _graphicsAttributes.backgroundColor = color;
}

inline void SimpleCell::setUnderlineColor(Color color) noexcept
{
    _graphicsAttributes.underlineColor = color;
}

inline Color SimpleCell::foregroundColor() const noexcept
{
    return _graphicsAttributes.foregroundColor;
}

inline Color SimpleCell::backgroundColor() const noexcept
{
    return _graphicsAttributes.backgroundColor;
}

inline Color SimpleCell::underlineColor() const noexcept
{
    return _graphicsAttributes.underlineColor;
}

inline std::shared_ptr<ImageFragment> SimpleCell::imageFragment() const noexcept
{
    return _imageFragment;
}

inline void SimpleCell::setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage,
                                         CellLocation offset)
{
    _imageFragment = std::make_shared<ImageFragment>(std::move(rasterizedImage), offset);
}

inline HyperlinkId SimpleCell::hyperlink() const noexcept
{
    return _hyperlink;
}

inline void SimpleCell::setHyperlink(HyperlinkId hyperlink) noexcept
{
    _hyperlink = hyperlink;
}

// }}}

// {{{ Optimized version for helpers from CellUtil
namespace CellUtil
{
    inline bool beginsWith(std::u32string_view text, SimpleCell const& cell) noexcept
    {
        assert(!text.empty());
        return text == cell.codepoints();
    }
} // namespace CellUtil
// }}}

} // namespace vtbackend
