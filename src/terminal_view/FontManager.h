/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H

#if defined(_MSC_VER)
// XXX purely for IntelliSense
#include <freetype/freetype.h>
#endif

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace terminal::view {

enum class FontStyle {
    Regular = 0,
    Bold = 1,
    Italic = 2,
    BoldItalic = 3,
};

constexpr FontStyle operator|(FontStyle lhs, FontStyle rhs)
{
    return static_cast<FontStyle>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr FontStyle& operator|=(FontStyle& lhs, FontStyle rhs)
{
    lhs = lhs | rhs;
    return lhs;
}

class Font {
  public:
    Font(FT_Library _ft, std::string const& _fontPath, unsigned int _fontSize);
    Font(Font const&) = delete;
    Font& operator=(Font const&) = delete;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;
    ~Font();

    unsigned int fontSize() const noexcept { return fontSize_; }

    void setFontSize(unsigned int _fontSize);

    unsigned int lineHeight() const noexcept { return face_->size->metrics.height >> 6; }

    unsigned int maxAdvance() const noexcept {
        // FIXME: should be that line, but doesn't work on FiraCode (Regular) for some reason.
        //return face_->size->metrics.max_advance >> 6;

        auto const a = face_->glyph->advance.x >> 6; // only works if glyph was loaded
        auto const b = face_->size->metrics.max_advance >> 6;
        if (a)
            return a;
        else
            return b;
    }

    unsigned int baseline() const noexcept { return abs(face_->size->metrics.descender) >> 6; }

    bool contains(char32_t _char) const noexcept { return FT_Get_Char_Index(face_, _char) != 0; }

    bool isFixedWidth() const noexcept { return face_->face_flags & FT_FACE_FLAG_FIXED_WIDTH; }

    void loadGlyphByChar(char32_t _char) { loadGlyphByIndex(FT_Get_Char_Index(face_, _char)); }
    void loadGlyphByIndex(unsigned int _glyphIndex);

    // well yeah, if it's only bitmap we still need, we can expose it and then [[deprecated]] this.
    FT_Face operator->() noexcept { return face_; }

    operator FT_Face () noexcept { return face_; }

    struct GlyphPosition {
        unsigned int x;
        unsigned int y;
        unsigned int codepoint;
    };
    /// Renders text into glyph positions of this font.
    void render(std::vector<char32_t> const& _chars, std::vector<GlyphPosition>& _result);

  private:
    FT_Library ft_;
    FT_Face face_;
    hb_font_t* hb_font_;
    hb_buffer_t* hb_buf_;
    unsigned int fontSize_;
};

/// API for managing multiple fonts.
class FontManager {
  public:
    FontManager();
    FontManager(FontManager&&) = delete;
    FontManager(FontManager const&) = delete;
    FontManager& operator=(FontManager&&) = delete;
    FontManager& operator=(FontManager const&) = delete;
    ~FontManager();

    Font& load(std::string const& _fontPattern, unsigned int _fontSize);

  private:
    FT_Library ft_;
    std::unordered_map<std::string, Font> fonts_;
};

} // namespace terminal::view
