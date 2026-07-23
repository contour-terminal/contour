// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/TtyRenderer.h>

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>
#include <vtbackend/SgrWriter.h>

#include <bit>
#include <format>
#include <optional>
#include <utility>

namespace vthost::client
{

namespace
{
    /// Appends the SGR parameters selecting @p raw as fore-, back- or
    /// underline color (SGR base 38, 48 or 58).
    void appendColor(std::string& out, uint32_t raw, int base)
    {
        auto const color = std::bit_cast<vtbackend::Color>(raw);
        switch (color.type())
        {
            case vtbackend::ColorType::Indexed: out += std::format(";{};5;{}", base, color.index()); break;
            case vtbackend::ColorType::Bright: out += std::format(";{};5;{}", base, color.index() + 8); break;
            case vtbackend::ColorType::RGB: {
                auto const rgb = color.rgb();
                out += std::format(";{};2;{};{};{}", base, rgb.red, rgb.green, rgb.blue);
                break;
            }
            case vtbackend::ColorType::Default:
            case vtbackend::ColorType::Undefined: break; // the leading reset already selected it
        }
    }
} // namespace

std::string sgrFor(proto::WireCell const& cell)
{
    auto out = std::string { "\033[0" };
    // The single shared flag→SGR table (vtbackend::SgrWriter) keeps this mirror renderer and
    // capture-pane's makeSgrSequence in lockstep, so curly/dotted/dashed underlines and rapid blink
    // reproduce identically on both paths.
    for (auto const& [flag, sgr]: vtbackend::SgrFlagCodes)
        if ((cell.flags & static_cast<uint32_t>(flag)) != 0)
            out += std::format(";{}", sgr);
    appendColor(out, cell.foreground, 38);
    appendColor(out, cell.background, 48);
    appendColor(out, cell.underlineColor, 58);
    out += 'm';
    return out;
}

std::string sgrForFill(proto::WireLine const& line)
{
    auto out = std::string { "\033[0" };
    appendColor(out, line.fillForeground, 38);
    appendColor(out, line.fillBackground, 48);
    out += 'm';
    return out;
}

std::string renderViewport(RemoteScreen const& screen)
{
    auto out = std::string { "\033[?25l" }; // hide the cursor while painting

    // Compare the raw attribute words, not formatted strings: runs of equal
    // cells (the common case) then never re-format the SGR sequence.
    auto previousSgrKey = std::optional<SgrKey> {};
    for (auto line = int32_t { 0 }; std::cmp_less(line, screen.lines); ++line)
    {
        out += std::format("\033[{};1H\033[0m\033[2K", line + 1);
        previousSgrKey.reset();

        auto const* row = screen.rowAt(line);
        if (row == nullptr)
            continue; // an unknown row stays blank

        auto skipContinuation = false;
        for (auto const& cell: row->cells)
        {
            if (skipContinuation)
            {
                skipContinuation = false;
                if (cell.codepoint == 0)
                    continue; // the wide glyph already covered this column
            }

            if (auto const key = sgrKeyOf(cell); previousSgrKey != key)
            {
                out += sgrFor(cell);
                previousSgrKey = key;
            }

            if (cell.codepoint == 0)
            {
                out += ' ';
                continue;
            }
            appendCluster(out, cell);
            skipContinuation = cell.width == 2;
        }
    }

    out += std::format("\033[0m\033[{};{}H\033[?25h", screen.cursorLine + 1, screen.cursorColumn + 1);
    return out;
}

} // namespace vthost::client
