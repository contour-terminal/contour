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

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <string>

#include <fmt/format.h>

namespace terminal::capabilities {

// TCap Code - terminal capability code, a unique 2-byte identifier.
struct Code
{
    uint16_t code{};

    constexpr operator uint16_t () const noexcept
    {
        return code;
    }

    std::string hex() const
    {
        return fmt::format("{:02X}{:02X}", unsigned((code >> 8) & 0xFF), unsigned(code & 0xFF));
    }

    constexpr Code() = default;
    constexpr Code(Code const&) = default;
    constexpr Code& operator=(Code const&) = default;
    constexpr Code(Code &&) = default;
    constexpr Code& operator=(Code &&) = default;
    constexpr explicit Code(uint16_t _code) : code{_code} {}

    constexpr explicit Code(std::string_view _code) : code{uint16_t(_code[0] << 8 | _code[1])}
    {
        code = uint16_t(_code[0] << 8 | _code[1]);
    }
};

constexpr bool operator==(Code a, Code b) noexcept
{
    return a.code == b.code;
}

constexpr bool operator==(Code a, std::string_view b) noexcept
{
    if (b.size() != 2)
        return false;
    return a == Code(b);
}

struct Def
{
    Code code;
    std::string_view name;
};

// {{{ variable names
constexpr auto inline auto_left_margin      = Def{Code{"am"}, "am"};
constexpr auto inline can_change            = Def{Code{"cc"}, "ccc"};
constexpr auto inline eat_newline_glitch    = Def{Code{"xn"}, "xenl"};
// TODO ... (all the rest that is at least needed by us)
// }}}

namespace literals
{
    constexpr Code operator "" _tcap (char const* _code, size_t)
    {
        return Code{uint16_t(_code[0] << 8 | _code[1])};
    }
}

class Database {
  public:
    virtual ~Database() = default;

    virtual bool booleanCapability(Code _cap) const = 0;
    virtual unsigned numericCapability(Code _cap) const = 0;
    virtual std::string_view stringCapability(Code _cap) const = 0;

    virtual bool booleanCapability(std::string_view _cap) const = 0;
    virtual unsigned numericCapability(std::string_view _cap) const = 0;
    virtual std::string_view stringCapability(std::string_view _cap) const = 0;

    virtual std::optional<Code> codeFromName(std::string_view _name) const = 0;

    virtual std::string terminfo() const = 0;
};

class StaticDatabase : public Database {
  public:
    bool booleanCapability(Code _cap) const override;
    unsigned numericCapability(Code _cap) const override;
    std::string_view stringCapability(Code _cap) const override;

    bool booleanCapability(std::string_view _cap) const override;
    unsigned numericCapability(std::string_view _cap) const override;
    std::string_view stringCapability(std::string_view _cap) const override;

    std::optional<Code> codeFromName(std::string_view _name) const override;

    std::string terminfo() const override;
};

} // end namespace

namespace fmt
{
    template <>
    struct formatter<terminal::capabilities::Code> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::capabilities::Code _value, FormatContext& ctx)
        {
            if (_value.code & 0xFF0000)
                return format_to(ctx.out(), "{}{}{}", char((_value.code >> 16) & 0xFF),
                                                      char((_value.code >> 8) & 0xFF),
                                                      char(_value.code & 0xFF));

            return format_to(ctx.out(), "{}{}", char((_value.code >> 8) & 0xFF),
                                                char(_value.code & 0xFF));
        }
    };
}
