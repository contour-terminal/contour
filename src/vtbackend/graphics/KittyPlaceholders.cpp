// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyPlaceholders.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>

namespace vtbackend::kitty_graphics
{

namespace
{
    // clang-format off
    /// kitty's row/column diacritics, in order: the diacritic at index n encodes n + 1.
    /// Generated from kitty's `gen/rowcolumn-diacritics.txt` (the code point column of every
    /// non-comment line, 297 of them, already sorted ascending); do not edit by hand.
    constexpr auto Diacritics = std::array<char32_t, DiacriticCount> {
        0x00305, 0x0030D, 0x0030E, 0x00310, 0x00312, 0x0033D, 0x0033E, 0x0033F, 0x00346, 0x0034A,
        0x0034B, 0x0034C, 0x00350, 0x00351, 0x00352, 0x00357, 0x0035B, 0x00363, 0x00364, 0x00365,
        0x00366, 0x00367, 0x00368, 0x00369, 0x0036A, 0x0036B, 0x0036C, 0x0036D, 0x0036E, 0x0036F,
        0x00483, 0x00484, 0x00485, 0x00486, 0x00487, 0x00592, 0x00593, 0x00594, 0x00595, 0x00597,
        0x00598, 0x00599, 0x0059C, 0x0059D, 0x0059E, 0x0059F, 0x005A0, 0x005A1, 0x005A8, 0x005A9,
        0x005AB, 0x005AC, 0x005AF, 0x005C4, 0x00610, 0x00611, 0x00612, 0x00613, 0x00614, 0x00615,
        0x00616, 0x00617, 0x00657, 0x00658, 0x00659, 0x0065A, 0x0065B, 0x0065D, 0x0065E, 0x006D6,
        0x006D7, 0x006D8, 0x006D9, 0x006DA, 0x006DB, 0x006DC, 0x006DF, 0x006E0, 0x006E1, 0x006E2,
        0x006E4, 0x006E7, 0x006E8, 0x006EB, 0x006EC, 0x00730, 0x00732, 0x00733, 0x00735, 0x00736,
        0x0073A, 0x0073D, 0x0073F, 0x00740, 0x00741, 0x00743, 0x00745, 0x00747, 0x00749, 0x0074A,
        0x007EB, 0x007EC, 0x007ED, 0x007EE, 0x007EF, 0x007F0, 0x007F1, 0x007F3, 0x00816, 0x00817,
        0x00818, 0x00819, 0x0081B, 0x0081C, 0x0081D, 0x0081E, 0x0081F, 0x00820, 0x00821, 0x00822,
        0x00823, 0x00825, 0x00826, 0x00827, 0x00829, 0x0082A, 0x0082B, 0x0082C, 0x0082D, 0x00951,
        0x00953, 0x00954, 0x00F82, 0x00F83, 0x00F86, 0x00F87, 0x0135D, 0x0135E, 0x0135F, 0x017DD,
        0x0193A, 0x01A17, 0x01A75, 0x01A76, 0x01A77, 0x01A78, 0x01A79, 0x01A7A, 0x01A7B, 0x01A7C,
        0x01B6B, 0x01B6D, 0x01B6E, 0x01B6F, 0x01B70, 0x01B71, 0x01B72, 0x01B73, 0x01CD0, 0x01CD1,
        0x01CD2, 0x01CDA, 0x01CDB, 0x01CE0, 0x01DC0, 0x01DC1, 0x01DC3, 0x01DC4, 0x01DC5, 0x01DC6,
        0x01DC7, 0x01DC8, 0x01DC9, 0x01DCB, 0x01DCC, 0x01DD1, 0x01DD2, 0x01DD3, 0x01DD4, 0x01DD5,
        0x01DD6, 0x01DD7, 0x01DD8, 0x01DD9, 0x01DDA, 0x01DDB, 0x01DDC, 0x01DDD, 0x01DDE, 0x01DDF,
        0x01DE0, 0x01DE1, 0x01DE2, 0x01DE3, 0x01DE4, 0x01DE5, 0x01DE6, 0x01DFE, 0x020D0, 0x020D1,
        0x020D4, 0x020D5, 0x020D6, 0x020D7, 0x020DB, 0x020DC, 0x020E1, 0x020E7, 0x020E9, 0x020F0,
        0x02CEF, 0x02CF0, 0x02CF1, 0x02DE0, 0x02DE1, 0x02DE2, 0x02DE3, 0x02DE4, 0x02DE5, 0x02DE6,
        0x02DE7, 0x02DE8, 0x02DE9, 0x02DEA, 0x02DEB, 0x02DEC, 0x02DED, 0x02DEE, 0x02DEF, 0x02DF0,
        0x02DF1, 0x02DF2, 0x02DF3, 0x02DF4, 0x02DF5, 0x02DF6, 0x02DF7, 0x02DF8, 0x02DF9, 0x02DFA,
        0x02DFB, 0x02DFC, 0x02DFD, 0x02DFE, 0x02DFF, 0x0A66F, 0x0A67C, 0x0A67D, 0x0A6F0, 0x0A6F1,
        0x0A8E0, 0x0A8E1, 0x0A8E2, 0x0A8E3, 0x0A8E4, 0x0A8E5, 0x0A8E6, 0x0A8E7, 0x0A8E8, 0x0A8E9,
        0x0A8EA, 0x0A8EB, 0x0A8EC, 0x0A8ED, 0x0A8EE, 0x0A8EF, 0x0A8F0, 0x0A8F1, 0x0AAB0, 0x0AAB2,
        0x0AAB3, 0x0AAB7, 0x0AAB8, 0x0AABE, 0x0AABF, 0x0AAC1, 0x0FE20, 0x0FE21, 0x0FE22, 0x0FE23,
        0x0FE24, 0x0FE25, 0x0FE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187, 0x1D188, 0x1D189,
        0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243, 0x1D244,
    };
    // clang-format on

    static_assert(std::ranges::is_sorted(Diacritics), "diacriticOrdinal() binary-searches the table");

    /// What resolvePlaceholderRuns() remembers of the cell left of the one it looks at. The ordinals are
    /// 1-based and 0 means unknown, as in PlaceholderCell.
    struct RunCell
    {
        uint32_t idLow = 0;
        uint32_t placementId = 0;
        uint32_t row = 0;
        uint32_t column = 0;
        uint32_t idHigh = 0;
    };

    /// The largest ordinal the third diacritic may state: it names the id's most significant byte, plus one.
    constexpr auto MaxIdHighOrdinal = uint32_t { 256 };

    /// @return Whether @p cell is a placeholder that can name an image: one whose third diacritic is past
    ///         MaxIdHighOrdinal would name bits beyond the 32-bit id, so it shows nothing.
    [[nodiscard]] bool namesAnImage(PlaceholderCell const& cell) noexcept
    {
        return cell.presence == CellPresence::Placeholder && cell.idHigh <= MaxIdHighOrdinal;
    }

    /// @return Whether @p cell continues the run whose last cell is @p previous: same image and
    ///         placement, and every ordinal it states is the one the run implies.
    [[nodiscard]] bool continuesRun(PlaceholderCell const& cell, RunCell const& previous) noexcept
    {
        return namesAnImage(cell) && cell.idLow == previous.idLow && cell.placementId == previous.placementId
               && (cell.row == 0 || cell.row == previous.row)
               && (cell.column == 0 || cell.column == previous.column + 1)
               && (cell.idHigh == 0 || cell.idHigh == previous.idHigh);
    }

    /// @return The run that ends just before column @p end, being @p length cells long, whose last cell
    ///         is @p last.
    [[nodiscard]] PlaceholderRun makeRun(RunCell const& last, size_t end, uint32_t length) noexcept
    {
        return PlaceholderRun {
            .firstColumn = static_cast<uint16_t>(end - length),
            .length = static_cast<uint16_t>(length),
            .imageId = last.idLow | ((last.idHigh - 1) << 24),
            .placementId = last.placementId,
            .imageRow = last.row - 1,
            .imageColumn = last.column - length,
        };
    }
} // namespace

char32_t diacriticOf(uint32_t ordinal) noexcept
{
    if (ordinal == 0 || ordinal > Diacritics.size())
        return U'\0';
    return Diacritics[ordinal - 1];
}

uint32_t diacriticOrdinal(char32_t codepoint) noexcept
{
    auto const found = std::ranges::lower_bound(Diacritics, codepoint);
    if (found == Diacritics.end() || *found != codepoint)
        return 0;
    return static_cast<uint32_t>(found - Diacritics.begin()) + 1;
}

uint32_t placeholderIdOf(Color color) noexcept
{
    switch (color.type())
    {
        case ColorType::RGB: return color.rgb().value();
        case ColorType::Indexed: return color.index();
        // The bright colours are palette entries 8..15, which `38;5;8..15` names as well.
        case ColorType::Bright: return 8u + color.index();
        case ColorType::Default:
        case ColorType::Undefined: break;
    }
    return 0;
}

std::vector<PlaceholderRun> resolvePlaceholderRuns(std::span<PlaceholderCell const> cells)
{
    auto runs = std::vector<PlaceholderRun> {};
    auto length = uint32_t { 0 };
    auto previous = RunCell {};

    for (auto const [index, cell]: core::views::enumerate(cells))
    {
        auto current = namesAnImage(cell) ? RunCell { .idLow = cell.idLow,
                                                      .placementId = cell.placementId,
                                                      .row = cell.row,
                                                      .column = cell.column,
                                                      .idHigh = cell.idHigh }
                                          : RunCell {};
        if (length > 0 && continuesRun(cell, previous))
        {
            // Whatever the cell leaves unstated it takes from its left neighbour.
            ++length;
            current.row = std::max(previous.row, 1u);
            current.column = previous.column + 1;
            current.idHigh = std::max(previous.idHigh, 1u);
        }
        else
        {
            if (length > 0)
                runs.push_back(makeRun(previous, index, length));
            if (namesAnImage(cell))
            {
                // A run's first cell has nothing to infer from: an unstated ordinal is the first.
                length = 1;
                current.row = std::max(current.row, 1u);
                current.column = std::max(current.column, 1u);
                current.idHigh = std::max(current.idHigh, 1u);
            }
            else
                length = 0;
        }
        previous = current;
    }
    if (length > 0)
        runs.push_back(makeRun(previous, cells.size(), length));
    return runs;
}

} // namespace vtbackend::kitty_graphics
