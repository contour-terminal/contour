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

#include <terminal/Sequencer.h>

#include <array>
#include <fmt/format.h>

namespace terminal {

using CharsetMap = std::array<char32_t, 127>;

/// @returns the charset
CharsetMap const* charsetMap(CharsetId _id) noexcept;

/// Charset mapping API for tables G0, G1, G2, and G3.
///
/// Relevant VT sequences are: SCS, SS2, SS3.
class CharsetMapping {
  public:
    CharsetMapping() noexcept :
        tables_{
            charsetMap(CharsetId::USASCII),
            charsetMap(CharsetId::USASCII),
            charsetMap(CharsetId::USASCII),
            charsetMap(CharsetId::USASCII),
        }
    {}

    char32_t map(char32_t _code) noexcept
    {
        // TODO: could surely be implemented branchless with a jump-table and computed goto.
        if (_code < 127)
        {
            auto result = map(shift_, static_cast<char>(_code));
            shift_ = selected_;
            return result;
        }
        else if (_code != 127)
        {
            return static_cast<char32_t>(_code);
        }
        else
        {
            return L' ';
        }
    }

    char32_t map(CharsetTable _table, char _code) const noexcept
    {
        return (*tables_[static_cast<size_t>(_table)])[static_cast<uint8_t>(_code)];
    }

    constexpr void singleShift(CharsetTable _table) noexcept
    {
        shift_ = _table;
    }

    constexpr void selectDefaultTable(CharsetTable _table) noexcept
    {
        selected_ = _table;
        shift_ = _table;
    }

    void select(CharsetTable _table, CharsetId _id) noexcept
    {
        tables_[static_cast<size_t>(_table)] = charsetMap(_id);
    }

    constexpr CharsetTable currentTable() const noexcept { return shift_; }

  private:
    CharsetTable shift_ = CharsetTable::G0;
    CharsetTable selected_ = CharsetTable::G0;

    using Tables = std::array<CharsetMap const*, 4>;
    Tables tables_;
};

} // end namespace
