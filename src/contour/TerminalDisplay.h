/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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

#include <contour/Config.h>
#include <contour/helper.h>

#include <terminal/InputGenerator.h>
#include <terminal/Image.h>
#include <terminal/ScreenEvents.h>

#include <crispy/point.h>
#include <crispy/size.h>

#include <chrono>
#include <functional>

namespace contour {

/**
 * VT Display interface.
 *
 * Must be implementible by:
 * - GUI OpenGL window or widget (e.g. QOpenGLWindow or QOpenGLWidget)
 * - server side display, keeping track of current state pushing it to the clients.
 */
class TerminalDisplay
{
public:
    virtual ~TerminalDisplay() = default;

    /// Ensures @p _fn is being executed within the thread context of the display.
    virtual void post(std::function<void()> _fn) = 0;

    // Close the display (regardless of whether the PTY is closed or not).
    virtual void closeDisplay() = 0;

    // Attributes
    virtual double refreshRate() const = 0;
    virtual crispy::Point screenDPI() const = 0;
    virtual bool isFullScreen() const = 0;
    virtual crispy::ImageSize pixelSize() const = 0;
    virtual crispy::ImageSize cellSize() const = 0;

    // (user requested) actions
    virtual bool requestPermission(config::Permission _allowedByConfig, std::string_view _topicText) = 0;
    virtual bool setFontSize(text::font_size _size) = 0;
    virtual bool setScreenSize(terminal::PageSize _size) = 0;
    virtual terminal::FontDef getFontDef() = 0;
    virtual void bell() = 0;
    virtual void copyToClipboard(std::string_view _data) = 0;
    virtual void dumpState() = 0;
    virtual void notify(std::string_view _title, std::string_view _body) = 0;
    virtual void resizeWindow(terminal::LineCount, terminal::ColumnCount) = 0;
    virtual void resizeWindow(terminal::Width, terminal::Height) = 0;
    virtual void setBackgroundBlur(bool _enabled) = 0;
    virtual void setFonts(terminal::renderer::FontDescriptions _fontDescriptions) = 0;
    virtual void setHyperlinkDecoration(terminal::renderer::Decorator _normal, terminal::renderer::Decorator _hover) = 0;
    virtual void setMouseCursorShape(MouseCursorShape _shape) = 0;
    virtual void setWindowFullScreen() = 0;
    virtual void setWindowMaximized() = 0;
    virtual void setWindowNormal() = 0;
    virtual void setWindowTitle(std::string_view _title) = 0;
    virtual void toggleFullScreen() = 0;
    virtual void setBackgroundOpacity(terminal::Opacity _opacity) = 0;

    // terminal events
    virtual void bufferChanged(terminal::ScreenType) = 0; // primary/alt buffer has flipped
    virtual void discardImage(terminal::Image const&) = 0; // the given image is not in use anymore
    virtual void onSelectionCompleted() = 0; // a visual selection has completed
    virtual void renderBufferUpdated() = 0; // notify on RenderBuffer updates
    virtual void scheduleRedraw() = 0; //!< forced redraw of the screen
};

}
