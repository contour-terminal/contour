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
        Select = 0x08,
        // future modes
        // ViSearch            = 0x10, // TODO: This mode we want.
    };

    enum class Status
    {
        Any,
        Enabled,
        Disabled
    };

    constexpr Status status(Flag _flag) const noexcept
    {
        if (enabled_ & static_cast<uint8_t>(_flag))
            return Status::Enabled;
        if (disabled_ & static_cast<uint8_t>(_flag))
            return Status::Disabled;
        return Status::Any;
    }

    constexpr unsigned enabled() const noexcept { return enabled_; }
    constexpr unsigned disabled() const noexcept { return disabled_; }

    constexpr void enable(Flag _flag) noexcept
    {
        enabled_ |= static_cast<uint8_t>(_flag);
        disabled_ = (uint8_t) (disabled_ & ~static_cast<uint8_t>(_flag));
    }

    constexpr void disable(Flag _flag) noexcept
    {
        enabled_ = (uint8_t) (enabled_ & ~static_cast<uint8_t>(_flag));
        disabled_ |= static_cast<uint8_t>(_flag);
    }

    constexpr bool has_value(Flag _flag) const noexcept
    {
        return enabled_ & static_cast<uint8_t>(_flag) || disabled_ & static_cast<uint8_t>(_flag);
    }

    constexpr void clear(Flag _flag) noexcept
    {
        enabled_ = (uint8_t) (enabled_ & ~static_cast<uint8_t>(_flag));
        disabled_ = (uint8_t) (disabled_ & ~static_cast<uint8_t>(_flag));
    }

    constexpr void reset() noexcept
    {
        enabled_ = 0;
        disabled_ = 0;
    }

    constexpr bool any() const noexcept { return enabled_ || disabled_; }

    constexpr uint16_t hashcode() const noexcept { return static_cast<uint16_t>(enabled_ << 8 | disabled_); }

  private:
    uint8_t enabled_ = 0;
    uint8_t disabled_ = 0;
};

constexpr bool operator==(MatchModes a, MatchModes b) noexcept
{
    return a.hashcode() == b.hashcode();
}

constexpr bool operator!=(MatchModes a, MatchModes b) noexcept
{
    return !(a == b);
}

class Terminal;

bool testMatch(Terminal const& _terminal, MatchModes _mode);

} // namespace terminal

namespace fmt
{ // {{{
template <>
struct formatter<terminal::MatchModes>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::MatchModes m, FormatContext& _ctx)
    {
        std::string s;
        auto const advance = [&](terminal::MatchModes::Flag _cond, std::string_view _text) {
            auto const status = m.status(_cond);
            if (status == terminal::MatchModes::Status::Any)
                return;
            if (!s.empty())
                s += '|';
            if (status == terminal::MatchModes::Status::Disabled)
                s += "~";
            s += _text;
        };
        advance(terminal::MatchModes::AppCursor, "AppCursor");
        advance(terminal::MatchModes::AppKeypad, "AppKeypad");
        advance(terminal::MatchModes::AlternateScreen, "AltScreen");
        advance(terminal::MatchModes::Select, "Select");
        if (s.empty())
            s = "Any";
        return format_to(_ctx.out(), "{}", s);
    }
};
} // namespace fmt
// }}}
