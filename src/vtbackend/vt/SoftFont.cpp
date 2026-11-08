// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/SoftFont.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <utility>

namespace vtbackend
{

namespace
{
    // Pe, Pt and Pcss are small enumerations: the raw value is the enumerator.
    template <typename Enum>
    [[nodiscard]] constexpr std::optional<Enum> toEnum(int value, int maxValue)
    {
        if (value < 0 || value > maxValue)
            return std::nullopt;
        return static_cast<Enum>(value);
    }

    /// Pss: the page format, as columns and lines.
    struct PageFormat
    {
        int pss;
        uint16_t columns;
        uint8_t lines;
    };

    constexpr auto PageFormats = std::array {
        PageFormat { .pss = 0, .columns = 80, .lines = 24 },
        PageFormat { .pss = 1, .columns = 80, .lines = 24 },
        PageFormat { .pss = 2, .columns = 132, .lines = 24 },
        PageFormat { .pss = 11, .columns = 80, .lines = 36 },
        PageFormat { .pss = 12, .columns = 132, .lines = 36 },
        PageFormat { .pss = 21, .columns = 80, .lines = 48 },
        PageFormat { .pss = 22, .columns = 132, .lines = 48 },
    };

    /// Which fonts a cell matrix may be declared for.
    enum class MatrixUsage : uint8_t
    {
        TextOnly, ///< Not valid for full-cell fonts (except 6x10 at 132 columns).
        Any,
    };

    /// Pcmw 2/3/4: the cell matrices.
    struct CellMatrix
    {
        int pcmw;
        uint8_t width;
        MatrixUsage usage;
    };

    constexpr auto CellMatrices = std::array {
        CellMatrix { .pcmw = 2, .width = 5, .usage = MatrixUsage::TextOnly },
        CellMatrix { .pcmw = 3, .width = 6, .usage = MatrixUsage::TextOnly },
        CellMatrix { .pcmw = 4, .width = 7, .usage = MatrixUsage::TextOnly },
    };
    constexpr auto MatrixHeight = 10;

    /// How a candidate row derives the text width of its result.
    enum class TextWidthRule : uint8_t
    {
        Declared, ///< The declared width for text fonts, none for full-cell fonts.
        Clip8,    ///< Always 8 (the VT2xx).
        Unclipped,
    };

    /// When a known terminal's row may be chosen.
    enum class SizeRule : uint8_t
    {
        Always,
        OnlyWhenNoDeclaredSize, ///< The VT2xx can only be sized by matrix or by default.
    };

    /// One known terminal: the cell a font fits when it is no bigger than the candidate.
    struct TargetCell
    {
        uint16_t columns;
        uint8_t candidateWidth;
        uint8_t candidateHeight;
        SizeRule sizeRule;
        uint8_t fullWidth;
        uint8_t fullHeight;
        TextWidthRule textWidth;
    };

    constexpr auto TargetCells = std::array {
        TargetCell { 80, 8, 10, SizeRule::OnlyWhenNoDeclaredSize, 10, 10, TextWidthRule::Clip8 }, // VT2xx
        TargetCell { 80, 15, 12, SizeRule::Always, 15, 12, TextWidthRule::Declared },             // VT320
        TargetCell { 80, 10, 16, SizeRule::Always, 10, 16, TextWidthRule::Declared }, // VT420, VT5xx
        TargetCell { 80, 10, 20, SizeRule::Always, 10, 20, TextWidthRule::Declared }, // VT340
        TargetCell { 80, 12, 30, SizeRule::Always, 12, 30, TextWidthRule::Declared }, // VT382
        TargetCell { 132, 6, 10, SizeRule::OnlyWhenNoDeclaredSize, 6, 10, TextWidthRule::Unclipped }, // VT240
        TargetCell { 132, 9, 12, SizeRule::Always, 9, 12, TextWidthRule::Declared },                  // VT320
        TargetCell { 132, 6, 16, SizeRule::Always, 6, 16, TextWidthRule::Declared }, // VT420, VT5xx
        TargetCell { 132, 6, 20, SizeRule::Always, 6, 20, TextWidthRule::Declared }, // VT340
        TargetCell { 132, 7, 30, SizeRule::Always, 7, 30, TextWidthRule::Declared }, // VT382
    };

    /// The reverse question mark of Windows Terminal's FontBuffer::_generateErrorGlyph, scaled to a cell by
    /// picking whole rows and columns of a base bitmap: 16 columns wide as WT treats it (15 pattern bits,
    /// bit 15 clear), 32 rows tall (23 drawn, the rest clear). Hand-tuned there for every cell size from
    /// 1x1 to 16x32, and ported as the same data rather than resampled, which turns the glyph to mush at the
    /// common sizes.
    constexpr auto ErrorGlyphPattern = std::array<uint16_t, MaxSoftCellHeight> {
        0b000000000000000, 0b000000000000000, 0b000000000000000, 0b000000000000000, 0b000000000000000,
        0b000000000000000, 0b001111111110000, 0b011111111111000, 0b111000000011100, 0b111000000011100,
        0b111000000000000, 0b111000000000000, 0b111100000000000, 0b011111000000000, 0b000011110000000,
        0b000001110000000, 0b000001110000000, 0b000001110000000, 0b000001110000000, 0b000000000000000,
        0b000001110000000, 0b000001110000000, 0b000001110000000,
    };

    /// Per cell width (index), the set of base-bitmap columns the error glyph keeps: bit n is column n
    /// counted from the right, and as many bits are set as the cell is wide.
    constexpr auto ErrorGlyphColumnMasks = std::array<uint32_t, MaxSoftCellWidth + 1> {
        0, 1, 3, 32771, 8457, 9481, 9545, 9673, 42441, 26061, 58829, 28141, 60909, 63453, 63485, 65533, 65535,
    };

    /// Per cell height (index), the set of base-bitmap rows the error glyph keeps: bit n is row n from the
    /// top, and as many bits are set as the cell is tall.
    constexpr auto ErrorGlyphRowMasks = std::array<uint32_t, MaxSoftCellHeight + 1> {
        0,         1,         3,          7,          15,         1613952,    10002560,  10002816,  10068352,
        10068353,  26845569,  26847617,   26847619,   26864003,   28961155,   28961219,  28961731,  62516163,
        62516167,  129625031, 129625039,  129756111,  263973839,  263974863,  268169167, 536604623, 536608719,
        536608735, 536870879, 1073741791, 2147483615, 2147483647, 4294967295,
    };

    [[nodiscard]] crispy::StrongHash hashGlyph(uint8_t width,
                                               uint8_t height,
                                               std::span<uint8_t const> coverage)
    {
        return crispy::StrongHash { width, height, 0, 0 }
               * crispy::StrongHash::compute(coverage.data(), coverage.size());
    }

    [[nodiscard]] std::shared_ptr<SoftGlyph const> makeErrorGlyph(SoftCellGeometry geometry)
    {
        auto const columnMask = ErrorGlyphColumnMasks.at(geometry.fullWidth);
        auto const rowMask = ErrorGlyphRowMasks.at(geometry.fullHeight);
        auto coverage = std::vector<uint8_t> {};
        coverage.reserve(static_cast<size_t>(geometry.fullWidth) * geometry.fullHeight);
        for (auto const row: std::views::iota(size_t { 0 }, ErrorGlyphPattern.size()))
        {
            if (((rowMask >> row) & 1U) == 0)
                continue;
            for (auto const column: std::views::iota(uint8_t { 0 }, MaxSoftCellWidth) | std::views::reverse)
                if (((columnMask >> column) & 1U) != 0)
                    coverage.push_back(static_cast<uint8_t>((ErrorGlyphPattern[row] >> column) & 1U));
        }
        return makeSoftGlyph(geometry.fullWidth, geometry.fullHeight, std::move(coverage));
    }

    /// One character as drawn from sixel data, before it is fitted to the cell.
    struct Scratch
    {
        std::array<uint8_t, static_cast<size_t>(MaxSoftCellWidth) * MaxSoftCellHeight> pixels {};
        int position = 0;
    };

    /// Whether two declarations describe the same font (the header fields that fix its cell).
    [[nodiscard]] bool sameFontKind(SoftFontDeclaration a, SoftFontDeclaration b) noexcept
    {
        a.startPosition = b.startPosition;
        a.erase = b.erase;
        return a == b;
    }

} // namespace

std::string_view toString(SoftFontError error) noexcept
{
    switch (error)
    {
        case SoftFontError::FontNumber: return "font number out of range";
        case SoftFontError::EraseControl: return "erase control out of range";
        case SoftFontError::CellMatrix: return "invalid cell matrix";
        case SoftFontError::FontSet: return "unknown font set size";
        case SoftFontError::Usage: return "invalid font usage";
        case SoftFontError::CharsetSize: return "invalid character set size";
        case SoftFontError::TooLarge: return "cell or start position too large";
        case SoftFontError::Designator: return "invalid designator";
        case SoftFontError::Glyph: return "invalid glyph";
    }
    return "unknown error";
}

std::expected<SoftFontDeclaration, SoftFontError> decodeDecdld(DecdldParameters const& p)
{
    auto declaration = SoftFontDeclaration {};

    if (p.pfn < 0 || p.pfn > 2)
        return std::unexpected(SoftFontError::FontNumber);
    declaration.fontNumber = static_cast<uint8_t>(p.pfn);

    auto const erase = toEnum<SoftFontErase>(p.pe, 2);
    if (!erase)
        return std::unexpected(SoftFontError::EraseControl);
    declaration.erase = *erase;

    auto const format = std::ranges::find(PageFormats, p.pss, &PageFormat::pss);
    if (format == PageFormats.end())
        return std::unexpected(SoftFontError::FontSet);
    declaration.columnsPerPage = format->columns;
    declaration.linesPerPage = format->lines;

    // Pt: 0 and 1 both mean text, 2 is full cell.
    if (p.pt < 0 || p.pt > 2)
        return std::unexpected(SoftFontError::Usage);
    declaration.usage = p.pt == 2 ? SoftFontUsage::FullCell : SoftFontUsage::Text;

    auto const charsetSize = toEnum<SoftCharsetSize>(p.pcss, 1);
    if (!charsetSize)
        return std::unexpected(SoftFontError::CharsetSize);
    declaration.charsetSize = *charsetSize;

    if (p.pcmw == 1 || p.pcmw < 0)
        return std::unexpected(SoftFontError::CellMatrix);
    if (auto const matrix = std::ranges::find(CellMatrices, p.pcmw, &CellMatrix::pcmw);
        matrix != CellMatrices.end())
    {
        auto const isSixByTenAt132 = matrix->width == 6 && declaration.columnsPerPage == 132;
        if (matrix->usage == MatrixUsage::TextOnly && declaration.usage != SoftFontUsage::Text
            && !isSixByTenAt132)
            return std::unexpected(SoftFontError::CellMatrix);
        declaration.sizeSource = SoftFontSizeSource::Matrix;
        declaration.declaredWidth = matrix->width;
        declaration.declaredHeight = MatrixHeight;
    }
    else
    {
        if (std::cmp_greater(p.pcmw, MaxSoftCellWidth) || p.pcmh < 0
            || std::cmp_greater(p.pcmh, MaxSoftCellHeight))
            return std::unexpected(SoftFontError::TooLarge);
        declaration.declaredWidth = static_cast<uint8_t>(p.pcmw);
        declaration.declaredHeight = static_cast<uint8_t>(p.pcmh);
    }

    // Pcn is a position in the set: absent means 1 in a 94-set (position 0x20 is the space) and 0 in a
    // 96-set.
    auto const pcn = p.pcn == 0 && declaration.charsetSize == SoftCharsetSize::Size94 ? 1 : p.pcn;
    if (pcn < 0 || FirstSoftGlyphPosition + pcn > LastSoftGlyphPosition)
        return std::unexpected(SoftFontError::TooLarge);
    declaration.startPosition = static_cast<uint8_t>(FirstSoftGlyphPosition + pcn);

    return declaration;
}

SoftCellGeometry detectTargetCell(SoftFontDeclaration const& declaration,
                                  uint8_t usedWidth,
                                  uint8_t usedHeight)
{
    auto const declaredText =
        declaration.usage == SoftFontUsage::Text ? declaration.declaredWidth : uint8_t { 0 };
    auto const is132 = declaration.columnsPerPage == 132;

    // A matrix is a VT2xx font, 10x10 with text clipped to 8, except the VT240's unclipped 6x10 at 132
    // columns.
    if (declaration.sizeSource == SoftFontSizeSource::Matrix)
        return is132 && declaration.declaredWidth <= 6 ? SoftCellGeometry { 6, 10, 0 }
                                                       : SoftCellGeometry { 10, 10, 8 };

    // Explicit dimensions on a full-cell font are the exact cell.
    if (declaration.declaredWidth != 0 && declaration.declaredHeight != 0
        && declaration.usage == SoftFontUsage::FullCell)
        return { declaration.declaredWidth, declaration.declaredHeight, 0 };

    // Anything but 24 lines targets a VT420 or VT5xx.
    if (declaration.linesPerPage != 24)
        return { static_cast<uint8_t>(is132 ? 6 : 10),
                 static_cast<uint8_t>(declaration.linesPerPage == 48 ? 8 : 10),
                 declaredText };

    // Used heights are always multiples of six, so a candidate height is compared rounded up to six.
    auto const noDeclaredSize = declaration.declaredWidth == 0 && declaration.declaredHeight == 0;
    auto const inRange = [&](TargetCell const& cell) {
        auto const sixelHeight = static_cast<uint8_t>((cell.candidateHeight + 5) / 6 * 6);
        auto const heightFits = declaration.declaredHeight != 0
                                    ? declaration.declaredHeight <= cell.candidateHeight
                                    : usedHeight <= sixelHeight;
        auto const widthFits = declaration.declaredWidth != 0
                                   ? declaration.declaredWidth <= cell.candidateWidth
                                   : usedWidth <= cell.candidateWidth;
        return heightFits && widthFits && (cell.sizeRule == SizeRule::Always || noDeclaredSize);
    };

    auto const match = std::ranges::find_if(TargetCells, [&](TargetCell const& cell) {
        return cell.columns == declaration.columnsPerPage && inRange(cell);
    });
    if (match == TargetCells.end())
        return { MaxSoftCellWidth, MaxSoftCellHeight, declaredText };

    switch (match->textWidth)
    {
        case TextWidthRule::Declared: return { match->fullWidth, match->fullHeight, declaredText };
        case TextWidthRule::Clip8: return { match->fullWidth, match->fullHeight, 8 };
        case TextWidthRule::Unclipped: return { match->fullWidth, match->fullHeight, 0 };
    }
    return { MaxSoftCellWidth, MaxSoftCellHeight, declaredText };
}

std::shared_ptr<SoftGlyph const> makeSoftGlyph(uint8_t width, uint8_t height, std::vector<uint8_t> coverage)
{
    auto glyph = std::make_shared<SoftGlyph>();
    glyph->id = hashGlyph(width, height, coverage);
    glyph->width = width;
    glyph->height = height;
    glyph->coverage = std::move(coverage);
    return glyph;
}

SoftFont makeSoftFont(SoftFontDeclaration const& declaration,
                      std::string designator,
                      SoftCellGeometry geometry,
                      std::array<std::shared_ptr<SoftGlyph const>, SoftGlyphCount> glyphs)
{
    return SoftFont { .declaration = declaration,
                      .designator = std::move(designator),
                      .geometry = geometry,
                      .glyphs = std::move(glyphs),
                      .errorGlyph = makeErrorGlyph(geometry) };
}

std::expected<SoftFont, SoftFontError> assembleSoftFont(SoftFontDeclaration const& declaration,
                                                        std::string designator,
                                                        SoftCellGeometry geometry,
                                                        std::span<SoftGlyphInput> glyphs)
{
    if (declaration.fontNumber >= SoftFontCount)
        return std::unexpected(SoftFontError::FontNumber);
    if (designator.size() > MaxSoftDesignatorLength)
        return std::unexpected(SoftFontError::Designator);
    if (geometry.fullWidth < 1 || geometry.fullWidth > MaxSoftCellWidth || geometry.fullHeight < 1
        || geometry.fullHeight > MaxSoftCellHeight || geometry.textWidth > geometry.fullWidth)
        return std::unexpected(SoftFontError::CellMatrix);

    auto placed = std::array<std::shared_ptr<SoftGlyph const>, SoftGlyphCount> {};
    auto previous = std::optional<uint8_t> {};
    for (auto& glyph: glyphs)
    {
        if (glyph.position < FirstSoftGlyphPosition || glyph.position > LastSoftGlyphPosition
            || (previous && *previous >= glyph.position) || glyph.width != geometry.fullWidth
            || glyph.height != geometry.fullHeight
            || glyph.coverage.size() != size_t { glyph.width } * glyph.height)
            return std::unexpected(SoftFontError::Glyph);
        previous = glyph.position;
        placed[glyph.position - FirstSoftGlyphPosition] =
            makeSoftGlyph(glyph.width, glyph.height, std::move(glyph.coverage));
    }
    return makeSoftFont(declaration, std::move(designator), geometry, std::move(placed));
}

std::expected<SoftFont, SoftFontError> buildSoftFont(SoftFontDeclaration const& declaration,
                                                     std::string_view body,
                                                     SoftFont const* previous)
{
    // Dscs: up to two intermediates and one final; bytes that are neither are ignored.
    auto designator = std::string {};
    auto const bodyAfterDscs = [&]() -> std::optional<std::string_view> {
        for (auto const index: std::views::iota(size_t { 0 }, body.size()))
        {
            auto const ch = static_cast<unsigned char>(body[index]);
            if (ch >= 0x20 && ch <= 0x2F)
                designator.push_back(static_cast<char>(ch));
            else if (ch >= 0x30 && ch <= 0x7E)
            {
                designator.push_back(static_cast<char>(ch));
                return body.substr(index + 1);
            }
            if (designator.size() > 2)
                return std::nullopt;
        }
        return std::nullopt;
    }();
    if (!bodyAfterDscs)
        return std::unexpected(SoftFontError::Designator);

    // Draw every character into a 16x32 scratch, ignoring what falls outside it.
    auto characters = std::vector<Scratch> {};
    auto usedWidth = 0;
    auto usedHeight = 0;
    auto position = static_cast<int>(declaration.startPosition);
    auto column = 0;
    auto row = 0;
    auto touched = false;
    auto storing = false;
    auto const endOfLine = [&] {
        row += 6;
        usedWidth = std::max(usedWidth, column);
        usedHeight = std::max(usedHeight, row);
        column = 0;
    };
    auto const beginCharacter = [&] {
        // Positions past the 7-bit range can never be stored, so they get no scratch (the body is untrusted
        // and may hold any number of them); they still count toward the used size.
        storing = std::cmp_less_equal(position, LastSoftGlyphPosition);
        if (storing)
            characters.push_back(Scratch { .position = position });
        row = 0;
        column = 0;
        touched = false;
    };
    auto const endOfCharacter = [&] {
        endOfLine();
        ++position;
        beginCharacter();
    };
    beginCharacter();
    for (auto const raw: *bodyAfterDscs)
    {
        auto const ch = static_cast<unsigned char>(raw);
        if (ch >= '?' && ch <= '~')
        {
            touched = true;
            if (storing && std::cmp_less(column, MaxSoftCellWidth))
                for (auto const bit: std::views::iota(0, 6))
                    if (row + bit < MaxSoftCellHeight && (((ch - '?') >> bit) & 1) != 0)
                        characters.back().pixels.at((static_cast<size_t>(row + bit) * MaxSoftCellWidth)
                                                    + static_cast<size_t>(column)) = 1;
            ++column;
        }
        else if (ch == '/')
        {
            touched = true;
            endOfLine();
        }
        else if (ch == ';')
            endOfCharacter();
    }
    if (touched)
        endOfLine();
    else if (storing)
        characters.pop_back(); // No data in the trailing character: it is not defined.

    auto const clampByte = [](int value) {
        return static_cast<uint8_t>(std::min(value, 255));
    };

    // A merge keeps the cell of the font it extends; anything else is sized from what was drawn.
    auto const merges = declaration.erase == SoftFontErase::ReloadedChars && previous != nullptr
                        && previous->designator == designator
                        && sameFontKind(previous->declaration, declaration);
    auto const geometry = merges ? previous->geometry
                                 : detectTargetCell(declaration, clampByte(usedWidth), clampByte(usedHeight));
    auto glyphs = merges ? previous->glyphs : std::array<std::shared_ptr<SoftGlyph const>, SoftGlyphCount> {};

    // Clip text to its width and centre it in the cell.
    auto const textWidth =
        geometry.textWidth != 0 ? std::min(geometry.textWidth, geometry.fullWidth) : geometry.fullWidth;
    auto const offset = (geometry.fullWidth - textWidth) / 2;
    for (auto const& character: characters)
    {
        auto coverage = std::vector<uint8_t>(static_cast<size_t>(geometry.fullWidth) * geometry.fullHeight);
        for (auto const y: std::views::iota(0, int { geometry.fullHeight }))
            for (auto const x: std::views::iota(0, int { textWidth }))
                coverage.at((static_cast<size_t>(y) * geometry.fullWidth) + static_cast<size_t>(x + offset)) =
                    character.pixels.at((static_cast<size_t>(y) * MaxSoftCellWidth) + static_cast<size_t>(x));
        glyphs.at(static_cast<size_t>(character.position - FirstSoftGlyphPosition)) =
            makeSoftGlyph(geometry.fullWidth, geometry.fullHeight, std::move(coverage));
    }

    return makeSoftFont(declaration, std::move(designator), geometry, std::move(glyphs));
}

} // namespace vtbackend
