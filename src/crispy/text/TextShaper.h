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

#include <crispy/text/FontManager.h>

namespace crispy::text {

class TextShaper {
  public:
    TextShaper();
    ~TextShaper();

    /// Renders text into glyph positions of this font.
    GlyphPositionList const* shape(FontList& _font, CodepointSequence const& _codes);

    void clearCache();

    void replaceMissingGlyphs(Font& _font, GlyphPositionList& _result);

  private:
    bool shape(CodepointSequence const& _codes, Font& _font, reference<GlyphPositionList> result);

  private:
    hb_buffer_t* hb_buf_;

    std::unordered_map<Font const*, hb_font_t*> hb_fonts_ = {};
    std::unordered_map<CodepointSequence, GlyphPositionList> cache_ = {};
    std::unordered_map<CharSequence, GlyphPositionList> cache2_ = {};
};

} // end namespace
