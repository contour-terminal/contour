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

#include <terminal_view/Renderer.h>
#include <terminal_view/FontConfig.h>

#include <terminal/Color.h>
#include <terminal/Logger.h>
#include <terminal/Process.h>
#include <terminal/Size.h>
#include <terminal/Terminal.h>

#include <crispy/text/Font.h>

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
        virtual void commands() {}
        virtual void copyToClipboard(std::string_view const& /*_data*/) {}
        virtual void dumpState() {}
        virtual void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) {}
        virtual void onClosed() {}
        virtual void onSelectionComplete() {}
        virtual void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) {}
        virtual void setWindowTitle(std::string_view const& /*_title*/) {}
    };

    TerminalView(std::chrono::steady_clock::time_point _now,
                 Events& _events,
                 std::optional<size_t> _maxHistoryLineCount,
                 std::string const& _wordDelimiters,
                 FontConfig const& _fonts,
                 CursorShape _cursorShape,
                 CursorDisplay _cursorDisplay,
                 std::chrono::milliseconds _cursorBlinkInterval,
                 terminal::ColorProfile _colorProfile,
                 terminal::Opacity _backgroundOpacity,
                 Decorator _hyperlinkNormal,
                 Decorator _hyperlinkHover,
                 std::unique_ptr<Pty> _client,
                 Process::ExecInfo const& _shell,
                 QMatrix4x4 const& _projectionMatrix,
                 ShaderConfig const& _backgroundShaderConfig,
                 ShaderConfig const& _textShaderConfig,
                 Logger _logger);

    TerminalView(TerminalView const&) = delete;
    TerminalView(TerminalView&&) = delete;
    TerminalView& operator=(TerminalView const&) = delete;
    TerminalView& operator=(TerminalView&&) = delete;
    ~TerminalView() = default;

    int cellWidth() const noexcept { return fonts_.regular.first.get().maxAdvance(); }
    int cellHeight() const noexcept { return fonts_.regular.first.get().lineHeight(); }
    Size cellSize() const noexcept { return Size{cellWidth(), cellHeight()}; }

    Size screenSize() const noexcept
    {
        return Size{
            size_.width / fonts_.regular.first.get().maxAdvance(),
            size_.height / fonts_.regular.first.get().lineHeight()
        };
    }

    /// Resizes the terminal view to the given number of pixels.
    ///
    /// It also computes the appropricate number of text lines and character columns
    /// and resizes the internal screen buffer as well as informs the connected
    /// PTY slave about the window resize event.
    void resize(int _width, int _height);

    void setFont(FontConfig const& _fonts);
    bool setFontSize(int _fontSize);
    bool setTerminalSize(Size _cells);
    void setCursorShape(CursorShape _shape);
    void setBackgroundOpacity(terminal::Opacity _opacity) { renderer_.setBackgroundOpacity(_opacity); }
    void setHyperlinkDecoration(Decorator _normal, Decorator _hover) { renderer_.setHyperlinkDecoration(_normal, _hover); }
    void setProjection(QMatrix4x4 const& _projectionMatrix) { return renderer_.setProjection(_projectionMatrix); }

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

    Renderer const& renderer() const { return renderer_; }

    void setColorProfile(terminal::ColorProfile const& _colors);

    struct WindowMargin {
        int left;
        int bottom;
    };

    WindowMargin computeMargin(Size const& ws, unsigned _width, unsigned _height) const noexcept;

    constexpr WindowMargin const& windowMargin() const noexcept { return windowMargin_; }

  private:
    std::optional<RGBColor> requestDynamicColor(DynamicColorName _name) override;
    void bell() override;
    void bufferChanged(ScreenType) override;
    void commands() override;
    void copyToClipboard(std::string_view const& _data) override;
    void dumpState() override;
    void notify(std::string_view const& /*_title*/, std::string_view const& /*_body*/) override;
    void onClosed() override;
    void onSelectionComplete() override;
    void resetDynamicColor(DynamicColorName /*_name*/) override;
    void resizeWindow(int /*_width*/, int /*_height*/, bool /*_unitInPixels*/) override;
    void setDynamicColor(DynamicColorName, RGBColor const&) override;
    void setWindowTitle(std::string_view const& /*_title*/) override;
    void discardImage(Image const& /*_image*/) override;

  private:
    Events& events_;
    Logger logger_;
    FontConfig fonts_;
    Size size_;
    WindowMargin windowMargin_;

    Renderer renderer_;

    Terminal terminal_;
    Process process_;
    std::thread processExitWatcher_;

    ColorProfile colorProfile_;
    ColorProfile defaultColorProfile_;
};

} // namespace terminal::view
