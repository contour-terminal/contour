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

#include <terminal/Screen.h>
#include <terminal_view/ScreenCoordinates.h>
#include <terminal_view/ShaderConfig.h>
#include <terminal_view/FontConfig.h>

#include <crispy/text/TextShaper.h>
#include <crispy/text/TextRenderer.h>
#include <crispy/text/Font.h>
#include <crispy/Atlas.h>
#include <crispy/AtlasRenderer.h>

#include <unicode/run_segmenter.h>

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#include <functional>
#include <vector>

namespace terminal::view {
    struct CacheKey {
        std::u32string_view text;
        CharacterStyleMask styles;

        bool operator==(CacheKey const& _rhs) const noexcept
        {
            return text == _rhs.text && styles == _rhs.styles;
        }

        bool operator!=(CacheKey const& _rhs) const noexcept
        {
            return !(*this == _rhs);
        }

        bool operator<(CacheKey const& _rhs) const noexcept
        {
            if (text < _rhs.text)
                return true;

            if (static_cast<unsigned>(styles) < static_cast<unsigned>(_rhs.styles))
                return true;

            return false;
        }
    };
}

namespace std {
    template <>
    struct hash<terminal::view::CacheKey> {
        size_t operator()(terminal::view::CacheKey const& _key) const noexcept
        {
            return hash<std::u32string_view>{}(_key.text)
                + static_cast<size_t>(_key.styles); // TODO maybe use FNV for both?
        }
    };
}

namespace terminal::view {

struct RenderMetrics;

/// Text Rendering Pipeline
class TextRenderer {
  public:
    TextRenderer(RenderMetrics& _renderMetrics,
                 crispy::atlas::CommandListener& _commandListener,
                 crispy::atlas::TextureAtlasAllocator& _textureAtlasAllocator,
                 crispy::atlas::TextureAtlasAllocator& _colorAtlasAllocator,
                 ScreenCoordinates const& _screenCoordinates,
                 ColorProfile const& _colorProfile,
                 FontConfig const& _fonts);

    void setFont(FontConfig const& _fonts);

    void setCellSize(crispy::text::CellSize const& _cellSize);
    void setColorProfile(ColorProfile const& _colorProfile);

    void schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell);
    void flushPendingSegments();
    void finish();

    void clearCache();

  private:
    void reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr);
    void extend(ScreenBuffer::Cell const& _cell, cursor_pos_t _column);
    crispy::text::GlyphPositionList prepareRun(unicode::run_segmenter::range const& _range);

    crispy::text::GlyphPositionList const& cachedGlyphPositions();
    crispy::text::GlyphPositionList requestGlyphPositions();

  private:
    // general properties
    //
    RenderMetrics& renderMetrics_;
    ScreenCoordinates const& screenCoordinates_;
    ColorProfile colorProfile_; // TODO: make const&, maybe reference_wrapper<>?
    FontConfig fonts_;

    // text run segmentation
    //
    enum class State { Empty, Filling };
    State state_ = State::Empty;
    cursor_pos_t row_ = 1;
    cursor_pos_t startColumn_ = 1;
    ScreenBuffer::GraphicsAttributes attributes_ = {};
    std::vector<char32_t> codepoints_{};
    std::vector<unsigned> clusters_{};
    unsigned clusterOffset_ = 0;

    // text shaping cache
    //
    std::unordered_map<std::u32string_view, std::u32string> cacheKeyStorage_;
    std::unordered_map<CacheKey, crispy::text::GlyphPositionList> cache_;

    // target surface rendering
    //
    crispy::text::CellSize cellSize_;
    crispy::text::TextShaper textShaper_;
    crispy::text::TextRenderer renderer_;
};

} // end namespace
