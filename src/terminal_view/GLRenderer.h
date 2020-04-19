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

#include <terminal/Logger.h>
#include <terminal/Terminal.h>

#include <terminal_view/CellBackground.h>
#include <terminal_view/FontManager.h>
#include <terminal_view/GLCursor.h>
#include <terminal_view/GLTextShaper.h>

#include <QPoint>
#include <QMatrix2x4>
#include <QOpenGLFunctions>

#include <fmt/format.h>

#include <chrono>
#include <vector>
#include <utility>

namespace terminal::view {

struct ShaderConfig;

/**
 * Renders a terminal's screen to the current OpenGL context.
 */
class GLRenderer : public QOpenGLFunctions {
  public:
    /** Constructs a GLRenderer instances.
     *
     * @p _logger the logging instance to use when logging is needed during rendering.
     * @p _regularFont reference to the font to use for rendering standard text.
     * @p _colorProfile user-configurable color profile to use to map terminal colors to.
     * @p _projectionMatrix projection matrix to apply to the rendered scene when rendering the screen.
     */
    GLRenderer(Logger _logger,
               Font& _regularFont,
               ColorProfile _colorProfile,
               Opacity _backgroundOpacity,
               ShaderConfig const& _backgroundShaderConfig,
               ShaderConfig const& _textShaderConfig,
               ShaderConfig const& _cursorShaderConfig,
               QMatrix4x4 const& _projectionMatrix);

    size_t cellHeight() const noexcept { return regularFont_.get().lineHeight(); }
    size_t cellWidth() const noexcept { return regularFont_.get().maxAdvance(); }

    void setColorProfile(ColorProfile const& _colors);
    void setBackgroundOpacity(terminal::Opacity _opacity);
    void setFont(Font& _font);
    bool setFontSize(unsigned int _fontSize);
    void setProjection(QMatrix4x4 const& _projectionMatrix);

    /**
     * Renders the given @p _terminal to the current OpenGL context.
     *
     * @p _now The time hint to use when rendering the eventually blinking cursor.
     */
    uint64_t render(Terminal const& _terminal, std::chrono::steady_clock::time_point _now);

    struct Metrics {
        unsigned renderTextGroup = 0;
        unsigned cellBackgroundRenderCount = 0;

        void clear()
        {
            renderTextGroup = 0;
            cellBackgroundRenderCount = 0;
        }

        std::string to_string() const
        {
            return fmt::format(
                "text renders: {}, background renders: {}",
                renderTextGroup,
                cellBackgroundRenderCount);
        }
    };

    Metrics const& metrics() const noexcept { return metrics_; }

    // Converts given RGBColor with its given opacity to a 4D-vector of values between 0.0 and 1.0
    static constexpr QVector4D canonicalColor(RGBColor const& _rgb, Opacity _opacity = Opacity::Opaque)
    {
        return QVector4D{
            static_cast<float>(_rgb.red) / 255.0f,
            static_cast<float>(_rgb.green) / 255.0f,
            static_cast<float>(_rgb.blue) / 255.0f,
            static_cast<float>(_opacity) / 255.0f
        };
    }

  private:
    void fillTextGroup(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell, WindowSize const& _screenSize);
    void fillBackgroundGroup(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::Cell const& _cell, WindowSize const& _screenSize);

    void renderTextGroup(WindowSize const& _screenSize);
    void renderPendingBackgroundCells(WindowSize const& _screenSize);

    QPoint makeCoords(cursor_pos_t _col, cursor_pos_t _row, WindowSize const& _screenSize) const;
    std::pair<RGBColor, RGBColor> makeColors(ScreenBuffer::GraphicsAttributes const& _attributes) const;

  private:
    Metrics metrics_;

    /// Holds an array of directly connected characters on a single line that all share the same visual attributes.
    struct PendingDraw {
        enum class State { Empty, Filling };
        cursor_pos_t lineNumber{};
        cursor_pos_t startColumn{};
        ScreenBuffer::GraphicsAttributes attributes{};
        std::vector<char32_t> text{};
        State state = State::Empty;

        void reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attributes)
        {
            lineNumber = _row;
            startColumn = _col;
            attributes = _attributes;
            text.clear();
        }
    };

    struct PendingBackgroundDraw
    {
        RGBColor color;                 // The background color the draw is pending for.
        cursor_pos_t lineNumber{};      // The line this color has to be drawn on.
        cursor_pos_t startColumn{};     // The first column to start drawing.
        cursor_pos_t endColumn{};       // The last column to draw.

        void reset(RGBColor const& _color, cursor_pos_t _lineNo, cursor_pos_t _col)
        {
            color = _color;
            lineNumber = _lineNo;
            startColumn = _col;
            endColumn = _col;
        }

        bool empty() const noexcept
        {
            return lineNumber == 0;
        }
    };

    PendingDraw pendingDraw_;
    PendingBackgroundDraw pendingBackgroundDraw_;
    Margin margin_{};
    Logger logger_;

    ColorProfile colorProfile_;
    Opacity backgroundOpacity_;

    std::reference_wrapper<Font> regularFont_;
    GLTextShaper textShaper_;
    CellBackground cellBackground_;
    GLCursor cursor_;
};

}
