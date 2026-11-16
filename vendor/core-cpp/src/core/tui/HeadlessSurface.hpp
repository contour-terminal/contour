// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/ByteSinkOutput.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/ScreenSurface.hpp>

#include <expected>
#include <utility>

namespace core::tui
{

/// @brief A @c ScreenSurface with no terminal under it: a size and a byte sink.
///
/// What a @c Screen draws on in a program that has no terminal of its own -- a WebAssembly guest,
/// which hands each frame's bytes to its host. Every flushed byte goes to the sink, and nothing
/// reads a descriptor, probes, waits or asks the platform anything, on any platform.
///
/// It is for the fullscreen and fixed viewports. Nobody can be asked where the cursor is, so
/// @c queryCursorPosition() answers @c QueryUnanswered::NotAsked, and an inline viewport on this
/// surface falls back as it does on a terminal that stays silent: it assumes the cursor is on the
/// last row. The inline viewport's cursor query is the terminal's, and stays in core::tui.
class HeadlessSurface final: public ScreenSurface
{
  public:
    /// @brief Constructs a surface of @p size whose frames go to @p sink.
    /// @param size The surface's size, in cells.
    /// @param sink Receives every byte of every frame, in order; must not be empty.
    /// @param destination What the bytes are for; a terminal on the host by default, so a frame is
    ///        bracketed in synchronized output.
    HeadlessSurface(Size size,
                    ByteSink sink,
                    SinkDestination destination = SinkDestination::Terminal) noexcept;

    [[nodiscard]] auto columns() const noexcept -> int override;
    [[nodiscard]] auto rows() const noexcept -> int override;
    [[nodiscard]] auto output() noexcept -> TerminalOutput& override;

    /// @brief Answers that nobody was asked: there is no terminal to ask.
    /// @return @c QueryUnanswered::NotAsked, always.
    [[nodiscard]] auto queryCursorPosition() -> std::expected<std::pair<int, int>, QueryUnanswered> override;

    /// @brief Changes the surface's size; the next @c Screen::draw() repaints at the new one.
    ///
    /// Externally-driven geometry: the host decides the size, not the surface.
    ///
    /// @param size The new size, in cells.
    void resize(Size size) noexcept;

  private:
    ByteSinkOutput _output;
};

} // namespace core::tui
