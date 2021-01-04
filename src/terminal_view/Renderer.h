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

#include <terminal_view/BackgroundRenderer.h>
#include <terminal_view/CursorRenderer.h>
#include <terminal_view/DecorationRenderer.h>
#include <terminal_view/ImageRenderer.h>
#include <terminal_view/TextRenderer.h>

#include <terminal_view/RenderMetrics.h>
#include <terminal_view/ScreenCoordinates.h>
#include <terminal_view/ShaderConfig.h>

#include <terminal_view/OpenGLRenderer.h>

#include <terminal/Terminal.h>

#include <crispy/text/Font.h>

#include <fmt/format.h>

#include <chrono>
#include <memory>
#include <vector>
#include <utility>

namespace terminal::view {

struct ShaderConfig;

/**
 * Renders a terminal's screen to the current OpenGL context.
 */
class Renderer {
  public:
    /** Constructs a Renderer instances.
     *
     * @p _fonts reference to the set of loaded fonts to be used for rendering text.
     * @p _colorProfile user-configurable color profile to use to map terminal colors to.
     * @p _projectionMatrix projection matrix to apply to the rendered scene when rendering the screen.
     */
    Renderer(Size const& _screenSize,
             FontConfig const& _fonts,
             ColorProfile _colorProfile,
             Opacity _backgroundOpacity,
             Decorator _hyperlinkNormal,
             Decorator _hyperlinkHover,
             ShaderConfig const& _backgroundShaderConfig,
             ShaderConfig const& _textShaderConfig,
             QMatrix4x4 const& _projectionMatrix);

    int cellHeight() const noexcept { return fonts_.regular.first.get().lineHeight(); }
    int cellWidth() const noexcept { return fonts_.regular.first.get().maxAdvance(); }
    Size cellSize() const noexcept { return Size{cellWidth(), cellHeight()}; }

    void setColorProfile(ColorProfile const& _colors);
    void setBackgroundOpacity(terminal::Opacity _opacity);
    void setFont(FontConfig const& _fonts);
    bool setFontSize(int _fontSize);
    void setProjection(QMatrix4x4 const& _projectionMatrix);

    void setHyperlinkDecoration(Decorator _normal, Decorator _hover)
    {
        decorationRenderer_.setHyperlinkDecoration(_normal, _hover);
    }

    constexpr void setScreenSize(Size const& _screenSize) noexcept
    {
        screenCoordinates_.screenSize = _screenSize;
    }

    constexpr void setMargin(int _leftMargin, int _bottomMargin) noexcept
    {
        renderTarget_.setMargin(_leftMargin, _bottomMargin);
        screenCoordinates_.leftMargin = _leftMargin;
        screenCoordinates_.bottomMargin = _bottomMargin;
    }

    /**
     * Renders the given @p _terminal to the current OpenGL context.
     *
     * @p _now The time hint to use when rendering the eventually blinking cursor.
     */
    uint64_t render(Terminal& _terminal,
                    std::chrono::steady_clock::time_point _now,
                    terminal::Coordinate const& _currentMousePosition,
                    bool _pressure);

    RenderMetrics const& metrics() const noexcept { return metrics_; }

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

    void discardImage(Image const& _image);

    void clearCache();

    void dumpState(std::ostream& _textOutput) const;

  private:
    /// Invoked internally by render() function.
    uint64_t renderInternalNoFlush(Terminal& _terminal,
                                   std::chrono::steady_clock::time_point _now,
                                   terminal::Coordinate const& _currentMousePosition,
                                   bool _pressure);

    void renderCell(Coordinate const& _pos, Cell const& _cell, bool _reverseVideo, bool _selected);
    void renderCursor(Terminal const& _terminal);

    void executeImageDiscards();

  private:
    RenderMetrics metrics_;

    ScreenCoordinates screenCoordinates_;

    ColorProfile colorProfile_;
    Opacity backgroundOpacity_;

    FontConfig fonts_;

    std::mutex imageDiscardLock_;               //!< Lock guard for accessing discardImageQueue_.
    std::vector<Image::Id> discardImageQueue_;  //!< List of images to be discarded.

    OpenGLRenderer renderTarget_;

    BackgroundRenderer backgroundRenderer_;
    ImageRenderer imageRenderer_;
    TextRenderer textRenderer_;
    DecorationRenderer decorationRenderer_;
    CursorRenderer cursorRenderer_;
};

} // end namespace
