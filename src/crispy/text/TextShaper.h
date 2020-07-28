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

#include <crispy/text/Font.h>
#include <crispy/reference.h>

#include <unicode/ucd.h> // Script

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

#include <string>
#include <string_view>

namespace crispy::text {

/**
 * Performs the actual text shaping.
 */
class TextShaper {
  public:
    TextShaper();
    ~TextShaper();

    /// Renders codepoints into glyph positions with the first font fully matching all codepoints.
    ///
    /// @param _script      the matching script for the given codepoints
    /// @param _font        the font list in priority order to be used for text shaping
    /// @param _advanceX    number of pixels to advance on X axis for each new glyph to be rendered
    /// @param _codepoints  array of codepoints to be shaped
    /// @param _size        number of codepoints and clusters
    /// @param _clusters    cluster assignments for each codepoint (must be of equal size of @p _code_codepoints)
    /// @param _clusterGap  value that will be subtracted from every cluster when constructing glyph positions
    ///
    /// @returns pointer to the shape result
    GlyphPositionList shape(unicode::Script _script,
                            FontList const& _font,
                            int _advanceX,
                            int _size,
                            char32_t const* _codepoints,
                            int const* _clusters,
                            int _clusterGap = 0);

    /// Replaces all missing glyphs with the missing-glyph glyph.
    void replaceMissingGlyphs(Font& _font, GlyphPositionList& _result);

    // Clears the internal font cache.
    void clearCache();

  private:
    /// Performs text shaping for given text using the given font.
    bool shape(int _size,
               char32_t const* _codepoints,
               int const* _clusters,
               int _clusterGap,
               unicode::Script _script,
               Font& _font,
               int _advanceX,
               reference<GlyphPositionList> _result);

  private:
    hb_buffer_t* hb_buf_;
    std::unordered_map<Font const*, hb_font_t*> hb_fonts_ = {};
};

} // end namespace
