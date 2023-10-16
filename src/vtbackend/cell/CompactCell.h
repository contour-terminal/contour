// SPDX-License-Identifier: Apache-2.0
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

#include <libunicode/capi.h>
#include <libunicode/convert.h>
#include <libunicode/width.h>

#include <memory>
#include <string>

namespace vtbackend
{

/// Rarely needed extra cell data.
///
/// In this struct we collect all the relevant cell data that is not frequently used,
/// and thus, would only waste unnecessary memory in most situations.
///
/// @see CompactCell
struct CellExtra
{
    /// With the main codepoint that is being stored in the CompactCell struct, followed by this
    /// sequence of codepoints, a grapheme cluster is formed that represents the visual
    /// character in this terminal cell.
    ///
    /// Since MOST content in the terminal is US-ASCII, all codepoints except the first one of a grapheme
    /// cluster is stored in CellExtra.
    std::u32string codepoints = {};

    /// Color for underline decoration (such as curly underline).
    Color underlineColor = DefaultColor();

    /// With OSC-8 a hyperlink can be associated with a range of terminal cells.
    HyperlinkId hyperlink = {};

    /// Holds a reference to an image tile to be rendered (above the text, if any).
    std::shared_ptr<ImageFragment> imageFragment = nullptr;

    /// Cell flags.
    CellFlags flags = CellFlags::None;

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
class CRISPY_PACKED CompactCell
{
  public:
    // NOLINTNEXTLINE(readability-identifier-naming)
    static uint8_t constexpr MaxCodepoints = 7;

    CompactCell() noexcept;
    CompactCell(CompactCell const& v) noexcept;
    CompactCell& operator=(CompactCell const& v) noexcept;
    explicit CompactCell(GraphicsAttributes attributes, HyperlinkId hyperlink = {}) noexcept;

    CompactCell(CompactCell&&) noexcept = default;
    CompactCell& operator=(CompactCell&&) noexcept = default;
    ~CompactCell() = default;

    void reset() noexcept;
    void reset(GraphicsAttributes const& attributes) noexcept;
    void reset(GraphicsAttributes const& attributes, HyperlinkId hyperlink) noexcept;

    void write(GraphicsAttributes const& attributes, char32_t ch, uint8_t width) noexcept;
    void write(GraphicsAttributes const& attributes,
               char32_t ch,
               uint8_t width,
               HyperlinkId hyperlink) noexcept;

    void writeTextOnly(char32_t ch, uint8_t width) noexcept;

    [[nodiscard]] std::u32string codepoints() const;
    [[nodiscard]] char32_t codepoint(size_t i) const noexcept;
    [[nodiscard]] std::size_t codepointCount() const noexcept;

    [[nodiscard]] constexpr uint8_t width() const noexcept;
    void setWidth(uint8_t width) noexcept;

    [[nodiscard]] CellFlags flags() const noexcept;

    [[nodiscard]] bool isFlagEnabled(CellFlags testFlags) const noexcept { return flags() & testFlags; }

    void resetFlags() noexcept
    {
        if (_extra)
            _extra->flags = CellFlags::None;
    }

    void resetFlags(CellFlags flags) noexcept { extra().flags = flags; }

    [[nodiscard]] Color underlineColor() const noexcept;
    void setUnderlineColor(Color color) noexcept;
    [[nodiscard]] Color foregroundColor() const noexcept;
    void setForegroundColor(Color color) noexcept;
    [[nodiscard]] Color backgroundColor() const noexcept;
    void setBackgroundColor(Color color) noexcept;

    [[nodiscard]] std::shared_ptr<ImageFragment> imageFragment() const noexcept;
    void setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage, CellLocation offset);

    void setCharacter(char32_t codepoint) noexcept;
    [[nodiscard]] int appendCharacter(char32_t codepoint) noexcept;
    [[nodiscard]] std::string toUtf8() const;

    [[nodiscard]] HyperlinkId hyperlink() const noexcept;
    void setHyperlink(HyperlinkId hyperlink);

    [[nodiscard]] bool empty() const noexcept;

    void setGraphicsRendition(GraphicsRendition sgr) noexcept;

  private:
    [[nodiscard]] CellExtra& extra() noexcept;

    template <typename... Args>
    void createExtra(Args... args) noexcept;

    // CompactCell data
    char32_t _codepoint = 0; /// Primary Unicode codepoint to be displayed.
    Color _foregroundColor = DefaultColor();
    Color _backgroundColor = DefaultColor();
    crispy::owned<CellExtra> _extra = {};
    // TODO(perf) ^^ use CellExtraId = boxed<int24_t> into pre-alloc'ed vector<CellExtra>.
};

// {{{ impl: ctor's
template <typename... Args>
inline void CompactCell::createExtra(Args... args) noexcept
{
    try
    {
        _extra.reset(new CellExtra(std::forward<Args>(args)...));
    }
    catch (std::bad_alloc const&)
    {
        Require(_extra.get() != nullptr);
    }
}

inline CompactCell::CompactCell() noexcept
{
    setWidth(1);
}

inline CompactCell::CompactCell(GraphicsAttributes attributes, HyperlinkId hyperlink) noexcept:
    _foregroundColor { attributes.foregroundColor }, _backgroundColor { attributes.backgroundColor }
{
    setWidth(1);
    setHyperlink(hyperlink);

    if (attributes.underlineColor != DefaultColor() || _extra)
        extra().underlineColor = attributes.underlineColor;

    if (attributes.flags != CellFlags::None || _extra)
        extra().flags = attributes.flags;
}

inline CompactCell::CompactCell(CompactCell const& v) noexcept:
    _codepoint { v._codepoint },
    _foregroundColor { v._foregroundColor },
    _backgroundColor { v._backgroundColor }
{
    if (v._extra)
        createExtra(*v._extra);
}

inline CompactCell& CompactCell::operator=(CompactCell const& v) noexcept
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
inline void CompactCell::reset() noexcept
{
    _codepoint = 0;
    _foregroundColor = DefaultColor();
    _backgroundColor = DefaultColor();
    _extra.reset();
}

inline void CompactCell::reset(GraphicsAttributes const& attributes) noexcept
{
    _codepoint = 0;
    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;
    _extra.reset();
    if (attributes.flags != CellFlags::None)
        extra().flags = attributes.flags;
    if (attributes.underlineColor != DefaultColor())
        extra().underlineColor = attributes.underlineColor;
}

inline void CompactCell::write(GraphicsAttributes const& attributes, char32_t ch, uint8_t width) noexcept
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

    if (attributes.flags != CellFlags::None || _extra)
        extra().flags = attributes.flags;

    if (attributes.underlineColor != DefaultColor())
        extra().underlineColor = attributes.underlineColor;
}

inline void CompactCell::write(GraphicsAttributes const& attributes,
                               char32_t ch,
                               uint8_t width,
                               HyperlinkId hyperlink) noexcept
{
    writeTextOnly(ch, width);
    if (_extra)
    {
        // Writing text into a cell destroys the image fragment (as least for Sixels).
        _extra->imageFragment = {};
    }

    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;

    if (attributes.flags != CellFlags::None || _extra || attributes.underlineColor != DefaultColor()
        || !!hyperlink)
    {
        CellExtra& ext = extra();
        ext.underlineColor = attributes.underlineColor;
        ext.hyperlink = hyperlink;
        ext.flags = attributes.flags;
    }
}

inline void CompactCell::writeTextOnly(char32_t ch, uint8_t width) noexcept
{
    setWidth(width);
    _codepoint = ch;
    if (_extra)
        _extra->codepoints.clear();
}

inline void CompactCell::reset(GraphicsAttributes const& attributes, HyperlinkId hyperlink) noexcept
{
    _codepoint = 0;
    _foregroundColor = attributes.foregroundColor;
    _backgroundColor = attributes.backgroundColor;

    _extra.reset();
    if (attributes.underlineColor != DefaultColor())
        extra().underlineColor = attributes.underlineColor;
    if (attributes.flags != CellFlags::None)
        extra().flags = attributes.flags;
    if (hyperlink != HyperlinkId())
        extra().hyperlink = hyperlink;
}
// }}}
// {{{ impl: character
inline constexpr uint8_t CompactCell::width() const noexcept
{
    return !_extra ? 1 : _extra->width;
    // return static_cast<int>((_codepoint >> 21) & 0x03); //return _width;
}

inline void CompactCell::setWidth(uint8_t width) noexcept
{
    assert(width < MaxCodepoints);
    // _codepoint = _codepoint | ((width << 21) & 0x03);
    if (width > 1 || _extra)
        extra().width = width;
    // TODO(perf) use u32_unused_bit_mask()
}

inline void CompactCell::setCharacter(char32_t codepoint) noexcept
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

inline int CompactCell::appendCharacter(char32_t codepoint) noexcept
{
    assert(codepoint != 0);

    CellExtra& ext = extra();
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

inline std::size_t CompactCell::codepointCount() const noexcept
{
    if (_codepoint)
    {
        if (!_extra)
            return 1;

        return 1 + _extra->codepoints.size();
    }
    return 0;
}

inline char32_t CompactCell::codepoint(size_t i) const noexcept
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
inline CellExtra& CompactCell::extra() noexcept
{
    if (_extra)
        return *_extra;
    createExtra();
    return *_extra;
}

inline CellFlags CompactCell::flags() const noexcept
{
    if (!_extra)
        return CellFlags::None;
    else
        return const_cast<CompactCell*>(this)->_extra->flags;
}

inline Color CompactCell::foregroundColor() const noexcept
{
    return _foregroundColor;
}

inline void CompactCell::setForegroundColor(Color color) noexcept
{
    _foregroundColor = color;
}

inline Color CompactCell::backgroundColor() const noexcept
{
    return _backgroundColor;
}

inline void CompactCell::setBackgroundColor(Color color) noexcept
{
    _backgroundColor = color;
}

inline Color CompactCell::underlineColor() const noexcept
{
    if (!_extra)
        return DefaultColor();
    else
        return _extra->underlineColor;
}

inline void CompactCell::setUnderlineColor(Color color) noexcept
{
    if (_extra)
        _extra->underlineColor = color;
    else if (color != DefaultColor())
        extra().underlineColor = color;
}

inline std::shared_ptr<ImageFragment> CompactCell::imageFragment() const noexcept
{
    if (_extra)
        return _extra->imageFragment;
    else
        return {};
}

inline void CompactCell::setImageFragment(std::shared_ptr<RasterizedImage> rasterizedImage,
                                          CellLocation offset)
{
    CellExtra& ext = extra();
    ext.imageFragment = std::make_shared<ImageFragment>(std::move(rasterizedImage), offset);
}

inline HyperlinkId CompactCell::hyperlink() const noexcept
{
    if (_extra)
        return _extra->hyperlink;
    else
        return HyperlinkId {};
}

inline void CompactCell::setHyperlink(HyperlinkId hyperlink)
{
    if (!!hyperlink)
        extra().hyperlink = hyperlink;
    else if (_extra)
        _extra->hyperlink = {};
}

inline bool CompactCell::empty() const noexcept
{
    return CellUtil::empty(*this);
}

inline void CompactCell::setGraphicsRendition(GraphicsRendition sgr) noexcept
{
    CellUtil::applyGraphicsRendition(sgr, *this);
}

// }}}
// {{{ free function implementations
inline bool beginsWith(std::u32string_view text, CompactCell const& cell) noexcept
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

} // namespace vtbackend

template <>
struct fmt::formatter<vtbackend::CompactCell>: fmt::formatter<std::string>
{
    auto format(vtbackend::CompactCell const& cell, format_context& ctx) -> format_context::iterator
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
