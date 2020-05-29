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

#include <crispy/text/Font.h>
#include <crispy/reference.h>

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

namespace crispy::text {

/**
 * Performs the actual text shaping.
 */
class TextShaper {
  public:
    TextShaper();
    ~TextShaper();

    /// Renders codepoints into glyph positions with the first font fully matching all codepoints.
    void shape(FontList const& _font, CodepointSequence const& _codes, reference<GlyphPositionList> _result);

    /// Replaces all missing glyphs with the missing-glyph glyph.
    void replaceMissingGlyphs(Font& _font, reference<GlyphPositionList> _result);

    // Clears the internal font cache.
    void clearCache();

  private:
    /// Performs text shaping for given text using the given font.
    bool shape(CodepointSequence const& _codes, Font& _font, reference<GlyphPositionList> _result);

  private:
    hb_buffer_t* hb_buf_;
    std::unordered_map<Font const*, hb_font_t*> hb_fonts_ = {};
};

} // end namespace
