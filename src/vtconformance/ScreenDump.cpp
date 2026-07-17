// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/CellProxy.h>
#include <vtbackend/Screen.h>
#include <vtbackend/Terminal.h>

#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtconformance/ScreenDump.h>

using namespace std::string_view_literals;

namespace vtconformance
{

namespace
{
    /// Identity of a cell's rendition, used to bucket cells into legend classes.
    struct Rendition
    {
        vtbackend::CellFlags flags;
        vtbackend::Color foreground;
        vtbackend::Color background;

        [[nodiscard]] bool operator==(Rendition const& other) const noexcept
        {
            return flags == other.flags && foreground == other.foreground && background == other.background;
        }

        /// A cell nobody has styled. Note that `Color{}` is *indexed black*, not the default
        /// colour — comparing against it would mark every untouched cell as attributed.
        [[nodiscard]] bool isDefault() const noexcept
        {
            return flags.value() == 0 && vtbackend::isDefaultColor(foreground)
                   && vtbackend::isDefaultColor(background);
        }
    };

    /// The characters handed out to distinct renditions, in order. `.` is reserved for the default
    /// rendition, so a screen with no SGR at all yields an attribute plane of pure dots.
    constexpr auto LegendAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv;

    /// Renders a rendition the way a human reads an SGR: only what deviates from the default.
    [[nodiscard]] std::string describe(Rendition const& rendition)
    {
        auto parts = std::vector<std::string> {};

        // Every flag the backend defines, read from the backend's own table rather than copied here --
        // a copy is what let `CharacterProtectedISO` go unreported. @see VTBACKEND_CELL_FLAGS.
        for (auto const flag: vtbackend::CellFlagList)
        {
            // The one flag that is structural rather than a rendition: the cell plane already renders a
            // wide character's continuation, and naming it here would report a style on every one.
            if (flag == vtbackend::CellFlag::WideCharContinuation)
                continue;

            if (rendition.flags.test(flag))
                parts.emplace_back(std::format("{}", flag));
        }

        if (!vtbackend::isDefaultColor(rendition.foreground))
            parts.emplace_back(std::format("fg={}", rendition.foreground));

        if (!vtbackend::isDefaultColor(rendition.background))
            parts.emplace_back(std::format("bg={}", rendition.background));

        if (parts.empty())
            return "default";

        auto result = std::string {};
        for (auto const& [index, part]: crispy::views::enumerate(parts))
            result += index ? std::format(" {}", part) : part;
        return result;
    }

    /// Renders one cell's text, collapsing the states a golden reader needs to tell apart.
    ///
    /// A never-written cell and a written space look identical on screen but are distinct to
    /// DECRQCRA and to selective erase, so they get distinct glyphs here.
    [[nodiscard]] std::string cellText(vtbackend::CellProxy const& cell)
    {
        if (cell.isFlagEnabled(vtbackend::CellFlag::WideCharContinuation))
            return "";
        if (cell.empty())
            return "·"; // MIDDLE DOT: never written
        auto text = cell.toUtf8();
        if (text == " ")
            return "␣"; // OPEN BOX: an explicitly written space
        return text;
    }

    /// The line flags a conformance golden cares about. The other `LineFlag`s (Wrappable, Marked,
    /// OutputStart, CommandEnd) describe shell integration and reflow bookkeeping, not VT semantics,
    /// so they are deliberately left out of the dump to keep goldens stable.
    constexpr auto DumpedLineFlags = std::array {
        std::pair { vtbackend::LineFlag::Wrapped, "Wrapped"sv },
        std::pair { vtbackend::LineFlag::DoubleWidth, "DoubleWidth"sv },
        std::pair { vtbackend::LineFlag::DoubleHeightTop, "DoubleHeightTop"sv },
        std::pair { vtbackend::LineFlag::DoubleHeightBottom, "DoubleHeightBottom"sv },
    };

    [[nodiscard]] std::string describeLineFlags(vtbackend::LineFlags flags)
    {
        auto result = std::string {};
        for (auto const& [flag, name]: DumpedLineFlags)
            if (flags.test(flag))
            {
                if (!result.empty())
                    result += ", ";
                result += name;
            }
        return result;
    }

    [[nodiscard]] std::string sectionRule(std::string_view title)
    {
        auto line = std::format("---- {} ", title);
        while (line.size() < 60)
            line += '-';
        return line + '\n';
    }
} // namespace

std::vector<ModeProbe> const& dumpedModes() noexcept
{
    // Data-driven: a mode becomes conformance-visible by gaining a row here.
    static auto const probes = std::vector<ModeProbe> {
        { "IRM", vtbackend::AnsiMode::Insert },
        { "SRM", vtbackend::AnsiMode::SendReceive },
        { "LNM", vtbackend::AnsiMode::AutomaticNewLine },
        { "DECCKM", vtbackend::DECMode::UseApplicationCursorKeys },
        { "DECCOLM", vtbackend::DECMode::Columns132 },
        { "DECSCLM", vtbackend::DECMode::SmoothScroll },
        { "DECSCNM", vtbackend::DECMode::ReverseVideo },
        { "DECOM", vtbackend::DECMode::Origin },
        { "DECAWM", vtbackend::DECMode::AutoWrap },
        { "DECARM", vtbackend::DECMode::AutoRepeat },
        { "DECTCEM", vtbackend::DECMode::VisibleCursor },
        { "DECNKM", vtbackend::DECMode::ApplicationKeypad },
        { "DECBKM", vtbackend::DECMode::BackarrowKey },
        { "DECLRMM", vtbackend::DECMode::LeftRightMargin },
    };
    return probes;
}

std::string dumpScreen(vtbackend::Terminal const& terminal, DumpOptions const& options)
{
    auto const& screen = terminal.currentScreen();
    auto const pageSize = screen.pageSize();
    auto out = std::ostringstream {};

    out << std::format("@geometry {}x{}\n", pageSize.lines.value, pageSize.columns.value);
    out << std::format("@screen {}\n", terminal.screenType());

    if (options.cursor)
    {
        auto const cursor = screen.cursor();
        out << std::format("@cursor line={} column={} visible={}\n",
                           cursor.position.line.value + 1,
                           cursor.position.column.value + 1,
                           terminal.isModeEnabled(vtbackend::DECMode::VisibleCursor) ? "yes" : "no");
    }

    if (options.modes)
    {
        out << "@modes";
        for (auto const& probe: dumpedModes())
        {
            auto const enabled =
                std::visit([&](auto mode) { return terminal.isModeEnabled(mode); }, probe.mode);
            out << std::format(" {}={}", probe.mnemonic, enabled ? "on" : "off");
        }
        out << '\n';
    }

    // Text plane.
    out << sectionRule("text");
    for (auto const line: std::views::iota(0, pageSize.lines.value))
    {
        out << std::format("{:02}|", line + 1);
        for (auto const column: std::views::iota(0, pageSize.columns.value))
            out << cellText(screen.at(vtbackend::LineOffset(line), vtbackend::ColumnOffset(column)));
        out << '\n';
    }

    // Attribute plane, with a legend allocated in first-seen (i.e. reading) order so that the
    // legend letters stay stable as long as the screen does.
    if (options.attributes)
    {
        auto legend = std::vector<Rendition> {};
        auto plane = std::vector<std::string> {};

        for (auto const line: std::views::iota(0, pageSize.lines.value))
        {
            auto row = std::string {};
            for (auto const column: std::views::iota(0, pageSize.columns.value))
            {
                auto const cell = screen.at(vtbackend::LineOffset(line), vtbackend::ColumnOffset(column));
                auto const rendition =
                    Rendition { cell.flags(), cell.foregroundColor(), cell.backgroundColor() };

                if (rendition.isDefault())
                {
                    row += '.';
                    continue;
                }

                auto const existing = std::ranges::find(legend, rendition);
                auto const index = static_cast<size_t>(std::ranges::distance(legend.begin(), existing));
                if (existing == legend.end())
                    legend.push_back(rendition);

                row += index < LegendAlphabet.size() ? LegendAlphabet[index] : '?';
            }
            plane.emplace_back(std::move(row));
        }

        out << sectionRule("attributes");
        for (auto const& [index, row]: crispy::views::enumerate(plane))
            out << std::format("{:02}|{}\n", index + 1, row);

        out << sectionRule("legend");
        out << ". = default\n";
        for (auto const& [index, rendition]: crispy::views::enumerate(legend))
            out << std::format("{} = {}\n",
                               static_cast<size_t>(index) < LegendAlphabet.size()
                                   ? LegendAlphabet[static_cast<size_t>(index)]
                                   : '?',
                               describe(rendition));
    }

    if (options.lineFlags)
    {
        out << sectionRule("lineflags");
        for (auto const line: std::views::iota(0, pageSize.lines.value))
        {
            auto const description =
                describeLineFlags(screen.grid().lineAt(vtbackend::LineOffset(line)).flags());
            if (!description.empty())
                out << std::format("{:02}|{}\n", line + 1, description);
        }
    }

    return out.str();
}

std::string diffDumps(std::string_view expected, std::string_view actual)
{
    if (expected == actual)
        return {};

    auto const expectedLines = crispy::split(expected, '\n');
    auto const actualLines = crispy::split(actual, '\n');
    auto out = std::ostringstream {};

    for (auto const index: std::views::iota(size_t { 0 }, std::max(expectedLines.size(), actualLines.size())))
    {
        auto const lhs = index < expectedLines.size() ? expectedLines[index] : std::string_view {};
        auto const rhs = index < actualLines.size() ? actualLines[index] : std::string_view {};
        if (lhs == rhs)
            continue;
        out << std::format("@@ line {}\n-{}\n+{}\n", index + 1, lhs, rhs);
    }

    return out.str();
}

} // namespace vtconformance
