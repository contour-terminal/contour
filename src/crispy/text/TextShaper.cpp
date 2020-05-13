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
#include <crispy/text/TextShaper.h>
#include <crispy/times.h>
#include <crispy/algorithm.h>

#include <fmt/format.h>

#include <iostream>
#include <functional>

using namespace std;

namespace crispy::text {

namespace
{
    constexpr bool glyphMissing(GlyphPosition const& _gp) noexcept
    {
        return _gp.glyphIndex == 0;
    }
}

TextShaper::TextShaper()
{
    hb_buf_ = hb_buffer_create();
}

TextShaper::~TextShaper()
{
    clearCache();
    hb_buffer_destroy(hb_buf_);
}

GlyphPositionList const* TextShaper::shape(FontList& _font, CodepointSequence const& _codes)
{
    if (auto i = cache_.find(_codes); i != cache_.end())
        return &i->second;

    GlyphPositionList result;
    if (shape(_codes, _font.first.get(), ref(result)))
        return &(cache_[_codes] = move(result));

    size_t i = 1;
    for (reference_wrapper<Font>& fallback : _font.second)
    {
        if (shape(_codes, fallback.get(), ref(result)))
            return &(cache_[_codes] = move(result));
        ++i;
    }

    string joinedCodes;
    for (Codepoint code : _codes)
    {
        if (!joinedCodes.empty())
            joinedCodes += " ";
        joinedCodes += fmt::format("{:<6x}", unsigned(code.value));
    }
    cerr << fmt::format("Shaping failed for {} codepoints: {}\n", _codes.size(), joinedCodes);

    shape(_codes, _font.first.get(), ref(result));
    replaceMissingGlyphs(_font.first.get(), result);
    return &(cache_[_codes] = move(result));
}

void TextShaper::clearCache()
{
    cache_.clear();
    cache2_.clear();

    for ([[maybe_unused]] auto [_, hbf] : hb_fonts_)
        hb_font_destroy(hbf);

    hb_fonts_.clear();
}

bool TextShaper::shape(CodepointSequence const& _codes, Font& _font, reference<GlyphPositionList> _result)
{
    hb_buffer_clear_contents(hb_buf_);

    for (Codepoint const& codepoint : _codes)
        hb_buffer_add(hb_buf_, codepoint.value, codepoint.cluster);

    hb_buffer_set_content_type(hb_buf_, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_direction(hb_buf_, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buf_, HB_SCRIPT_COMMON);
    hb_buffer_set_language(hb_buf_, hb_language_get_default());
    hb_buffer_guess_segment_properties(hb_buf_);

    hb_font_t* hb_font = nullptr;
    if (auto i = hb_fonts_.find(&_font); i != hb_fonts_.end())
        hb_font = i->second;
    else
    {
        hb_font = hb_ft_font_create_referenced(_font);
        hb_fonts_[&_font] = hb_font;
    }

    hb_shape(hb_font, hb_buf_, nullptr, 0);

    hb_buffer_normalize_glyphs(hb_buf_);

    unsigned const glyphCount = hb_buffer_get_length(hb_buf_);
    hb_glyph_info_t const* info = hb_buffer_get_glyph_infos(hb_buf_, nullptr);
    hb_glyph_position_t const* pos = hb_buffer_get_glyph_positions(hb_buf_, nullptr);

    _result.get().clear();
    _result.get().reserve(glyphCount);

    unsigned int cx = 0;
    unsigned int cy = 0;
    for (unsigned const i : times(glyphCount))
    {
        // TODO: maybe right in here, apply incremented cx/xy only if cluster number has changed?
        _result.get().emplace_back(GlyphPosition{
            _font,
            cx + (pos[i].x_offset >> 6),
            cy + (pos[i].y_offset >> 6),
            info[i].codepoint,
            info[i].cluster
        });

        if (pos[i].x_advance)
            cx += _font.maxAdvance();

        cy += pos[i].y_advance >> 6;
    }

    return not any_of(_result.get(), glyphMissing);
}

void TextShaper::replaceMissingGlyphs(Font& _font, GlyphPositionList& _result)
{
    auto constexpr missingGlyphId = 0xFFFDu;
    auto const missingGlyph = FT_Get_Char_Index(_font, missingGlyphId);

    if (missingGlyph)
    {
        for (auto i : times(_result.size()))
            if (glyphMissing(_result[i]))
                _result[i].glyphIndex = missingGlyph;
    }
}

} // end namespace
