// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

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
    USASCII
};

enum class CharsetTable : std::uint8_t
{
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

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
        case CharsetId::USASCII: return 'B';
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
            return static_cast<char32_t>(code);
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

    constexpr void lockingShift(CharsetTable table) noexcept
    {
        _selectedTable = table;
        _tableForNextGraphic = table;
    }

    [[nodiscard]] bool isSelected(CharsetTable table, CharsetId id) const noexcept
    {
        return _tables[static_cast<std::size_t>(table)] == charsetMap(id);
    }

    [[nodiscard]] bool isSelected(CharsetId id) const noexcept
    {
        return isSelected(_tableForNextGraphic, id);
    }

    /// Selects a given designated character set into the table G0, G1, G2, or G3.
    void select(CharsetTable table, CharsetId id) noexcept
    {
        _tables[static_cast<std::size_t>(table)] = charsetMap(id);
        _charsetIds[static_cast<std::size_t>(table)] = id;
    }

    /// @returns the G-set table currently mapped to GL (the active locking shift).
    [[nodiscard]] constexpr CharsetTable selectedTable() const noexcept { return _selectedTable; }

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

    using Tables = std::array<CharsetMap const*, 4>;
    Tables _tables;
    std::array<CharsetId, 4> _charsetIds = {
        CharsetId::USASCII, CharsetId::USASCII, CharsetId::USASCII, CharsetId::USASCII
    };
};

} // namespace vtbackend
