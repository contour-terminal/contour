// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/TtyRenderer.h>

#include <vtbackend/CellFlags.h>
#include <vtbackend/Color.h>

#include <libunicode/convert.h>

#include <array>
#include <bit>
#include <format>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>

namespace muxserver::client
{

namespace
{
    /// One SGR rendition flag: the wire bit and the SGR parameter selecting it.
    struct FlagSgr
    {
        vtbackend::CellFlag flag;
        int sgr;
    };

    /// The renditions a plain terminal can reproduce, in SGR order.
    constexpr auto FlagSgrTable = std::array {
        FlagSgr { vtbackend::CellFlag::Bold, 1 },
        FlagSgr { vtbackend::CellFlag::Faint, 2 },
        FlagSgr { vtbackend::CellFlag::Italic, 3 },
        FlagSgr { vtbackend::CellFlag::Underline, 4 },
        FlagSgr { vtbackend::CellFlag::Blinking, 5 },
        FlagSgr { vtbackend::CellFlag::Inverse, 7 },
        FlagSgr { vtbackend::CellFlag::Hidden, 8 },
        FlagSgr { vtbackend::CellFlag::CrossedOut, 9 },
        FlagSgr { vtbackend::CellFlag::DoublyUnderlined, 21 },
        FlagSgr { vtbackend::CellFlag::Overline, 53 },
    };

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
    for (auto const& [flag, sgr]: FlagSgrTable)
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
    using SgrKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
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

            if (auto const key = SgrKey { cell.flags, cell.foreground, cell.background, cell.underlineColor };
                previousSgrKey != key)
            {
                out += sgrFor(cell);
                previousSgrKey = key;
            }

            if (cell.codepoint == 0)
            {
                out += ' ';
                continue;
            }
            unicode::convert_to<char>(std::u32string_view(&cell.codepoint, 1), std::back_inserter(out));
            for (auto const extra: cell.clusterExtras)
                unicode::convert_to<char>(std::u32string_view(&extra, 1), std::back_inserter(out));
            skipContinuation = cell.width == 2;
        }
    }

    out += std::format("\033[0m\033[{};{}H\033[?25h", screen.cursorLine + 1, screen.cursorColumn + 1);
    return out;
}

} // namespace muxserver::client
