/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <vtbackend/ColorPalette.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/Image.h>
#include <vtbackend/primitives.h>

#include <crispy/Owned.h>
#include <crispy/defines.h>
#include <crispy/times.h>

#include <memory>
#include <string>

#include <libunicode/capi.h>
#include <libunicode/convert.h>
#include <libunicode/width.h>

namespace terminal
{

/// Rarely needed extra cell data.
///
/// In this struct we collect all the relevant cell data that is not frequently used,
/// and thus, would only waste unnecessary memory in most situations.
///
/// @see CompactCell
struct cell_extra
{
    /// With the main codepoint that is being stored in the CompactCell struct, followed by this
    /// sequence of codepoints, a grapheme cluster is formed that represents the visual
    /// character in this terminal cell.
    ///
    /// Since MOST content in the terminal is US-ASCII, all codepoints except the first one of a grapheme
    /// cluster is stored in CellExtra.
    std::u32string codepoints = {};

    /// Color for underline decoration (such as curly underline).
    color underlineColor = DefaultColor();

    /// With OSC-8 a hyperlink can be associated with a range of terminal cells.
    hyperlink_id hyperlink = {};

    /// Holds a reference to an image tile to be rendered (above the text, if any).
    std::shared_ptr<image_fragment> imageFragment = nullptr;

    /// Cell flags.
    cell_flags flags = cell_flags::None;

    /// In terminals, the Unicode's East asian Width property is used to determine the
    /// number of columns, a graphical character is spanning.
    /// Since most graphical characters in a terminal will be US-ASCII, this width property
    /// will be only used when NOT being 1.
    uint8_t width = 1;
};

/// Grid cell with character and graphics rendition information.
///
/// TODO(perf): ensure POD'ness so that we can SIMD-copy it.
/// - Requires moving out CellExtra into Line<T>?
class CRISPY_PACKED compact_cell
{
  public:
    // NOLINTNEXTLINE(readability-identifier-naming)
    static uint8_t constexpr MaxCodepoints = 7;

    compact_cell() noexcept;
    compact_cell(compact_cell const& v) noexcept;
    compact_cell& operator=(compact_cell const& v) noexcept;
    explicit compact_cell(graphics_attributes attributes, hyperlink_id hyperlink = {}) noexcept;

    compact_cell(compact_cell&&) noexcept = default;
    compact_cell& operator=(compact_cell&&) noexcept = default;
    ~compact_cell() = default;

    void reset() noexcept;
    void reset(graphics_attributes const& attributes) noexcept;
    void reset(graphics_attributes const& attributes, hyperlink_id hyperlink) noexcept;

    void write(graphics_attributes const& attributes, char32_t ch, uint8_t width) noexcept;
    void write(graphics_attributes const& attributes,
               char32_t ch,
               uint8_t width,
               hyperlink_id hyperlink) noexcept;

    void writeTextOnly(char32_t ch, uint8_t width) noexcept;

    [[nodiscard]] std::u32string codepoints() const;
    [[nodiscard]] char32_t codepoint(size_t i) const noexcept;
    [[nodiscard]] std::size_t codepointCount() const noexcept;

    [[nodiscard]] constexpr uint8_t width() const noexcept;
    void setWidth(uint8_t width) noexcept;

    [[nodiscard]] cell_flags flags() const noexcept;

    [[nodiscard]] bool isFlagEnabled(cell_flags testFlags) const noexcept { return flags() & testFlags; }

    void resetFlags() noexcept
    {
        if (_extra)
            _extra->flags = cell_flags::None;
    }

    void resetFlags(cell_flags flags) noexcept { extra().flags = flags; }

    [[nodiscard]] color underlineColor() const noexcept;
    void setUnderlineColor(color color) noexcept;
    [[nodiscard]] color foregroundColor() const noexcept;
    void setForegroundColor(color color) noexcept;
    [[nodiscard]] color backgroundColor() const noexcept;
    void setBackgroundColor(color color) noexcept;

    [[nodiscard]] std::shared_ptr<image_fragment> imageFragment() const noexcept;
    void setImageFragment(std::shared_ptr<rasterized_image> rasterizedImage, cell_location offset);

    void setCharacter(char32_t codepoint) noexcept;
    [[nodiscard]] int appendCharacter(char32_t codepoint) noexcept;
    [[nodiscard]] std::string toUtf8() const;

    [[nodiscard]] hyperlink_id hyperlink() const noexcept;
    void setHyperlink(hyperlink_id hyperlink);

    [[nodiscard]] bool empty() const noexcept;

    void setGraphicsRendition(graphics_rendition sgr) noexcept;

  private:
    [[nodiscard]] cell_extra& extra() noexcept;

    template <typename... Args>
    void createExtra(Args... args) noexcept;

    // CompactCell data
    char32_t _codepoint = 0; /// Primary Unicode codepoint to be displayed.
    color _foregroundColor = DefaultColor();
    color _backgroundColor = DefaultColor();
    crispy::Owned<cell_extra> _extra = {};
    // TODO(perf) ^^ use CellExtraId = boxed<int24_t> into pre-alloc'ed vector<CellExtra>.
};

// {{{ impl: ctor's
template <typename... Args>
inline void compact_cell::createExtra(Args... args) noexcept
{
    try
    {
        _extra.reset(new cell_extra(std::forward<Args>(args)...));
    }
    catch (std::bad_alloc const&)
    {
        Require(_extra.get() != nullptr);
    }
}

inline compact_cell::compact_cell() noexcept
{
    setWidth(1);
}

inline compact_cell::compact_cell(graphics_attributes attributes, hyperlink_id hyperlink) noexcept:
    _foregroundColor { attributes.foregroundColor }, _backgroundColor { attributes.backgroundColor }
{
    setWidth(1);
    setHyperlink(hyperlink);

    if (attributes.underlineColor != DefaultColor() || _extra)
        extra().underlineColor = attributes.underlineColor;

    if (attributes.flags != cell_flags::None || _extra)
        extra().flags = attributes.flags;
}

inline compact_cell::compact_cell(compact_cell const& v) noexcept:
    _codepoint { v._codepoint },
    _foregroundColor { v._foregroundColor },
    _backgroundColor { v._backgroundColor }
{
    if (v._extra)
        createExtra(*v._extra);
}

inline compact_cell& compact_cell::operator=(compact_cell const& v) noexcept
{
    _codepoint = v._codepoint;
    _foregroundColor = v._foregroundColor;
    _backgroundColor = v._backgroundColor;
    if (v._extra)
        createExtra(*v._extra);
    return *this;
}
// }}}
// {{{ impl: reset
inline void compact_cell::reset() noexcept
{
    _codepoint = 0;
    _foregroundColor = DefaultColor();
    _backgroundColor = DefaultColor();
    _extra.reset();
}

inline void compact_cell::reset(graphics_attributes const& attributes) noexcept
{
    _codepoint = 0;
    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;
    _extra.reset();
    if (attributes.flags != cell_flags::None)
        extra().flags = attributes.flags;
    if (attributes.underlineColor != DefaultColor())
        extra().underlineColor = attributes.underlineColor;
}

inline void compact_cell::write(graphics_attributes const& attributes, char32_t ch, uint8_t width) noexcept
{
    setWidth(width);

    _codepoint = ch;
    if (_extra)
    {
        _extra->codepoints.clear();
        _extra->imageFragment = {};
    }

    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;

    if (attributes.flags != cell_flags::None || _extra)
        extra().flags = attributes.flags;

    if (attributes.underlineColor != DefaultColor())
        extra().underlineColor = attributes.underlineColor;
}

inline void compact_cell::write(graphics_attributes const& attributes,
                                char32_t ch,
                                uint8_t width,
                                hyperlink_id hyperlink) noexcept
{
    writeTextOnly(ch, width);
    if (_extra)
    {
        // Writing text into a cell destroys the image fragment (as least for Sixels).
        _extra->imageFragment = {};
    }

    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;

    if (attributes.flags != cell_flags::None || _extra || attributes.underlineColor != DefaultColor()
        || !!hyperlink)
    {
        cell_extra& ext = extra();
        ext.underlineColor = attributes.underlineColor;
        ext.hyperlink = hyperlink;
        ext.flags = attributes.flags;
    }
}

inline void compact_cell::writeTextOnly(char32_t ch, uint8_t width) noexcept
{
    setWidth(width);
    _codepoint = ch;
    if (_extra)
        _extra->codepoints.clear();
}

inline void compact_cell::reset(graphics_attributes const& attributes, hyperlink_id hyperlink) noexcept
{
    _codepoint = 0;
    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;

    _extra.reset();
    if (attributes.underlineColor != DefaultColor())
        extra().underlineColor = attributes.underlineColor;
    if (attributes.flags != cell_flags::None)
        extra().flags = attributes.flags;
    if (hyperlink != hyperlink_id())
        extra().hyperlink = hyperlink;
}
// }}}
// {{{ impl: character
inline constexpr uint8_t compact_cell::width() const noexcept
{
    return !_extra ? 1 : _extra->width;
    // return static_cast<int>((_codepoint >> 21) & 0x03); //return _width;
}

inline void compact_cell::setWidth(uint8_t width) noexcept
{
    assert(width < MaxCodepoints);
    // _codepoint = _codepoint | ((width << 21) & 0x03);
    if (width > 1 || _extra)
        extra().width = width;
    // TODO(perf) use u32_unused_bit_mask()
}

inline void compact_cell::setCharacter(char32_t codepoint) noexcept
{
    _codepoint = codepoint;
    if (_extra)
    {
        _extra->codepoints.clear();
        _extra->imageFragment = {};
    }
    if (codepoint)
        setWidth(static_cast<uint8_t>(std::max(unicode::width(codepoint), 1)));
    else
        setWidth(1);
}

inline int compact_cell::appendCharacter(char32_t codepoint) noexcept
{
    assert(codepoint != 0);

    cell_extra& ext = extra();
    if (ext.codepoints.size() < MaxCodepoints - 1)
    {
        ext.codepoints.push_back(codepoint);
        if (auto const diff = CellUtil::computeWidthChange(*this, codepoint))
        {
            setWidth(static_cast<uint8_t>(static_cast<int>(width()) + diff));
            return diff;
        }
    }
    return 0;
}

inline std::size_t compact_cell::codepointCount() const noexcept
{
    if (_codepoint)
    {
        if (!_extra)
            return 1;

        return 1 + _extra->codepoints.size();
    }
    return 0;
}

inline char32_t compact_cell::codepoint(size_t i) const noexcept
{
    if (i == 0)
        return _codepoint;

    if (!_extra)
        return 0;

#if !defined(NDEBUG)
    return _extra->codepoints.at(i - 1);
#else
    return _extra->codepoints[i - 1];
#endif
}
// }}}
// {{{ attrs
inline cell_extra& compact_cell::extra() noexcept
{
    if (_extra)
        return *_extra;
    createExtra();
    return *_extra;
}

inline cell_flags compact_cell::flags() const noexcept
{
    if (!_extra)
        return cell_flags::None;
    else
        return const_cast<compact_cell*>(this)->_extra->flags;
}

inline color compact_cell::foregroundColor() const noexcept
{
    return _foregroundColor;
}

inline void compact_cell::setForegroundColor(color color) noexcept
{
    _foregroundColor = color;
}

inline color compact_cell::backgroundColor() const noexcept
{
    return _backgroundColor;
}

inline void compact_cell::setBackgroundColor(color color) noexcept
{
    _backgroundColor = color;
}

inline color compact_cell::underlineColor() const noexcept
{
    if (!_extra)
        return DefaultColor();
    else
        return _extra->underlineColor;
}

inline void compact_cell::setUnderlineColor(color color) noexcept
{
    if (_extra)
        _extra->underlineColor = color;
    else if (color != DefaultColor())
        extra().underlineColor = color;
}

inline std::shared_ptr<image_fragment> compact_cell::imageFragment() const noexcept
{
    if (_extra)
        return _extra->imageFragment;
    else
        return {};
}

inline void compact_cell::setImageFragment(std::shared_ptr<rasterized_image> rasterizedImage,
                                           cell_location offset)
{
    cell_extra& ext = extra();
    ext.imageFragment = std::make_shared<image_fragment>(std::move(rasterizedImage), offset);
}

inline hyperlink_id compact_cell::hyperlink() const noexcept
{
    if (_extra)
        return _extra->hyperlink;
    else
        return hyperlink_id {};
}

inline void compact_cell::setHyperlink(hyperlink_id hyperlink)
{
    if (!!hyperlink)
        extra().hyperlink = hyperlink;
    else if (_extra)
        _extra->hyperlink = {};
}

inline bool compact_cell::empty() const noexcept
{
    return CellUtil::empty(*this);
}

inline void compact_cell::setGraphicsRendition(graphics_rendition sgr) noexcept
{
    CellUtil::applyGraphicsRendition(sgr, *this);
}

// }}}
// {{{ free function implementations
inline bool beginsWith(std::u32string_view text, compact_cell const& cell) noexcept
{
    assert(!text.empty());

    if (cell.codepointCount() == 0)
        return false;

    if (text.size() < cell.codepointCount())
        return false;

    for (size_t i = 0; i < cell.codepointCount(); ++i)
        if (cell.codepoint(i) != text[i])
            return false;

    return true;
}
// }}}

} // namespace terminal

template <>
struct fmt::formatter<terminal::compact_cell>: fmt::formatter<std::string>
{
    auto format(terminal::compact_cell const& cell, format_context& ctx) -> format_context::iterator
    {
        std::string codepoints;
        for (auto const i: crispy::times(cell.codepointCount()))
        {
            if (i)
                codepoints += ", ";
            codepoints += fmt::format("{:02X}", static_cast<unsigned>(cell.codepoint(i)));
        }
        return formatter<std::string>::format(fmt::format("(chars={}, width={})", codepoints, cell.width()),
                                              ctx);
    }
};
