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

class MatchModes
{
  public:
    enum Flag : uint8_t
    {
        Default = 0x00,
        AlternateScreen = 0x01,
        AppCursor = 0x02,
        AppKeypad = 0x04,
        Insert = 0x08, // vi-like insert mode
        Select = 0x10, // visual / visual-line / visual-block
        Search = 0x20, // something's in the search buffer
        Trace = 0x40,
    };

    enum class Status
    {
        Any,
        Enabled,
        Disabled
    };

    [[nodiscard]] constexpr Status status(Flag flag) const noexcept
    {
        if (_enabled & static_cast<uint8_t>(flag))
            return Status::Enabled;
        if (_disabled & static_cast<uint8_t>(flag))
            return Status::Disabled;
        return Status::Any;
    }

    [[nodiscard]] constexpr unsigned enabled() const noexcept { return _enabled; }
    [[nodiscard]] constexpr unsigned disabled() const noexcept { return _disabled; }

    constexpr void enable(Flag flag) noexcept
    {
        _enabled |= static_cast<uint8_t>(flag);
        _disabled = (uint8_t) (_disabled & ~static_cast<uint8_t>(flag));
    }

    constexpr void disable(Flag flag) noexcept
    {
        _enabled = (uint8_t) (_enabled & ~static_cast<uint8_t>(flag));
        _disabled |= static_cast<uint8_t>(flag);
    }

    [[nodiscard]] constexpr bool has_value(Flag flag) const noexcept
    {
        return _enabled & static_cast<uint8_t>(flag) || _disabled & static_cast<uint8_t>(flag);
    }

    constexpr void clear(Flag flag) noexcept
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

    constexpr MatchModes() noexcept = default;
    constexpr MatchModes(MatchModes const&) noexcept = default;
    constexpr MatchModes(MatchModes&&) noexcept = default;
    constexpr MatchModes& operator=(MatchModes const&) noexcept = default;
    constexpr MatchModes& operator=(MatchModes&&) noexcept = default;

    constexpr MatchModes(uint8_t enabled, uint8_t disabled) noexcept:
        _enabled { enabled }, _disabled { disabled }
    {
    }

  private:
    uint8_t _enabled = 0;
    uint8_t _disabled = 0;
};

constexpr bool operator==(MatchModes a, MatchModes b) noexcept
{
    return a.hashcode() == b.hashcode();
}

constexpr bool operator!=(MatchModes a, MatchModes b) noexcept
{
    return !(a == b);
}

} // namespace terminal

// {{{ fmtlib support
template <>
struct fmt::formatter<terminal::MatchModes>: formatter<std::string>
{
    auto format(terminal::MatchModes m, format_context& ctx) -> format_context::iterator
    {
        std::string s;
        auto const advance = [&](terminal::MatchModes::Flag cond, std::string_view text) {
            auto const status = m.status(cond);
            if (status == terminal::MatchModes::Status::Any)
                return;
            if (!s.empty())
                s += '|';
            if (status == terminal::MatchModes::Status::Disabled)
                s += "~";
            s += text;
        };
        advance(terminal::MatchModes::AppCursor, "AppCursor");
        advance(terminal::MatchModes::AppKeypad, "AppKeypad");
        advance(terminal::MatchModes::AlternateScreen, "AltScreen");
        advance(terminal::MatchModes::Insert, "Insert");
        advance(terminal::MatchModes::Select, "Select");
        advance(terminal::MatchModes::Search, "Search");
        advance(terminal::MatchModes::Trace, "Trace");
        if (s.empty())
            s = "Any";
        return formatter<std::string>::format(s, ctx);
    }
};
// }}}
