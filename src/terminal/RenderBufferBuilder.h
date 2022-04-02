#pragma once

#include <terminal/RenderBuffer.h>
#include <terminal/Terminal.h>

#include <optional>

namespace terminal
{

/**
 * RenderBufferBuilder<Cell> renders the current screen state into a RenderBuffer.
 */
template <typename Cell>
class RenderBufferBuilder
{
  public:
    RenderBufferBuilder(Terminal const& terminal, RenderBuffer& output);

    void operator()(Cell const& _cell, LineOffset _line, ColumnOffset _column);

  private:
    std::optional<RenderCursor> renderCursor() const;

    static RenderCell makeRenderCell(ColorPalette const& _colorPalette,
                                     HyperlinkStorage const& _hyperlinks,
                                     Cell const& _cell,
                                     RGBColor fg,
                                     RGBColor bg,
                                     LineOffset _line,
                                     ColumnOffset _column);

    // clang-format off
    enum class State { Gap, Sequence };
    // clang-format on

    RenderBuffer& output;
    Terminal const& terminal;

    bool reverseVideo = terminal.isModeEnabled(terminal::DECMode::ReverseVideo);
    int prevWidth = 0;
    bool prevHasCursor = false;
    State state = State::Gap;
    LineOffset lineNr = LineOffset(0);
};

} // namespace terminal
