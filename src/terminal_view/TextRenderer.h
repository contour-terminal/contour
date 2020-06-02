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

#include <crispy/text/TextShaper.h>
#include <crispy/text/TextRenderer.h>
#include <crispy/text/Font.h>

#include <unicode/run_segmenter.h>

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>

#include <functional>
#include <vector>

namespace terminal::view {

struct RenderMetrics;

/// Text Rendering Pipeline
class TextRenderer : public QOpenGLFunctions {
  public:
    TextRenderer(RenderMetrics& _renderMetrics,
                 ScreenCoordinates const& _screenCoordinates,
                 ColorProfile const& _colorProfile,
                 crispy::text::FontList const& _regularFont,
                 crispy::text::FontList const& _emojiFont,
                 ShaderConfig const& _shaderConfig);

    void setFont(crispy::text::Font& _font, crispy::text::FontFallbackList const& _fallback);
    void setProjection(QMatrix4x4 const& _projectionMatrix);
    void setColorProfile(ColorProfile const& _colorProfile);

    void schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell);
    void flushPendingSegments();
    void execute();

    void clearCache();

  private:
    size_t cellHeight() const noexcept { return regularFont_.first.get().lineHeight(); }
    size_t cellWidth() const noexcept { return regularFont_.first.get().maxAdvance(); }
    ColorProfile const& colorProfile() const noexcept { return colorProfile_; }

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
    crispy::text::FontList regularFont_;
    crispy::text::FontList emojiFont_;

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
    std::unordered_map<std::u32string_view, crispy::text::GlyphPositionList> cache_;

    // target surface rendering
    //
    QMatrix4x4 projectionMatrix_;
    crispy::text::TextShaper textShaper_;
    std::unique_ptr<QOpenGLShaderProgram> textShader_;
    int textProjectionLocation_;
    crispy::text::TextRenderer renderer_;
};

} // end namespace
