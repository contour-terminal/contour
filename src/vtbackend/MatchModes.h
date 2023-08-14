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

#include <cstdint>

namespace terminal
{

class match_modes
{
  public:
    enum flag : uint8_t
    {
        default_flag = 0x00,
        alternate_screen = 0x01,
        app_cursor = 0x02,
        app_keypad = 0x04,
        insert = 0x08, // vi-like insert mode
        select = 0x10, // visual / visual-line / visual-block
        search = 0x20, // something's in the search buffer
        trace = 0x40,
    };

    enum class status
    {
        Any,
        Enabled,
        Disabled
    };

    [[nodiscard]] constexpr status getStatus(flag flag) const noexcept
    {
        if (_enabled & static_cast<uint8_t>(flag))
            return status::Enabled;
        if (_disabled & static_cast<uint8_t>(flag))
            return status::Disabled;
        return status::Any;
    }

    [[nodiscard]] constexpr unsigned enabled() const noexcept { return _enabled; }
    [[nodiscard]] constexpr unsigned disabled() const noexcept { return _disabled; }

    constexpr void enable(flag flag) noexcept
    {
        _enabled |= static_cast<uint8_t>(flag);
        _disabled = (uint8_t) (_disabled & ~static_cast<uint8_t>(flag));
    }

    constexpr void disable(flag flag) noexcept
    {
        _enabled = (uint8_t) (_enabled & ~static_cast<uint8_t>(flag));
        _disabled |= static_cast<uint8_t>(flag);
    }

    [[nodiscard]] constexpr bool has_value(flag flag) const noexcept
    {
        return _enabled & static_cast<uint8_t>(flag) || _disabled & static_cast<uint8_t>(flag);
    }

    constexpr void clear(flag flag) noexcept
    {
        _enabled = (uint8_t) (_enabled & ~static_cast<uint8_t>(flag));
        _disabled = (uint8_t) (_disabled & ~static_cast<uint8_t>(flag));
    }

    constexpr void reset() noexcept
    {
        _enabled = 0;
        _disabled = 0;
    }

    [[nodiscard]] constexpr bool any() const noexcept { return _enabled || _disabled; }

    [[nodiscard]] constexpr uint16_t hashcode() const noexcept
    {
        return static_cast<uint16_t>(_enabled << 8 | _disabled);
    }

    constexpr match_modes() noexcept = default;
    constexpr match_modes(match_modes const&) noexcept = default;
    constexpr match_modes(match_modes&&) noexcept = default;
    constexpr match_modes& operator=(match_modes const&) noexcept = default;
    constexpr match_modes& operator=(match_modes&&) noexcept = default;

    constexpr match_modes(uint8_t enabled, uint8_t disabled) noexcept:
        _enabled { enabled }, _disabled { disabled }
    {
    }

  private:
    uint8_t _enabled = 0;
    uint8_t _disabled = 0;
};

constexpr bool operator==(match_modes a, match_modes b) noexcept
{
    return a.hashcode() == b.hashcode();
}

constexpr bool operator!=(match_modes a, match_modes b) noexcept
{
    return !(a == b);
}

} // namespace terminal

// {{{ fmtlib support
template <>
struct fmt::formatter<terminal::match_modes>: formatter<std::string>
{
    auto format(terminal::match_modes m, format_context& ctx) -> format_context::iterator
    {
        std::string s;
        auto const advance = [&](terminal::match_modes::flag cond, std::string_view text) {
            auto const status = m.getStatus(cond);
            if (status == terminal::match_modes::status::Any)
                return;
            if (!s.empty())
                s += '|';
            if (status == terminal::match_modes::status::Disabled)
                s += "~";
            s += text;
        };
        advance(terminal::match_modes::app_cursor, "AppCursor");
        advance(terminal::match_modes::app_keypad, "AppKeypad");
        advance(terminal::match_modes::alternate_screen, "AltScreen");
        advance(terminal::match_modes::insert, "Insert");
        advance(terminal::match_modes::select, "Select");
        advance(terminal::match_modes::search, "Search");
        advance(terminal::match_modes::trace, "Trace");
        if (s.empty())
            s = "Any";
        return formatter<std::string>::format(s, ctx);
    }
};
// }}}
