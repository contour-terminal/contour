// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/TerminalTestFixtures.hpp>

namespace vtbackend::test
{

vtbackend::RGBColor screenLineBackground(vtbackend::RenderBufferRef const& buf,
                                         vtbackend::LineOffset line) noexcept
{
    for (auto const& cell: buf.get().cells)
        if (cell.position.line == line && cell.position.column >= vtbackend::ColumnOffset(2))
            return cell.attributes.backgroundColor;
    for (auto const& renderLine: buf.get().lines)
        if (renderLine.lineOffset == line)
            // The batched RenderLine covers text (0..usedColumns) and fill (usedColumns..end)
            // with one attribute set each; the cursorline tint is applied uniformly to both, so
            // either represents the line background — prefer the fill (always present).
            return renderLine.fillAttributes.backgroundColor;
    return vtbackend::RGBColor {};
}

std::optional<vtbackend::LineOffset> renderedCursorLine(vtbackend::RenderBufferRef const& buf) noexcept
{
    if (!buf.get().cursor.has_value())
        return std::nullopt;
    return buf.get().cursor->position.line;
}

std::vector<DrawnGlyph> renderedLayout(vtbackend::RenderBufferRef const& buf, vtbackend::LineOffset line)
{
    auto result = std::vector<DrawnGlyph> {};

    for (auto const& cell: buf.get().cells)
        if (cell.position.line == line && !cell.codepoints.empty() && cell.codepoints != U" ")
            result.emplace_back(cell.position.column.value, cell.codepoints, int { cell.width });

    if (!result.empty())
        return result;

    // Batched path: the rasterizer walks the flat text one codepoint per column, advancing by that
    // codepoint's own width. @see vtrasterizer::TextClusterGrouper::renderLine.
    for (auto const& renderLine: buf.get().lines)
    {
        if (renderLine.lineOffset != line)
            continue;
        auto column = 0;
        for (auto const codepoint: renderLine.text)
        {
            auto const width = static_cast<int>(std::max(1u, unicode::width(codepoint)));
            if (codepoint != U' ')
                result.emplace_back(column, std::u32string(1, codepoint), width);
            column += width;
        }
    }

    return result;
}

std::string describeLayout(std::vector<DrawnGlyph> const& layout)
{
    auto result = std::string {};
    for (auto const& glyph: layout)
    {
        result += std::format("{}{}:", result.empty() ? "" : " ", glyph.column);
        for (auto const [index, codepoint]: crispy::views::enumerate(glyph.codepoints))
            result += std::format("{}U+{:04X}", index ? "+" : "", static_cast<uint32_t>(codepoint));
        result += std::format("(w{})", glyph.width);
    }
    return result;
}

std::pair<std::string, RenderPath> renderLineOf(vtbackend::Terminal& terminal, LineOffset line)
{
    terminal.refreshRenderBuffer();
    auto const buf = terminal.renderBuffer();
    auto const batched = std::ranges::any_of(
        buf.get().lines, [&](auto const& renderLine) { return renderLine.lineOffset == line; });
    return { describeLayout(renderedLayout(buf, line)), batched ? RenderPath::Batched : RenderPath::PerCell };
}

void selectColumns(vtbackend::Terminal& terminal,
                   LineOffset line,
                   ColumnOffset firstColumn,
                   ColumnOffset lastColumn)
{
    terminal.setSelector(std::make_unique<vtbackend::LinearSelection>(
        terminal.selectionHelper(), CellLocation { .line = line, .column = firstColumn }, []() {}));
    (void) terminal.selector()->extend(CellLocation { .line = line, .column = lastColumn });
    terminal.selector()->complete();
}

[[nodiscard]] float totalScrollPixels(vtbackend::Terminal const& terminal, float cellHeight) noexcept
{
    return (static_cast<float>(terminal.viewport().scrollOffset().value) * cellHeight)
           + terminal.smoothScrollPixelOffset();
}

} // namespace vtbackend::test
