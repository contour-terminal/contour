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

#include <terminal_renderer/GridMetrics.h>
#include <terminal_renderer/Renderer.h>

#include <terminal/Color.h>
#include <terminal/Process.h>
#include <terminal/Terminal.h>

#include <crispy/point.h>
#include <crispy/size.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace terminal::view {

struct ShaderConfig;

/// OpenGL-Terminal Object.
class TerminalView : private Terminal::Events {
  public:
    class Events {
      public:
        virtual ~Events() = default;

        virtual void bell() {}
        virtual void bufferChanged(ScreenType) {}
        virtual void renderBufferUpdated() = 0;
        virtual void screenUpdated() {}
        virtual void requestCaptureBuffer(int /*_absoluteStartLine*/, int /*_lineCount*/) {}
        virtual void setFontDef(FontDef const& /*_fontDef*/) {}
        virtual void copyToClipboard(std::string_view const& /*_data*/) {}
        virtual void dumpState() {}
        virtual void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) {}
        virtual void reply(std::string_view const& /*_response*/) {}
        virtual void onClosed() {}
        virtual void onSelectionComplete() {}
        virtual void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) {}
        virtual void setWindowTitle(std::string_view const& /*_title*/) {}
        virtual void setTerminalProfile(std::string const& /*_configProfileName*/) {}
    };

    TerminalView(std::chrono::steady_clock::time_point _now,
                 Events& _events,
                 std::optional<size_t> _maxHistoryLineCount,
                 std::string const& _wordDelimiters,
                 Modifier _mouseProtocolSuppressModifier,
                 crispy::Point _logicalDpi,
                 renderer::FontDescriptions const& _fontDescriptions,
                 CursorShape _cursorShape,
                 CursorDisplay _cursorDisplay,
                 std::chrono::milliseconds _cursorBlinkInterval,
                 terminal::ColorPalette _colorPalette,
                 terminal::Opacity _backgroundOpacity,
                 renderer::Decorator _hyperlinkNormal,
                 renderer::Decorator _hyperlinkHover,
                 std::unique_ptr<Pty> _client,
                 Process::ExecInfo const& _shell,
                 double _refreshRate);

    TerminalView(TerminalView const&) = delete;
    TerminalView(TerminalView&&) = delete;
    TerminalView& operator=(TerminalView const&) = delete;
    TerminalView& operator=(TerminalView&&) = delete;
    ~TerminalView() = default;

    void setRenderTarget(renderer::RenderTarget& _renderTarget);

    crispy::Size size() const noexcept { return size_; }

    crispy::Size screenSize() const noexcept
    {
        return size_ / gridMetrics().cellSize;
    }

    int cellWidth() const noexcept { return gridMetrics().cellSize.width; }
    int cellHeight() const noexcept { return gridMetrics().cellSize.height; }
    crispy::Size cellSize() const noexcept { return gridMetrics().cellSize; }

    /// Resizes the terminal view to the given number of pixels.
    ///
    /// It also computes the appropricate number of text lines and character columns
    /// and resizes the internal screen buffer as well as informs the connected
    /// PTY slave about the window resize event.
    void resize(crispy::Size _size);

    void updateFontMetrics();
    void setFonts(FontDef const& _fonts);
    bool setFontSize(text::font_size  _fontSize);
    bool setTerminalSize(crispy::Size _cells);
    void setBackgroundOpacity(terminal::Opacity _opacity) { renderer_.setBackgroundOpacity(_opacity); }
    void setHyperlinkDecoration(renderer::Decorator _normal, renderer::Decorator _hover) { renderer_.setHyperlinkDecoration(_normal, _hover); }

    /// Renders the screen buffer to the current OpenGL screen.
    uint64_t render(std::chrono::steady_clock::time_point const& _now, bool _pressure);

    /// Checks if there is still a slave connected to the PTY.
    bool alive() const;

    /// Waits until the PTY slave has terminated, and then closes the underlying terminal.
    ///
    /// The alive() test will fail after this call.
    Process::ExitStatus waitForProcessExit();

    Process const& process() const noexcept { return process_; }
    Process& process() noexcept { return process_; }
    Terminal const& terminal() const noexcept { return terminal_; }
    Terminal& terminal() noexcept { return terminal_; }

    renderer::Renderer& renderer() noexcept { return renderer_; }
    renderer::Renderer const& renderer() const noexcept { return renderer_; }
    renderer::GridMetrics const& gridMetrics() const noexcept { return renderer_.gridMetrics(); }

    void setColorPalette(terminal::ColorPalette const& _colors);

    struct WindowMargin {
        int left;
        int bottom;
    };

    WindowMargin computeMargin(crispy::Size _charCells, crispy::Size _pixels) const noexcept;

    constexpr WindowMargin const& windowMargin() const noexcept { return windowMargin_; }

  private:
    void requestCaptureBuffer(int _absoluteStartLine, int _lineCount) override;
    void bell() override;
    void bufferChanged(ScreenType) override;
    void renderBufferUpdated() override;
    void screenUpdated() override;
    FontDef getFontDef() override;
    void setFontDef(FontDef const& _fontSpec) override;
    void copyToClipboard(std::string_view const& _data) override;
    void dumpState() override;
    void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) override;
    void reply(std::string_view const& /*_response*/) override;
    void onClosed() override;
    void onSelectionComplete() override;
    void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) override;
    void setWindowTitle(std::string_view const& /*_title*/) override;
    void setTerminalProfile(std::string const& /*_configProfileName*/) override;
    void discardImage(Image const& /*_image*/) override;

  private:
    Events& events_;
    WindowMargin windowMargin_;

    text::font_size fontSize_;
    crispy::Size size_;                     // view size in pixels

    Terminal terminal_;
    Process process_;
    std::thread processExitWatcher_;

    renderer::Renderer renderer_;
};

} // namespace terminal::view
