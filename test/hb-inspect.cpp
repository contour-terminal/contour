/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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
#include <unicode/width.h>
#include <unicode/utf8.h>

#include <iostream>
#include <fstream>
#include <stdexcept>

#include <fmt/format.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ERRORS_H

#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

using namespace std;

static string freetypeErrorString(FT_Error _errorCode)
{
    #undef __FTERRORS_H__
    #define FT_ERROR_START_LIST     switch (_errorCode) {
    #define FT_ERRORDEF(e, v, s)    case e: return s;
    #define FT_ERROR_END_LIST       }
    #include FT_ERRORS_H
    return "(Unknown error)";
}

static unsigned computeMaxAdvance(FT_Face _face)
{
    if (FT_Load_Char(_face, 'M', FT_LOAD_BITMAP_METRICS_ONLY) == FT_Err_Ok)
        return _face->glyph->advance.x >> 6;

    unsigned long long maxAdvance = 0;
    unsigned count = 0;
    for (unsigned glyphIndex = 0; glyphIndex < _face->num_glyphs; ++glyphIndex)
    {
        if (FT_Load_Glyph(_face, glyphIndex, FT_LOAD_BITMAP_METRICS_ONLY) == FT_Err_Ok)// FT_LOAD_BITMAP_METRICS_ONLY);
        {
            maxAdvance += _face->glyph->advance.x >> 6;
            count++;
        }
    }

    return maxAdvance / count;
}

int main(int argc, char const* argv[])
{
    // ==================================================================================================
    // setup

    auto const fontSize = 32;
    string const textPath = argc > 1 ? argv[1] : "text.txt";

    string const fontPath = argc > 2 ? argv[2] :
#if 0
        "/home/trapni/.fonts/f/FiraCode_Regular.otf"
#else
        "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"
#endif
        ;

    string const text = [&]() {
        string total;
        fstream in(textPath);
        in.seekg(0, ios::end);
        fstream::pos_type const len = in.tellg();
        in.seekg(ios::beg);
        total.resize(len);
        in.read(total.data(), len);
        return total;
    }();

    u32string const text32 = unicode::from_utf8(text);

    // ==================================================================================================
    // print input
    cout << fmt::format("Input text of {} unicode codepoints ({} bytes in UTF-8):\n",
            text32.size(), text.size());
    for (unsigned i = 0; i < text32.size(); ++i)
        cout << fmt::format("{:>4}: codepoint:{:>08x} width:{}\n",
                i,
                (unsigned) text32[i],
                unicode::width(text32[i]));

    // ==================================================================================================
    // initialize
    FT_Library ft_;
    FT_Face face_;

    if (auto ec = FT_Init_FreeType(&ft_); ec != FT_Err_Ok)
        throw runtime_error{ "Failed to initialize FreeType. " + freetypeErrorString(ec) };

    if (auto ec = FT_New_Face(ft_, fontPath.c_str(), 0, &face_); ec != FT_Err_Ok)
        throw runtime_error{ "Failed to load font." + freetypeErrorString(ec) };

    if (auto ec = FT_Select_Charmap(face_, FT_ENCODING_UNICODE); ec != FT_Err_Ok)
        throw runtime_error{ string{"Failed to set charmap. "} + freetypeErrorString(ec) };

    bool const hasColor = FT_HAS_COLOR(face_);
    if (hasColor)
    {
        // FIXME i think this one can be omitted?
        FT_Error const ec = FT_Select_Size(face_, 0);
        if (ec != FT_Err_Ok)
            throw runtime_error{fmt::format("Failed to FT_Select_Size. {}", freetypeErrorString(ec))};
    }
    else
    {
        FT_Error const ec = FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(fontSize));
        if (ec)
            throw runtime_error{ string{"Failed to set font pixel size. "} + freetypeErrorString(ec) };
    }

    auto const [bitmapWidth, bitmapHeight] = [&]() -> tuple<int, int> {
        if (FT_IS_SCALABLE(face_))
            return { FT_MulFix(face_->bbox.xMax - face_->bbox.xMin, face_->size->metrics.x_scale) >> 6,
                     FT_MulFix(face_->bbox.yMax - face_->bbox.yMin, face_->size->metrics.y_scale) >> 6 };
        else
            return { face_->available_sizes[0].width,
                     face_->available_sizes[0].height };
    }();

    auto const maxAdvance = computeMaxAdvance(face_);

    // ==================================================================================================
    // shaping

    auto hb_font_ = hb_ft_font_create_referenced(face_);
    auto hb_buf_ = hb_buffer_create();

    hb_buffer_add_utf32(hb_buf_, (uint32_t const*)text32.data(), text32.size(), 0, text32.size());

    hb_buffer_set_direction(hb_buf_, HB_DIRECTION_LTR);
    hb_buffer_set_script(hb_buf_, HB_SCRIPT_COMMON);
    hb_buffer_set_language(hb_buf_, hb_language_get_default());
    hb_buffer_guess_segment_properties(hb_buf_);

    hb_shape(hb_font_, hb_buf_, nullptr, 0);

    hb_buffer_normalize_glyphs(hb_buf_); // needed?

    unsigned const glyphCount = hb_buffer_get_length(hb_buf_);
    hb_glyph_info_t* info = hb_buffer_get_glyph_infos(hb_buf_, nullptr);
    hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(hb_buf_, nullptr);

    // ==================================================================================================
    // print result

    cout << fmt::format("font: {} {}\n", fontPath, hasColor ? "(colors)" : "(monochrome)");
    cout << fmt::format("bitmap: {}x{}, maxAdvance:{}\n", bitmapWidth, bitmapHeight, maxAdvance);
    cout << fmt::format("shaping result: {} glyphs\n", glyphCount);
    unsigned cx = 0;
    unsigned cy = 0;
    for (unsigned i = 0; i < glyphCount; ++i)
    {
        auto const codepoint = unsigned(text32[info[i].cluster]);
        auto const glyphIndex = info[i].codepoint;
        auto const columnWidth = unicode::width(codepoint);
        cout << fmt::format("{:>4}: code:{:>08x} width:{} glyphIndex:{:<5} xoff:{:<3} yoff:{:<3} xadv:{:<3} yadv:{}\n",
                            info[i].cluster,
                            codepoint,
                            columnWidth,
                            glyphIndex,
                            pos[i].x_offset >> 6,
                            pos[i].y_offset >> 6,
                            pos[i].x_advance >> 6,
                            pos[i].y_advance >> 6);

        if (columnWidth)
            cx += maxAdvance;
        cy += pos[i].y_advance >> 6;
    }
    cout << '\n';
    cout << fmt::format("cx:{}, xy:{}\n", cx, cy);

    // ==================================================================================================
    // cleanup

    FT_Done_FreeType(ft_);
    return EXIT_SUCCESS;
}
