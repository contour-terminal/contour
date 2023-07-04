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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace terminal::capabilities
{

// TCap Code - terminal capability code, a unique 2-byte identifier.
struct Code
{
    uint16_t code {};

    constexpr operator uint16_t() const noexcept { return code; }

    [[nodiscard]] std::string hex() const
    {
        return fmt::format("{:02X}{:02X}", unsigned((code >> 8) & 0xFF), unsigned(code & 0xFF));
    }

    constexpr Code() noexcept = default;
    constexpr Code(Code const&) noexcept = default;
    constexpr Code& operator=(Code const&) noexcept = default;
    constexpr Code(Code&&) noexcept = default;
    constexpr Code& operator=(Code&&) noexcept = default;
    constexpr explicit Code(uint16_t code) noexcept: code { code } {}

    constexpr explicit Code(std::string_view value) noexcept: code { uint16_t(value[0] << 8 | value[1]) } {}
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
constexpr auto inline auto_left_margin = Def { Code { "am" }, "am" };
constexpr auto inline can_change = Def { Code { "cc" }, "ccc" };
constexpr auto inline eat_newline_glitch = Def { Code { "xn" }, "xenl" };
// TODO ... (all the rest that is at least needed by us)
// }}}

namespace literals
{
    constexpr Code operator"" _tcap(char const* code, size_t)
    {
        return Code { uint16_t(code[0] << 8 | code[1]) };
    }
} // namespace literals

class Database
{
  public:
    constexpr static inline unsigned npos = unsigned(-1);

    virtual ~Database() = default;

    [[nodiscard]] virtual bool booleanCapability(Code value) const = 0;
    [[nodiscard]] virtual unsigned numericCapability(Code value) const = 0;
    [[nodiscard]] virtual std::string_view stringCapability(Code value) const = 0;

    [[nodiscard]] virtual bool booleanCapability(std::string_view value) const = 0;
    [[nodiscard]] virtual unsigned numericCapability(std::string_view value) const = 0;
    [[nodiscard]] virtual std::string_view stringCapability(std::string_view value) const = 0;

    [[nodiscard]] virtual std::optional<Code> codeFromName(std::string_view name) const = 0;

    [[nodiscard]] virtual std::string terminfo() const = 0;
};

class StaticDatabase: public Database
{
  public:
    [[nodiscard]] bool booleanCapability(Code code) const override;
    [[nodiscard]] unsigned numericCapability(Code code) const override;
    [[nodiscard]] std::string_view stringCapability(Code code) const override;

    [[nodiscard]] bool booleanCapability(std::string_view name) const override;
    [[nodiscard]] unsigned numericCapability(std::string_view name) const override;
    [[nodiscard]] std::string_view stringCapability(std::string_view name) const override;

    [[nodiscard]] std::optional<Code> codeFromName(std::string_view name) const override;

    [[nodiscard]] std::string terminfo() const override;
};

} // namespace terminal::capabilities

template <>
struct fmt::formatter<terminal::capabilities::Code>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(terminal::capabilities::Code value, format_context& ctx) -> format_context::iterator
    {
        if (value.code & 0xFF0000)
            return fmt::format_to(ctx.out(),
                                  "{}{}{}",
                                  char((value.code >> 16) & 0xFF),
                                  char((value.code >> 8) & 0xFF),
                                  char(value.code & 0xFF));

        return fmt::format_to(ctx.out(), "{}{}", char((value.code >> 8) & 0xFF), char(value.code & 0xFF));
    }
};
