// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/VTType.h>

#include <array>
#include <cstdint>
#include <optional>

namespace vtbackend
{

using CharsetMap = std::array<char32_t, 127>;

enum class CharsetId : std::uint8_t
{
    Special, // Special Character and Line Drawing Set

    British,
    Dutch,
    Finnish,
    French,
    FrenchCanadian,
    German,
    NorwegianDanish,
    Spanish,
    Swedish,
    Swiss,
    USASCII,
    Technical,

    ISOLatin1Supplemental // 96-character set, designated via ESC - A / ESC . A / ESC / A
};

enum class CharsetTable : std::uint8_t
{
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

/// A User-Preferred Supplemental Set (UPSS) designation.
///
/// UPSS names the supplemental character set that GR maps to by default, and that the `<` SCS
/// designator resolves to. It is assigned with DECAUPSS (`DCS Ps ! u D...D ST`) and reported back by
/// DECRQUPSS (`CSI & u`).
///
/// @note Contour decodes its input as UTF-8, so GR never carries 8-bit graphics and a UPSS
/// designation does not re-map decoded codepoints -- doing so would corrupt Latin-1 text. It is
/// tracked as faithful state and reported, exactly as the GR register and the 96-charset
/// designations are. @see CharsetMapping::lockingShiftGR, CharsetMapping::select96.
struct UserPreferredSupplementalSet
{
    /// The designator's intermediate byte, or '\0' when the designator is a single final byte.
    char intermediate;

    /// The designator's final byte.
    char final;

    /// Whether this is a 96-character set. This is *also* DECAUPSS's `Ps` parameter: `Ps` is not an
    /// independent value but the set's own size, so a `Ps` that disagrees with the designator names
    /// no set at all.
    bool is96;

    /// The conformance level at which DEC introduced this set. Finer-grained than DECAUPSS's own
    /// VT320 gate: the DEC/ISO Greek, Hebrew, Turkish and Cyrillic sets are VT500-era.
    VTType minimumConformanceLevel;

    /// The charset Contour maps for this set, where it has one. Sets without a map are tracked as a
    /// designation only -- which is all UPSS can mean for a UTF-8 terminal.
    std::optional<CharsetId> charset;

    [[nodiscard]] constexpr bool operator==(UserPreferredSupplementalSet const& rhs) const noexcept
    {
        return intermediate == rhs.intermediate && final == rhs.final && is96 == rhs.is96;
    }
};

/// Every User-Preferred Supplemental Set a terminal may be asked to assign.
///
/// The table is the authority: adding a set is adding a row. Mirrors xterm's `upss_table`
/// (`misc.c`, `decode_upss`) and the sets its `ctlseqs.txt` lists under DECAUPSS. Only xterm and
/// Windows Terminal implement UPSS at all -- foot, kitty, ghostty, wezterm, konsole and xterm.js
/// have no notion of it -- so xterm is the reference here.
///
/// The `A`, `0` and `>` 94-character rows are undocumented by DEC, but xterm accepts them on the
/// evidence of real VT520/VT525 hardware, and so does Contour.
constexpr inline auto UpssTable = std::array<UserPreferredSupplementalSet, 14> {
    // 94-character sets (DECAUPSS Ps = 0).
    UserPreferredSupplementalSet { '%', '5', false, VTType::VT320, std::nullopt }, // DEC Supplemental Graphic
    UserPreferredSupplementalSet { '\0', 'A', false, VTType::VT100, CharsetId::USASCII }, // US ASCII
    UserPreferredSupplementalSet {
        '\0', '0', false, VTType::VT100, CharsetId::Special }, // DEC Special Graphic
    UserPreferredSupplementalSet { '\0', '>', false, VTType::VT320, CharsetId::Technical }, // DEC Technical
    UserPreferredSupplementalSet { '"', '?', false, VTType::VT510, std::nullopt },          // DEC Greek
    UserPreferredSupplementalSet { '"', '4', false, VTType::VT510, std::nullopt },          // DEC Hebrew
    UserPreferredSupplementalSet { '%', '0', false, VTType::VT510, std::nullopt },          // DEC Turkish
    UserPreferredSupplementalSet { '&', '4', false, VTType::VT510, std::nullopt },          // DEC Cyrillic
    // 96-character sets (DECAUPSS Ps = 1).
    UserPreferredSupplementalSet { '\0', 'A', true, VTType::VT320, CharsetId::ISOLatin1Supplemental },
    UserPreferredSupplementalSet { '\0', 'B', true, VTType::VT510, std::nullopt }, // ISO Latin-2 Supplemental
    UserPreferredSupplementalSet { '\0', 'F', true, VTType::VT510, std::nullopt }, // ISO Greek Supplemental
    UserPreferredSupplementalSet { '\0', 'H', true, VTType::VT510, std::nullopt }, // ISO Hebrew Supplemental
    UserPreferredSupplementalSet { '\0', 'M', true, VTType::VT510, std::nullopt }, // ISO Latin-5 Supplemental
    UserPreferredSupplementalSet { '\0', 'L', true, VTType::VT510, std::nullopt }, // ISO Latin-Cyrillic
};

/// The User-Preferred Supplemental Set a terminal powers up with, and that DECSTR and RIS restore:
/// DEC Supplemental Graphic. Matches xterm's `DFT_UPSS` (`ptyx.h`) and vttest's own `reset_upss()`.
constexpr inline auto DefaultUserPreferredSupplementalSet = UpssTable[0];

/// Looks a UPSS designator up in @ref UpssTable.
///
/// @param intermediate the designator's intermediate byte, or '\0' when it has none.
/// @param final the designator's final byte.
/// @param is96 the set size named by DECAUPSS's `Ps` parameter; a row whose size disagrees does not
///             match, because `Ps` names the set's size rather than selecting between readings of
///             the same designator. `A` at 94 is US ASCII while `A` at 96 is ISO Latin-1.
/// @return the matching row, or nullopt when the combination names no set.
[[nodiscard]] constexpr std::optional<UserPreferredSupplementalSet> findUserPreferredSupplementalSet(
    char intermediate, char final, bool is96) noexcept
{
    for (auto const& entry: UpssTable)
        if (entry.intermediate == intermediate && entry.final == final && entry.is96 == is96)
            return entry;
    return std::nullopt;
}

/// @returns the charset mapping table for the given charset identifier.
CharsetMap const* charsetMap(CharsetId id) noexcept;

/// @returns the SCS (Select Character Set) final character for the given charset identifier.
///
/// This is the final byte used in SCS escape sequences (e.g., 'B' for USASCII, '0' for Special).
/// Used by DECCIR (Cursor Information Report) to encode the Sdesig field.
constexpr char charsetDesignation(CharsetId id) noexcept
{
    switch (id)
    {
        case CharsetId::Special: return '0';
        case CharsetId::British: return 'A';
        case CharsetId::Dutch: return '4';
        case CharsetId::Finnish: return 'C';
        case CharsetId::French: return 'R';
        case CharsetId::FrenchCanadian: return 'Q';
        case CharsetId::German: return 'K';
        case CharsetId::NorwegianDanish: return 'E';
        case CharsetId::Spanish: return 'Z';
        case CharsetId::Swedish: return 'H';
        case CharsetId::Swiss: return '=';
        case CharsetId::Technical: return '>';
        case CharsetId::USASCII: return 'B';
        case CharsetId::ISOLatin1Supplemental: return 'A';
    }
    return 'B'; // fallback to USASCII
}

/// Charset mapping API for tables G0, G1, G2, and G3.
///
/// Relevant VT sequences are: SCS, SS2, SS3.
class CharsetMapping
{
  public:
    CharsetMapping() noexcept:
        _tables {
            charsetMap(CharsetId::USASCII),
            charsetMap(CharsetId::USASCII),
            charsetMap(CharsetId::USASCII),
            charsetMap(CharsetId::USASCII),
        }
    {
    }

    [[nodiscard]] char32_t map(char32_t code) noexcept
    {
        // TODO: could surely be implemented branchless with a jump-table and computed goto.
        if (code < 127)
        {
            auto result = map(_tableForNextGraphic, static_cast<char>(code));
            _tableForNextGraphic = _selectedTable;
            return result;
        }
        else if (code != 127)
        {
            return code;
        }
        else
        {
            return L' ';
        }
    }

    [[nodiscard]] char32_t map(CharsetTable table, char code) const noexcept
    {
        return (*_tables[static_cast<std::size_t>(table)])[static_cast<std::uint8_t>(code)];
    }

    constexpr void singleShift(CharsetTable table) noexcept { _tableForNextGraphic = table; }

    /// Invokes @p table into GL (the left graphic half, codes 0x20..0x7F), persisting until the next
    /// locking shift. This is LS0 (SI), LS1 (SO), LS2 (ESC n) and LS3 (ESC o).
    constexpr void lockingShift(CharsetTable table) noexcept
    {
        _selectedTable = table;
        _tableForNextGraphic = table;
    }

    /// Invokes @p table into GR (the right graphic half, codes 0xA0..0xFF), persisting until the next
    /// GR locking shift. This is LS1R (ESC ~), LS2R (ESC }) and LS3R (ESC |).
    ///
    /// @note Contour decodes its input as UTF-8, so bytes 0xA0..0xFF are UTF-8 continuation/lead bytes
    /// rather than 8-bit GR graphics; the GR register is therefore tracked faithful state (reported by
    /// DECCIR) but does not re-map decoded codepoints, which would corrupt Latin-1 text.
    constexpr void lockingShiftGR(CharsetTable table) noexcept { _selectedTableGR = table; }

    [[nodiscard]] bool isSelected(CharsetTable table, CharsetId id) const noexcept
    {
        return _tables[static_cast<std::size_t>(table)] == charsetMap(id);
    }

    [[nodiscard]] bool isSelected(CharsetId id) const noexcept
    {
        return isSelected(_tableForNextGraphic, id);
    }

    /// Selects a given 94-character set into the table G0, G1, G2, or G3.
    void select(CharsetTable table, CharsetId id) noexcept
    {
        _tables[static_cast<std::size_t>(table)] = charsetMap(id);
        _charsetIds[static_cast<std::size_t>(table)] = id;
        _drcsFontNumber[static_cast<std::size_t>(table)].reset();
        _is96Charset[static_cast<std::size_t>(table)] = false;
        _isUpss[static_cast<std::size_t>(table)] = false;
    }

    /// Selects a given 96-character set into the table G1, G2, or G3 (G0 cannot hold a 96-charset).
    ///
    /// @note A 96-charset occupies positions 0x20..0x7F and is designed to be invoked into GR; because
    /// Contour decodes input as UTF-8 the GR half never carries 8-bit graphics, so this mainly makes the
    /// designation observable through DECCIR (Scss size bits, Sdesig final byte).
    void select96(CharsetTable table, CharsetId id) noexcept
    {
        select(table, id);
        _is96Charset[static_cast<std::size_t>(table)] = true;
    }

    /// @returns whether the given G-set table currently holds a 96-character set.
    [[nodiscard]] constexpr bool is96Charset(CharsetTable table) const noexcept
    {
        return _is96Charset[static_cast<std::size_t>(table)];
    }

    /// Designates the User-Preferred Supplemental Set into a G-set table (the `<` SCS designator).
    ///
    /// @param table the G-set to designate into.
    /// @param upss the terminal's current UPSS, resolved here rather than at use time.
    ///
    /// @note The designation records that this G-set holds *UPSS*, not the set UPSS currently names:
    /// DECCIR must report `<` (@see charsetDesignation), because that is what was designated and the
    /// resolved set is not what the application asked for.
    ///
    /// @note Contour resolves UPSS at designation time; xterm resolves it at use time (its
    /// `HandleUPSS` macro), so a DECAUPSS arriving *after* an `ESC ( <` retroactively changes what
    /// that G-set maps. The two diverge only for a G-set designated `<` before a DECAUPSS naming one
    /// of the four sets Contour has a map for, and only in GL -- where a supplemental set is not
    /// meant to be invoked anyway. Resolving at use time would mean reaching every cursor in the
    /// terminal (both screens, every saved cursor, both status lines, every page) on each DECAUPSS.
    void selectUserPreferred(CharsetTable table, UserPreferredSupplementalSet const& upss) noexcept
    {
        select(table, upss.charset.value_or(CharsetId::USASCII));
        _is96Charset[static_cast<std::size_t>(table)] = upss.is96;
        _isUpss[static_cast<std::size_t>(table)] = true;
    }

    /// @returns whether the given G-set table was designated as the User-Preferred Supplemental Set.
    [[nodiscard]] constexpr bool isUserPreferred(CharsetTable table) const noexcept
    {
        return _isUpss[static_cast<std::size_t>(table)];
    }

    /// @returns the SCS designation final byte for the given G-set, as DECCIR's Sdesig reports it.
    ///
    /// A G-set designated `ESC ( <` reports `<`, not the set UPSS currently resolves to: `<` is a
    /// character set in its own right (xterm's `nrc_DEC_UPSS`), and reporting the resolved set would
    /// claim a designation the application never made.
    [[nodiscard]] constexpr char designationOf(CharsetTable table) const noexcept
    {
        return isUserPreferred(table) ? '<' : charsetDesignation(charsetIdOf(table));
    }

    /// Selects a DRCS font into the given G-set table.
    void selectDRCS(CharsetTable table, int fontNumber) noexcept
    {
        _drcsFontNumber[static_cast<std::size_t>(table)] = fontNumber;
        // Set the table to USASCII as fallback for unmapped positions
        _tables[static_cast<std::size_t>(table)] = charsetMap(CharsetId::USASCII);
        _charsetIds[static_cast<std::size_t>(table)] = CharsetId::USASCII;
        // This deliberately does not route through select(): it must not clear the DRCS font it has
        // just designated. So it clears the designation flags select() would have cleared -- without
        // this, `ESC ( <` followed by a DRCS designation would still report `<` through DECCIR.
        _is96Charset[static_cast<std::size_t>(table)] = false;
        _isUpss[static_cast<std::size_t>(table)] = false;
    }

    /// @returns the DRCS font number for the given G-set table, or nullopt if not a DRCS set.
    [[nodiscard]] std::optional<int> drcsFont(CharsetTable table) const noexcept
    {
        return _drcsFontNumber[static_cast<std::size_t>(table)];
    }

    /// @returns the DRCS font number for the table that will be used for the next graphic character.
    [[nodiscard]] std::optional<int> activeDRCSFont() const noexcept
    {
        return _drcsFontNumber[static_cast<std::size_t>(_tableForNextGraphic)];
    }

    /// @returns the G-set table currently mapped to GL (the active locking shift).
    [[nodiscard]] constexpr CharsetTable selectedTable() const noexcept { return _selectedTable; }

    /// @returns the G-set table currently mapped to GR (defaults to G2, per the VT standard).
    [[nodiscard]] constexpr CharsetTable selectedTableGR() const noexcept { return _selectedTableGR; }

    /// @returns the G-set table used for the next graphic character (differs from selectedTable() after
    /// SS2/SS3).
    [[nodiscard]] constexpr CharsetTable tableForNextGraphic() const noexcept { return _tableForNextGraphic; }

    /// @returns the CharsetId designated for the given G-set table.
    [[nodiscard]] constexpr CharsetId charsetIdOf(CharsetTable table) const noexcept
    {
        return _charsetIds[static_cast<std::size_t>(table)];
    }

  private:
    CharsetTable _tableForNextGraphic = CharsetTable::G0;
    CharsetTable _selectedTable = CharsetTable::G0;
    CharsetTable _selectedTableGR = CharsetTable::G2;

    using Tables = std::array<CharsetMap const*, 4>;
    Tables _tables;
    std::array<CharsetId, 4> _charsetIds = {
        CharsetId::USASCII, CharsetId::USASCII, CharsetId::USASCII, CharsetId::USASCII
    };
    std::array<bool, 4> _is96Charset = {};
    std::array<bool, 4> _isUpss = {};
    std::array<std::optional<int>, 4> _drcsFontNumber = {};
};

} // namespace vtbackend
