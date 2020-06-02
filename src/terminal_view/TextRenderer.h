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

/// Text Rendering Pipeline
class TextRenderer : public QOpenGLFunctions {
  public:
    TextRenderer(ScreenCoordinates const& _screenCoordinates,
                 ColorProfile const& _colorProfile,
                 crispy::text::FontList const& _regularFont,
                 crispy::text::FontList const& _emojiFont,
                 ShaderConfig const& _shaderConfig);

    void clearCache();

    size_t cellHeight() const noexcept { return regularFont_.first.get().lineHeight(); }
    size_t cellWidth() const noexcept { return regularFont_.first.get().maxAdvance(); }

    void setFont(crispy::text::Font& _font, crispy::text::FontFallbackList const& _fallback);
    void setProjection(QMatrix4x4 const& _projectionMatrix);
    void setColorProfile(ColorProfile const& _colorProfile);
    ColorProfile const& colorProfile() const noexcept { return colorProfile_; }

    void schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell);
    void flushPendingSegments();
    void execute();

  private:
    void reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr);
    void extend(ScreenBuffer::Cell const& _cell, cursor_pos_t _column);
    void prepareRun(unicode::run_segmenter::range const& _range);

  private:
    enum class State { Empty, Filling };

    ScreenCoordinates const& screenCoordinates_;
    ColorProfile colorProfile_;
    WindowSize screenSize_;
    crispy::text::FontList regularFont_;
    crispy::text::FontList emojiFont_;

    State state_ = State::Empty;
    cursor_pos_t row_ = 1;
    cursor_pos_t startColumn_ = 1;
    ScreenBuffer::GraphicsAttributes attributes_ = {};
    std::vector<char32_t> codepoints_{};
    std::vector<unsigned> clusters_{};
    unsigned clusterOffset_ = 0;

    //.
    QMatrix4x4 projectionMatrix_;
    crispy::text::TextShaper textShaper_;
    std::unique_ptr<QOpenGLShaderProgram> textShader_;
    int textProjectionLocation_;
    crispy::text::TextRenderer renderer_;
};

} // end namespace
