/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <fmt/format.h>

#include <array>

namespace terminal
{

using charset_map = std::array<char32_t, 127>;

enum class charset_id
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

enum class charset_table
{
    G0 = 0,
    G1 = 1,
    G2 = 2,
    G3 = 3
};

/// @returns the charset
charset_map const* charsetMap(charset_id id) noexcept;

/// Charset mapping API for tables G0, G1, G2, and G3.
///
/// Relevant VT sequences are: SCS, SS2, SS3.
class charset_mapping
{
  public:
    charset_mapping() noexcept:
        _tables {
            charsetMap(charset_id::USASCII),
            charsetMap(charset_id::USASCII),
            charsetMap(charset_id::USASCII),
            charsetMap(charset_id::USASCII),
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

    [[nodiscard]] char32_t map(charset_table table, char code) const noexcept
    {
        return (*_tables[static_cast<size_t>(table)])[static_cast<uint8_t>(code)];
    }

    constexpr void singleShift(charset_table table) noexcept { _tableForNextGraphic = table; }

    constexpr void lockingShift(charset_table table) noexcept
    {
        _selectedTable = table;
        _tableForNextGraphic = table;
    }

    [[nodiscard]] bool isSelected(charset_table table, charset_id id) const noexcept
    {
        return _tables[static_cast<size_t>(table)] == charsetMap(id);
    }

    [[nodiscard]] bool isSelected(charset_id id) const noexcept
    {
        return isSelected(_tableForNextGraphic, id);
    }

    // Selects a given designated character set into the table G0, G1, G2, or G3.
    void select(charset_table table, charset_id id) noexcept
    {
        _tables[static_cast<size_t>(table)] = charsetMap(id);
    }

  private:
    charset_table _tableForNextGraphic = charset_table::G0;
    charset_table _selectedTable = charset_table::G0;

    using tables = std::array<charset_map const*, 4>;
    tables _tables;
};

} // namespace terminal
