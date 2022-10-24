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

#include <terminal/CellFlags.h>
#include <terminal/ColorPalette.h>
#include <terminal/GraphicsAttributes.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/primitives.h>

#include <crispy/Owned.h>
#include <crispy/defines.h>
#include <crispy/times.h>

#include <unicode/capi.h>
#include <unicode/convert.h>
#include <unicode/width.h>

#include <memory>
#include <string>

#define LIBTERMINAL_GRAPHEME_CLUSTERS 1

namespace terminal
{

/// Rarely needed extra cell data.
///
/// @see Cell
struct CellExtra
{
    std::u32string codepoints = {};
    Color underlineColor = DefaultColor();
    HyperlinkId hyperlink = {};
    std::shared_ptr<ImageFragment> imageFragment = nullptr;
    CellFlags flags = CellFlags::None;
    uint8_t width = 1;
};

/// Grid cell with character and graphics rendition information.
///
/// TODO(perf): ensure POD'ness so that we can SIMD-copy it.
/// - Requires moving out CellExtra into Line<T>?
class CRISPY_PACKED Cell
{
  public:
    static uint8_t constexpr MaxCodepoints = 7;

    Cell() noexcept;
    Cell(Cell const& v) noexcept;
    Cell& operator=(Cell const& v) noexcept;
    explicit Cell(GraphicsAttributes _attributes, HyperlinkId hyperlink = {}) noexcept;

    Cell(Cell&&) noexcept = default;
    Cell& operator=(Cell&&) noexcept = default;
    ~Cell() = default;

    void reset() noexcept;
    void reset(GraphicsAttributes const& _attributes) noexcept;
    void reset(GraphicsAttributes const& _attributes, HyperlinkId _hyperlink) noexcept;

    void write(GraphicsAttributes const& _attributes, char32_t _ch, uint8_t _width) noexcept;
    void write(GraphicsAttributes const& _attributes,
               char32_t _ch,
               uint8_t _width,
               HyperlinkId _hyperlink) noexcept;

    void writeTextOnly(char32_t _ch, uint8_t _width) noexcept;

    [[nodiscard]] std::u32string codepoints() const;
    [[nodiscard]] char32_t codepoint(size_t i) const noexcept;
    [[nodiscard]] std::size_t codepointCount() const noexcept;

    [[nodiscard]] bool compareText(char codepoint) const noexcept;

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] constexpr uint8_t width() const noexcept;
    void setWidth(uint8_t _width) noexcept;

    [[nodiscard]] CellFlags flags() const noexcept;

    [[nodiscard]] bool isFlagEnabled(CellFlags testFlags) const noexcept { return flags() & testFlags; }

    void resetFlags() noexcept
    {
        if (extra_)
            extra_->flags = CellFlags::None;
    }

    void setFlags(CellFlags flags, bool enable = true)
    {
        if (enable)
            extra().flags = extra().flags | flags;
        else
            extra().flags = CellFlags(int(extra().flags) & ~int(flags));
    }

    void setGraphicsRendition(GraphicsRendition sgr) noexcept;

    [[nodiscard]] Color underlineColor() const noexcept;
    void setUnderlineColor(Color color) noexcept;
    [[nodiscard]] Color foregroundColor() const noexcept;
    void setForegroundColor(Color color) noexcept;
    [[nodiscard]] Color backgroundColor() const noexcept;
    void setBackgroundColor(Color color) noexcept;

    [[nodiscard]] RGBColor getUnderlineColor(ColorPalette const& _colorPalette,
                                             RGBColor _defaultColor) const noexcept;

    [[nodiscard]] RGBColorPair makeColors(ColorPalette const& _colorPalette,
                                          bool _reverseVideo,
                                          bool _blink,
                                          bool _rapidBlink) const noexcept;

    [[nodiscard]] std::shared_ptr<ImageFragment> imageFragment() const noexcept;
    void setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage, CellLocation offset);

    void setCharacter(char32_t _codepoint) noexcept;
    void setCharacter(char32_t _codepoint, uint8_t _width) noexcept;
    [[nodiscard]] int appendCharacter(char32_t _codepoint) noexcept;
    [[nodiscard]] std::string toUtf8() const;

    [[nodiscard]] HyperlinkId hyperlink() const noexcept;
    void setHyperlink(HyperlinkId _hyperlink);

    [[nodiscard]] CellExtra& extra() noexcept;

  private:
    template <typename... Args>
    void createExtra(Args... args) noexcept;

    // Cell data
    char32_t codepoint_ = 0; /// Primary Unicode codepoint to be displayed.
    Color foregroundColor_ = DefaultColor();
    Color backgroundColor_ = DefaultColor();
    crispy::Owned<CellExtra> extra_ = {};
    // TODO(perf) ^^ use CellExtraId = boxed<int24_t> into pre-alloc'ed vector<CellExtra>.
};

bool beginsWith(std::u32string_view text, Cell const& cell) noexcept;

// {{{ impl: ctor's
template <typename... Args>
inline void Cell::createExtra(Args... args) noexcept
{
    try
    {
        extra_.reset(new CellExtra(std::forward<Args>(args)...));
    }
    catch (std::bad_alloc const&)
    {
        Require(extra_.get() != nullptr);
    }
}

inline Cell::Cell() noexcept
{
    setWidth(1);
}

inline Cell::Cell(GraphicsAttributes _attributes, HyperlinkId hyperlink) noexcept:
    foregroundColor_ { _attributes.foregroundColor }, backgroundColor_ { _attributes.backgroundColor }
{
    setWidth(1);
    setHyperlink(hyperlink);

    if (_attributes.underlineColor != DefaultColor() || extra_)
        extra().underlineColor = _attributes.underlineColor;

    if (_attributes.flags != CellFlags::None || extra_)
        extra().flags = _attributes.flags;
}

inline Cell::Cell(Cell const& v) noexcept:
    codepoint_ { v.codepoint_ },
    foregroundColor_ { v.foregroundColor_ },
    backgroundColor_ { v.backgroundColor_ }
{
    if (v.extra_)
        createExtra(*v.extra_);
}

inline Cell& Cell::operator=(Cell const& v) noexcept
{
    codepoint_ = v.codepoint_;
    foregroundColor_ = v.foregroundColor_;
    backgroundColor_ = v.backgroundColor_;
    if (v.extra_)
        createExtra(*v.extra_);
    return *this;
}
// }}}
// {{{ impl: reset
inline void Cell::reset() noexcept
{
    codepoint_ = 0;
    foregroundColor_ = DefaultColor();
    backgroundColor_ = DefaultColor();
    extra_.reset();
}

inline void Cell::reset(GraphicsAttributes const& _attributes) noexcept
{
    codepoint_ = 0;
    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;
    extra_.reset();
    if (_attributes.flags != CellFlags::None)
        extra().flags = _attributes.flags;
    if (_attributes.underlineColor != DefaultColor())
        extra().underlineColor = _attributes.underlineColor;
}

inline void Cell::write(GraphicsAttributes const& _attributes, char32_t _ch, uint8_t _width) noexcept
{
    setWidth(_width);

    codepoint_ = _ch;
    if (extra_)
    {
        extra_->codepoints.clear();
        extra_->imageFragment = {};
    }

    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    if (_attributes.flags != CellFlags::None || extra_)
        extra().flags = _attributes.flags;

    if (_attributes.underlineColor != DefaultColor())
        extra().underlineColor = _attributes.underlineColor;
}

inline void Cell::write(GraphicsAttributes const& _attributes,
                        char32_t _ch,
                        uint8_t _width,
                        HyperlinkId _hyperlink) noexcept
{
    writeTextOnly(_ch, _width);
    if (extra_)
    {
        // Writing text into a cell destroys the image fragment (as least for Sixels).
        extra_->imageFragment = {};
    }

    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    if (_attributes.flags != CellFlags::None || extra_ || _attributes.underlineColor != DefaultColor()
        || !!_hyperlink)
    {
        CellExtra& ext = extra();
        ext.underlineColor = _attributes.underlineColor;
        ext.hyperlink = _hyperlink;
        ext.flags = _attributes.flags;
    }
}

inline void Cell::writeTextOnly(char32_t _ch, uint8_t _width) noexcept
{
    setWidth(_width);
    codepoint_ = _ch;
    if (extra_)
        extra_->codepoints.clear();
}

inline void Cell::reset(GraphicsAttributes const& _attributes, HyperlinkId _hyperlink) noexcept
{
    codepoint_ = 0;
    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    extra_.reset();
    if (_attributes.underlineColor != DefaultColor())
        extra().underlineColor = _attributes.underlineColor;
    if (_attributes.flags != CellFlags::None)
        extra().flags = _attributes.flags;
    if (_hyperlink != HyperlinkId())
        extra().hyperlink = _hyperlink;
}
// }}}
// {{{ impl: character
inline constexpr uint8_t Cell::width() const noexcept
{
    return !extra_ ? 1 : extra_->width;
    // return static_cast<int>((codepoint_ >> 21) & 0x03); //return width_;
}

inline void Cell::setWidth(uint8_t _width) noexcept
{
    assert(_width < MaxCodepoints);
    // codepoint_ = codepoint_ | ((_width << 21) & 0x03);
    if (_width > 1 || extra_)
        extra().width = _width;
    // TODO(perf) use u32_unused_bit_mask()
}

inline void Cell::setCharacter(char32_t _codepoint, uint8_t _width) noexcept
{
    assert(_codepoint != 0);

    codepoint_ = _codepoint;

    if (extra_)
    {
        extra_->codepoints.clear();
        extra_->imageFragment = {};
        extra_->width = _width;
    }
    else
        setWidth(_width);
}

inline void Cell::setCharacter(char32_t _codepoint) noexcept
{
    codepoint_ = _codepoint;
    if (extra_)
    {
        extra_->codepoints.clear();
        extra_->imageFragment = {};
    }
    if (_codepoint)
        setWidth(static_cast<uint8_t>(std::max(unicode::width(_codepoint), 1)));
    else
        setWidth(1);
}

inline int Cell::appendCharacter(char32_t _codepoint) noexcept
{
    assert(_codepoint != 0);

    CellExtra& ext = extra();
    if (ext.codepoints.size() < MaxCodepoints - 1)
    {
        ext.codepoints.push_back(_codepoint);

        constexpr bool AllowWidthChange = false; // TODO: make configurable

        auto const w = [&]() {
            switch (_codepoint)
            {
                case 0xFE0E: return 1;
                case 0xFE0F: return 2;
                default: return unicode::width(_codepoint);
            }
        }();

        if (w != width() && AllowWidthChange)
        {
            int const diff = w - width();
            setWidth(static_cast<uint8_t>(w));
            return diff;
        }
    }
    return 0;
}

inline std::size_t Cell::codepointCount() const noexcept
{
    if (codepoint_)
    {
        if (!extra_)
            return 1;

        return 1 + extra_->codepoints.size();
    }
    return 0;
}

inline bool Cell::compareText(char codepoint) const noexcept
{
    if (codepointCount() != 1)
        return false;

    return codepoint_ == static_cast<char32_t>(codepoint);
}

inline char32_t Cell::codepoint(size_t i) const noexcept
{
    if (i == 0)
        return codepoint_;

    if (!extra_)
        return 0;

#if !defined(NDEBUG)
    return extra_->codepoints.at(i - 1);
#else
    return extra_->codepoints[i - 1];
#endif
}
// }}}
// {{{ attrs
inline bool Cell::empty() const noexcept
{
    return (codepointCount() == 0 || codepoint(0) == 0x20) && !imageFragment();
}

inline CellExtra& Cell::extra() noexcept
{
    if (extra_)
        return *extra_;
    createExtra();
    return *extra_;
}

inline CellFlags Cell::flags() const noexcept
{
    if (!extra_)
        return CellFlags::None;
    else
        return const_cast<Cell*>(this)->extra_->flags;
}

inline Color Cell::foregroundColor() const noexcept
{
    return foregroundColor_;
}

inline void Cell::setForegroundColor(Color color) noexcept
{
    foregroundColor_ = color;
}

inline Color Cell::backgroundColor() const noexcept
{
    return backgroundColor_;
}

inline void Cell::setBackgroundColor(Color color) noexcept
{
    backgroundColor_ = color;
}

inline Color Cell::underlineColor() const noexcept
{
    if (!extra_)
        return DefaultColor();
    else
        return extra_->underlineColor;
}

inline void Cell::setUnderlineColor(Color color) noexcept
{
    if (extra_)
        extra_->underlineColor = color;
    else if (color != DefaultColor())
        extra().underlineColor = color;
}

inline RGBColor getUnderlineColor(ColorPalette const& colorPalette,
                                  CellFlags cellFlags,
                                  RGBColor defaultColor,
                                  Color underlineColor) noexcept
{
    if (isDefaultColor(underlineColor))
        return defaultColor;

    auto const mode = (cellFlags & CellFlags::Faint)                                    ? ColorMode::Dimmed
                      : ((cellFlags & CellFlags::Bold) && colorPalette.useBrightColors) ? ColorMode::Bright
                                                                                        : ColorMode::Normal;

    return apply(colorPalette, underlineColor, ColorTarget::Foreground, mode);
}

inline RGBColor Cell::getUnderlineColor(ColorPalette const& _colorPalette,
                                        RGBColor _defaultColor) const noexcept
{
    return terminal::getUnderlineColor(_colorPalette, flags(), _defaultColor, underlineColor());
}

inline RGBColorPair makeColors(ColorPalette const& colorPalette,
                               CellFlags cellFlags,
                               bool _reverseVideo,
                               Color foregroundColor,
                               Color backgroundColor,
                               bool blinkingState_,
                               bool rapidBlinkState_) noexcept
{
    auto const fgMode = (cellFlags & CellFlags::Faint)                                    ? ColorMode::Dimmed
                        : ((cellFlags & CellFlags::Bold) && colorPalette.useBrightColors) ? ColorMode::Bright
                                                                                          : ColorMode::Normal;

    auto constexpr bgMode = ColorMode::Normal;

    auto const [fgColorTarget, bgColorTarget] =
        _reverseVideo ? std::pair { ColorTarget::Background, ColorTarget::Foreground }
                      : std::pair { ColorTarget::Foreground, ColorTarget::Background };

    auto rgbColors = RGBColorPair { apply(colorPalette, foregroundColor, fgColorTarget, fgMode),
                                    apply(colorPalette, backgroundColor, bgColorTarget, bgMode) };

    if (cellFlags & CellFlags::Inverse)
        rgbColors = rgbColors.swapped();

    if (cellFlags & CellFlags::Hidden)
        rgbColors = rgbColors.allBackground();

    if ((cellFlags & CellFlags::Blinking) && !blinkingState_)
        return rgbColors.allBackground();
    if ((cellFlags & CellFlags::RapidBlinking) && !rapidBlinkState_)
        return rgbColors.allBackground();

    return rgbColors;
}

inline RGBColorPair Cell::makeColors(ColorPalette const& _colorPalette,
                                     bool _reverseVideo,
                                     bool _blink,
                                     bool _rapidBlink) const noexcept
{
    return terminal::makeColors(
        _colorPalette, flags(), _reverseVideo, foregroundColor(), backgroundColor(), _blink, _rapidBlink);
}

inline std::shared_ptr<ImageFragment> Cell::imageFragment() const noexcept
{
    if (extra_)
        return extra_->imageFragment;
    else
        return {};
}

inline void Cell::setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage, CellLocation offset)
{
    CellExtra& ext = extra();
    ext.imageFragment = std::make_shared<ImageFragment>(rasterizedImage, offset);
}

inline HyperlinkId Cell::hyperlink() const noexcept
{
    if (extra_)
        return extra_->hyperlink;
    else
        return HyperlinkId {};
}

inline void Cell::setHyperlink(HyperlinkId _hyperlink)
{
    if (!!_hyperlink)
        extra().hyperlink = _hyperlink;
    else if (extra_)
        extra_->hyperlink = {};
}

inline void Cell::setGraphicsRendition(GraphicsRendition _rendition) noexcept
{
    // TODO: optimize this as there are only 3 cases
    // 1.) reset
    // 2.) set some bits |=
    // 3.) clear some bits &= ~
    switch (_rendition)
    {
        case GraphicsRendition::Reset: extra().flags = {}; break;
        case GraphicsRendition::Bold: extra().flags |= CellFlags::Bold; break;
        case GraphicsRendition::Faint: extra().flags |= CellFlags::Faint; break;
        case GraphicsRendition::Italic: extra().flags |= CellFlags::Italic; break;
        case GraphicsRendition::Underline: extra().flags |= CellFlags::Underline; break;
        case GraphicsRendition::Blinking:
            extra().flags &= ~CellFlags::RapidBlinking;
            extra().flags |= CellFlags::Blinking;
            break;
        case GraphicsRendition::RapidBlinking:
            extra().flags &= ~CellFlags::Blinking;
            extra().flags |= CellFlags::RapidBlinking;
            break;
        case GraphicsRendition::Inverse: extra().flags |= CellFlags::Inverse; break;
        case GraphicsRendition::Hidden: extra().flags |= CellFlags::Hidden; break;
        case GraphicsRendition::CrossedOut: extra().flags |= CellFlags::CrossedOut; break;
        case GraphicsRendition::DoublyUnderlined: extra().flags |= CellFlags::DoublyUnderlined; break;
        case GraphicsRendition::CurlyUnderlined: extra().flags |= CellFlags::CurlyUnderlined; break;
        case GraphicsRendition::DottedUnderline: extra().flags |= CellFlags::DottedUnderline; break;
        case GraphicsRendition::DashedUnderline: extra().flags |= CellFlags::DashedUnderline; break;
        case GraphicsRendition::Framed: extra().flags |= CellFlags::Framed; break;
        case GraphicsRendition::Overline: extra().flags |= CellFlags::Overline; break;
        case GraphicsRendition::Normal: extra().flags &= ~(CellFlags::Bold | CellFlags::Faint); break;
        case GraphicsRendition::NoItalic: extra().flags &= ~CellFlags::Italic; break;
        case GraphicsRendition::NoUnderline:
            extra().flags &= ~(CellFlags::Underline | CellFlags::DoublyUnderlined | CellFlags::CurlyUnderlined
                               | CellFlags::DottedUnderline | CellFlags::DashedUnderline);
            break;
        case GraphicsRendition::NoBlinking:
            extra().flags &= ~(CellFlags::Blinking | CellFlags::RapidBlinking);
            break;
        case GraphicsRendition::NoInverse: extra().flags &= ~CellFlags::Inverse; break;
        case GraphicsRendition::NoHidden: extra().flags &= ~CellFlags::Hidden; break;
        case GraphicsRendition::NoCrossedOut: extra().flags &= ~CellFlags::CrossedOut; break;
        case GraphicsRendition::NoFramed: extra().flags &= ~CellFlags::Framed; break;
        case GraphicsRendition::NoOverline: extra().flags &= ~CellFlags::Overline; break;
    }
}

// }}}
// {{{ free function implementations
inline bool beginsWith(std::u32string_view text, Cell const& cell) noexcept
{
    assert(text.size() != 0);

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

namespace fmt // {{{
{
template <>
struct formatter<terminal::Cell>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::Cell const& cell, FormatContext& ctx)
    {
        std::string codepoints;
        for (auto const i: crispy::times(cell.codepointCount()))
        {
            if (i)
                codepoints += ", ";
            codepoints += fmt::format("{:02X}", static_cast<unsigned>(cell.codepoint(i)));
        }
        return fmt::format_to(ctx.out(), "(chars={}, width={})", codepoints, cell.width());
    }
};
} // namespace fmt
