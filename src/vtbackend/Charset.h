// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <array>

namespace terminal
{

using CharsetMap = std::array<char32_t, 127>;

enum class CharsetId
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

enum class CharsetTable
{
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

/// @returns the charset
CharsetMap const* charsetMap(CharsetId id) noexcept;

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
        return (*_tables[static_cast<size_t>(table)])[static_cast<uint8_t>(code)];
    }

    constexpr void singleShift(CharsetTable table) noexcept { _tableForNextGraphic = table; }

    constexpr void lockingShift(CharsetTable table) noexcept
    {
        _selectedTable = table;
        _tableForNextGraphic = table;
    }

    [[nodiscard]] bool isSelected(CharsetTable table, CharsetId id) const noexcept
    {
        return _tables[static_cast<size_t>(table)] == charsetMap(id);
    }

    [[nodiscard]] bool isSelected(CharsetId id) const noexcept
    {
        return isSelected(_tableForNextGraphic, id);
    }

    // Selects a given designated character set into the table G0, G1, G2, or G3.
    void select(CharsetTable table, CharsetId id) noexcept
    {
        _tables[static_cast<size_t>(table)] = charsetMap(id);
    }

  private:
    CharsetTable _tableForNextGraphic = CharsetTable::G0;
    CharsetTable _selectedTable = CharsetTable::G0;

    using Tables = std::array<CharsetMap const*, 4>;
    Tables _tables;
};

} // namespace terminal
