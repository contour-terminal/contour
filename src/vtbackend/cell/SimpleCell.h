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

#include <vtbackend/CellFlags.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>

#include <string>
#include <utility>

#include <libunicode/convert.h>
#include <libunicode/width.h>

namespace terminal
{

/// Terminal Cell, optimized for use with the alternate screen.
///
/// This means, only a fixed amount of cells should be living without the need
/// of scrollback buffer and ideally fast access to all relevant properties.
class simple_cell
{
  public:
    explicit simple_cell(graphics_attributes attributes, hyperlink_id hyperlink = {}) noexcept;
    simple_cell() = default;
    ~simple_cell() = default;
    simple_cell(simple_cell&&) = default;
    simple_cell(simple_cell const&) = default;
    simple_cell& operator=(simple_cell&&) = default;
    simple_cell& operator=(simple_cell const&) = default;

    void reset();
    void reset(graphics_attributes sgr);
    void reset(graphics_attributes sgr, hyperlink_id hyperlink);

    void write(graphics_attributes sgr, char32_t codepoint, uint8_t width);
    void write(graphics_attributes sgr, char32_t codepoint, uint8_t width, hyperlink_id hyperlink);
    void writeTextOnly(char32_t codepoint, uint8_t width);

    [[nodiscard]] std::u32string codepoints() const noexcept;
    [[nodiscard]] char32_t codepoint(size_t index) const noexcept;
    [[nodiscard]] size_t codepointCount() const noexcept;

    void setCharacter(char32_t codepoint);
    int appendCharacter(char32_t codepoint);

    [[nodiscard]] std::string toUtf8() const;

    [[nodiscard]] uint8_t width() const noexcept;
    void setWidth(uint8_t width) noexcept;

    [[nodiscard]] cell_flags flags() const noexcept;
    [[nodiscard]] bool isFlagEnabled(cell_flags flags) const noexcept;
    void resetFlags(cell_flags flags = cell_flags::None) noexcept;

    void setGraphicsRendition(graphics_rendition sgr) noexcept;
    void setForegroundColor(color color) noexcept;
    void setBackgroundColor(color color) noexcept;
    void setUnderlineColor(color color) noexcept;
    [[nodiscard]] color foregroundColor() const noexcept;
    [[nodiscard]] color backgroundColor() const noexcept;
    [[nodiscard]] color underlineColor() const noexcept;

    [[nodiscard]] std::shared_ptr<image_fragment> imageFragment() const noexcept;
    void setImageFragment(std::shared_ptr<rasterized_image> image, cell_location offset);

    [[nodiscard]] hyperlink_id hyperlink() const noexcept;
    void setHyperlink(hyperlink_id hyperlink) noexcept;

    [[nodiscard]] bool empty() const noexcept { return CellUtil::empty(*this); }

  private:
    std::u32string _codepoints {};
    graphics_attributes _graphicsAttributes {};
    cell_flags _flags {};
    uint8_t _width = 1;
    hyperlink_id _hyperlink {};
    std::shared_ptr<image_fragment> _imageFragment {};
};

// {{{ implementation
inline simple_cell::simple_cell(graphics_attributes attributes, hyperlink_id hyperlink) noexcept:
    _graphicsAttributes { attributes }, _hyperlink { hyperlink }
{
}

inline void simple_cell::reset()
{
    *this = {};
}

inline void simple_cell::reset(graphics_attributes sgr)
{
    *this = {};
    _graphicsAttributes = sgr;
}

inline void simple_cell::reset(graphics_attributes sgr, hyperlink_id hyperlink)
{
    *this = {};
    _graphicsAttributes = sgr;
    _hyperlink = hyperlink;
}

inline void simple_cell::write(graphics_attributes sgr, char32_t codepoint, uint8_t width)
{
    _graphicsAttributes = sgr;
    _codepoints.clear();
    _codepoints.push_back(codepoint);

    _width = width;
}

inline void simple_cell::write(graphics_attributes sgr,
                               char32_t codepoint,
                               [[maybe_unused]] uint8_t width,
                               [[maybe_unused]] hyperlink_id hyperlink)
{
    _graphicsAttributes = sgr;
    _codepoints.clear();
    _codepoints.push_back(codepoint);
    _width = width;
    _hyperlink = hyperlink;
}

inline void simple_cell::writeTextOnly(char32_t codepoint, uint8_t width)
{
    _codepoints.clear();
    _codepoints.push_back(codepoint);
    _width = width;
}

inline std::u32string simple_cell::codepoints() const noexcept
{
    return _codepoints;
}

inline char32_t simple_cell::codepoint(size_t index) const noexcept
{
    if (index < _codepoints.size())
        return _codepoints[index];
    else
        return 0;
}

inline size_t simple_cell::codepointCount() const noexcept
{
    return _codepoints.size();
}

inline void simple_cell::setCharacter(char32_t codepoint)
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

inline int simple_cell::appendCharacter(char32_t codepoint)
{
    _codepoints.push_back(codepoint);

    auto const diff = CellUtil::computeWidthChange(*this, codepoint);
    if (diff)
        _width = uint8_t(int(_width) + diff);

    return diff;
}

inline std::string simple_cell::toUtf8() const
{
    return unicode::convert_to<char>(std::u32string_view(_codepoints.data(), _codepoints.size()));
}

inline uint8_t simple_cell::width() const noexcept
{
    return _width;
}

inline void simple_cell::setWidth(uint8_t newWidth) noexcept
{
    _width = newWidth;
}

inline cell_flags simple_cell::flags() const noexcept
{
    return _flags;
}

inline bool simple_cell::isFlagEnabled(cell_flags testFlags) const noexcept
{
    return _flags & testFlags;
}

inline void simple_cell::resetFlags(cell_flags flags) noexcept
{
    _flags = flags;
}

inline void simple_cell::setGraphicsRendition(graphics_rendition sgr) noexcept
{
    CellUtil::applyGraphicsRendition(sgr, *this);
}

inline void simple_cell::setForegroundColor(color color) noexcept
{
    _graphicsAttributes.foregroundColor = color;
}

inline void simple_cell::setBackgroundColor(color color) noexcept
{
    _graphicsAttributes.backgroundColor = color;
}

inline void simple_cell::setUnderlineColor(color color) noexcept
{
    _graphicsAttributes.underlineColor = color;
}

inline color simple_cell::foregroundColor() const noexcept
{
    return _graphicsAttributes.foregroundColor;
}

inline color simple_cell::backgroundColor() const noexcept
{
    return _graphicsAttributes.backgroundColor;
}

inline color simple_cell::underlineColor() const noexcept
{
    return _graphicsAttributes.underlineColor;
}

inline std::shared_ptr<image_fragment> simple_cell::imageFragment() const noexcept
{
    return _imageFragment;
}

inline void simple_cell::setImageFragment(std::shared_ptr<rasterized_image> rasterizedImage,
                                          cell_location offset)
{
    _imageFragment = std::make_shared<image_fragment>(std::move(rasterizedImage), offset);
}

inline hyperlink_id simple_cell::hyperlink() const noexcept
{
    return _hyperlink;
}

inline void simple_cell::setHyperlink(hyperlink_id hyperlink) noexcept
{
    _hyperlink = hyperlink;
}

// }}}

// {{{ Optimized version for helpers from CellUtil
namespace CellUtil
{
    inline bool beginsWith(std::u32string_view text, simple_cell const& cell) noexcept
    {
        assert(!text.empty());
        return text == cell.codepoints();
    }
} // namespace CellUtil
// }}}

} // namespace terminal
