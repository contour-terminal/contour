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
#include <terminal/GraphicsAttributes.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/primitives.h>
#include <terminal/defines.h>

#include <crispy/times.h>

#include <unicode/capi.h>
#include <unicode/convert.h>
#include <unicode/width.h>

#include <string>

#define LIBTERMINAL_GRAPHEME_CLUSTERS 1

namespace terminal {

/**
 * Owned<T> behaves mostly like std::unique_ptr<T> except that it can be also
 * used within packed structs.
 */
template <typename T>
struct CONTOUR_PACKED Owned
{
    T* ptr_ = nullptr;

    constexpr T* operator->() noexcept { return ptr_; }
    constexpr T const* operator->() const noexcept { return ptr_; }
    constexpr T& operator*() noexcept { return *ptr_; }
    constexpr T const& operator*() const noexcept { return *ptr_; }

    constexpr operator bool () const noexcept { return ptr_ != nullptr; }

    void reset(T* p = nullptr) { if (ptr_) { delete ptr_; } ptr_ = p; }
    T* release() noexcept { auto p = ptr_; ptr_ = nullptr; return p; }

    ~Owned() { reset(); }
    Owned() noexcept {}
    Owned(Owned&& v) noexcept: ptr_{v.release()} {}
    Owned& operator=(Owned&& v) noexcept { ptr_ = v.release(); return *this; }

    Owned(Owned const& v) noexcept = delete;
    Owned& operator=(Owned const& v) = delete;
};

/// Rarely needed extra cell data.
///
/// @see Cell
struct CellExtra
{
    std::u32string codepoints = {};
    Color underlineColor = DefaultColor();
    HyperlinkId hyperlink = {};
    ImageFragmentId imageFragment = {};
    CellFlags flags = CellFlags::None;
    uint8_t width = 1;
};

/// Grid cell with character and graphics rendition information.
///
/// TODO(perf): ensure POD'ness so that we can SIMD-copy it.
/// - Requires moving out CellExtra into Line<T>?
class CONTOUR_PACKED Cell
{
public:
    static int constexpr MaxCodepoints = 7;

    Cell() noexcept;
    Cell(Cell const& v) noexcept;
    Cell& operator=(Cell const& v) noexcept;
    explicit Cell(GraphicsAttributes _attrib) noexcept;

    Cell(Cell&&) noexcept = default;
    Cell& operator=(Cell&&) noexcept = default;
    ~Cell() = default;

    void reset() noexcept;
    void reset(GraphicsAttributes const& _attributes) noexcept;
    void reset(GraphicsAttributes const& _attributes, HyperlinkId _hyperlink) noexcept;

    void write(GraphicsAttributes const& _attributes, char32_t _ch, uint8_t _width) noexcept;
    void write(GraphicsAttributes const& _attributes, char32_t _ch, uint8_t _width, HyperlinkId _hyperlink) noexcept;

    std::u32string codepoints() const;
    char32_t codepoint(size_t i) const noexcept;
    std::size_t codepointCount() const noexcept;

    bool empty() const noexcept;

    constexpr int width() const noexcept;
    void setWidth(uint8_t _width) noexcept;

    /*TODO(perf) [[deprecated]]*/ GraphicsAttributes attributes() const noexcept;
    void setAttributes(GraphicsAttributes const& _attributes) noexcept;

    CellFlags styles() const noexcept;

    Color underlineColor() const noexcept;
    void setUnderlineColor(Color color) noexcept;
    Color foregroundColor() const noexcept;
    void setForegroundColor(Color color) noexcept;
    Color backgroundColor() const noexcept;
    void setBackgroundColor(Color color) noexcept;

    RGBColor getUnderlineColor(ColorPalette const& _colorPalette, RGBColor _defaultColor) const noexcept;

    std::pair<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette, bool _reverseVideo) const noexcept;

    ImageFragmentId imageFragment() const noexcept;
    void setImage(ImageFragmentId _imageFragment);
    void setImage(ImageFragmentId _imageFragment, HyperlinkId _hyperlink);

    void setCharacter(char32_t _codepoint) noexcept;
    void setCharacter(char32_t _codepoint, uint8_t _width) noexcept;
    int appendCharacter(char32_t _codepoint) noexcept;
    std::string toUtf8() const;

    HyperlinkId hyperlink() const noexcept;
    void setHyperlink(HyperlinkId _hyperlink);

    CellExtra& extra() noexcept;

private:
    // Cell data
    char32_t codepoint_ = 0; /// Primary Unicode codepoint to be displayed.
    Color foregroundColor_ = DefaultColor();
    Color backgroundColor_ = DefaultColor();
    Owned<CellExtra> extra_ = {}; // TODO(perf) ^^ use CellExtraId = boxed<int24_t> into pre-alloc'ed vector<CellExtra>.
};

// {{{ impl: ctor's
inline Cell::Cell() noexcept
{
    setWidth(1);
}

inline Cell::Cell(GraphicsAttributes _attrib) noexcept
{
    setWidth(1);
    setAttributes(_attrib);
}

inline Cell::Cell(Cell const& v) noexcept:
    codepoint_{v.codepoint_},
    foregroundColor_{v.foregroundColor_},
    backgroundColor_{v.backgroundColor_}
{
    if (v.extra_)
        extra_.reset(new CellExtra(*v.extra_));
}

inline Cell& Cell::operator=(Cell const& v) noexcept
{
    codepoint_ = v.codepoint_;
    foregroundColor_ = v.foregroundColor_;
    backgroundColor_ = v.backgroundColor_;
    if (v.extra_)
        extra_.reset(new CellExtra(*v.extra_));
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
    if (_attributes.styles != CellFlags::None)
        extra().flags = _attributes.styles;
    if (_attributes.underlineColor != DefaultColor())
        extra().underlineColor = _attributes.underlineColor;
}

inline void Cell::write(GraphicsAttributes const& _attributes, char32_t _ch, uint8_t _width) noexcept
{
    setWidth(_width);

    codepoint_ = _ch;
    if (extra_)
        extra_->codepoints.clear();

    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    if (_attributes.styles != CellFlags::None || extra_)
        extra().flags = _attributes.styles;

    if (_attributes.underlineColor != DefaultColor())
        extra().underlineColor = _attributes.underlineColor;
}

inline void Cell::write(GraphicsAttributes const& _attributes, char32_t _ch, uint8_t _width, HyperlinkId _hyperlink) noexcept
{
    setWidth(_width);

    codepoint_ = _ch;
    if (extra_)
        extra_->codepoints.clear();

    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    if (_attributes.styles != CellFlags::None ||
        extra_ ||
        _attributes.underlineColor != DefaultColor() ||
        !!_hyperlink)
    {
        CellExtra& ext = extra();
        ext.underlineColor = _attributes.underlineColor;
        ext.hyperlink = _hyperlink;
        ext.flags = _attributes.styles;
    }
}

inline void Cell::reset(GraphicsAttributes const& _attributes, HyperlinkId _hyperlink) noexcept
{
    codepoint_ = 0;
    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    extra_.reset();
    if (_attributes.underlineColor != DefaultColor())
        extra().underlineColor = _attributes.underlineColor;
    if (_attributes.styles != CellFlags::None)
        extra().flags = _attributes.styles;
}
// }}}
// {{{ impl: character
inline constexpr int Cell::width() const noexcept
{
    return !extra_ ? 1 : extra_->width;
    //return static_cast<int>((codepoint_ >> 21) & 0x03); //return width_;
}

inline void Cell::setWidth(uint8_t _width) noexcept
{
    assert(_width < MaxCodepoints);
    //codepoint_ = codepoint_ | ((_width << 21) & 0x03);
    if (_width > 1 || extra_)
        extra().width = _width;
    //TODO(perf) use u32_unused_bit_mask()
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
        extra_->codepoints.clear();
    if (_codepoint)
        setWidth(static_cast<uint8_t>(std::max(unicode::width(_codepoint), 1)));
    else
        setWidth(1);
}

inline int Cell::appendCharacter(char32_t _codepoint) noexcept
{
    assert(codepoint_ != 0);

    CellExtra& ext = extra();
    if (ext.codepoints.size() < MaxCodepoints - 1)
    {
        ext.codepoints.push_back(_codepoint);

        constexpr bool AllowWidthChange = false; // TODO: make configurable

        auto const w = [&]() {
            switch (_codepoint)
            {
                case 0xFE0E:
                    return 1;
                case 0xFE0F:
                    return 2;
                default:
                    return unicode::width(_codepoint);
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
    extra_.reset(new CellExtra());
    return *extra_;
}

inline void Cell::setAttributes(GraphicsAttributes const& _attributes) noexcept
{
    foregroundColor_ = _attributes.foregroundColor;
    backgroundColor_ = _attributes.backgroundColor;

    if (_attributes.underlineColor != DefaultColor() || extra_)
        extra().underlineColor = _attributes.underlineColor;

    if (_attributes.styles != CellFlags::None || extra_)
        extra().flags = _attributes.styles;
}

inline CellFlags Cell::styles() const noexcept
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

inline RGBColor Cell::getUnderlineColor(ColorPalette const& _colorPalette, RGBColor _defaultColor) const noexcept
{
    if (isDefaultColor(underlineColor()))
        return _defaultColor;

    float const opacity = [this]() {
        if (styles() & CellFlags::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    bool const bright = (styles() & CellFlags::Bold) != 0;
    return apply(_colorPalette, underlineColor(), ColorTarget::Foreground, bright) * opacity;
}

inline std::pair<RGBColor, RGBColor> Cell::makeColors(ColorPalette const& _colorPalette, bool _reverseVideo) const noexcept
{
    float const opacity = [this]() { // TODO: don't make opacity dependant on Faint-attribute.
        if (styles() & CellFlags::Faint)
            return 0.5f;
        else
            return 1.0f;
    }();

    bool const bright = (styles() & CellFlags::Bold);

    auto const [fgColorTarget, bgColorTarget] =
        _reverseVideo
            ? std::pair{ ColorTarget::Background, ColorTarget::Foreground }
            : std::pair{ ColorTarget::Foreground, ColorTarget::Background };

    return (styles() & CellFlags::Inverse) == 0
        ? std::pair{ apply(_colorPalette, foregroundColor(), fgColorTarget, bright) * opacity,
                     apply(_colorPalette, backgroundColor(), bgColorTarget, false) }
        : std::pair{ apply(_colorPalette, backgroundColor(), bgColorTarget, bright) * opacity,
                     apply(_colorPalette, foregroundColor(), fgColorTarget, false) };
}

inline ImageFragmentId Cell::imageFragment() const noexcept
{
    if (extra_)
        return extra_->imageFragment;
    else
        return {};
}

inline void Cell::setImage(ImageFragmentId _imageFragment)
{
    assert(!!_imageFragment);
    CellExtra& ext = extra();
    ext.imageFragment = _imageFragment;
}

inline void Cell::setImage(ImageFragmentId _imageFragment, HyperlinkId _hyperlink)
{
    setImage(_imageFragment);
    assert(extra_);
    if (!!_hyperlink)
        extra_->hyperlink = _hyperlink;
}

inline HyperlinkId Cell::hyperlink() const noexcept
{
    if (extra_)
        return extra_->hyperlink;
    else
        return HyperlinkId{};
}

inline void Cell::setHyperlink(HyperlinkId _hyperlink)
{
    if (!!_hyperlink)
        extra().hyperlink = _hyperlink;
    else if (extra_)
        extra_->hyperlink = {};
}
// }}}

}

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Cell> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Cell const& cell, FormatContext& ctx)
        {
            std::string codepoints;
            for (auto const i : crispy::times(cell.codepointCount()))
            {
                if (i)
                    codepoints += ", ";
                codepoints += fmt::format("{:02X}", static_cast<unsigned>(cell.codepoint(i)));
            }
            return format_to(ctx.out(), "(chars={}, width={})", codepoints, cell.width());
        }
    };
} // }}}
