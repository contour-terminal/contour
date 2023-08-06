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

// TCap code - terminal capability code, a unique 2-byte identifier.
struct code
{
    uint16_t c {};

    constexpr operator uint16_t() const noexcept { return c; }

    [[nodiscard]] std::string hex() const
    {
        return fmt::format("{:02X}{:02X}", unsigned((c >> 8) & 0xFF), unsigned(c & 0xFF));
    }

    constexpr code() noexcept = default;
    constexpr code(code const&) noexcept = default;
    constexpr code& operator=(code const&) noexcept = default;
    constexpr code(code&&) noexcept = default;
    constexpr code& operator=(code&&) noexcept = default;
    constexpr explicit code(uint16_t c) noexcept: c { c } {}

    constexpr explicit code(std::string_view value) noexcept: code { uint16_t(value[0] << 8 | value[1]) } {}
};

constexpr bool operator==(code a, code b) noexcept
{
    return a.c == b.c;
}

constexpr bool operator==(code a, std::string_view b) noexcept
{
    if (b.size() != 2)
        return false;
    return a == code(b);
}

struct def
{
    code code;
    std::string_view name;
};

// {{{ variable names
constexpr auto inline auto_left_margin = def { code { "am" }, "am" };
constexpr auto inline can_change = def { code { "cc" }, "ccc" };
constexpr auto inline eat_newline_glitch = def { code { "xn" }, "xenl" };
// TODO ... (all the rest that is at least needed by us)
// }}}

namespace literals
{
    constexpr code operator"" _tcap(char const* c, size_t)
    {
        return code { uint16_t(c[0] << 8 | c[1]) };
    }
} // namespace literals

class database
{
  public:
    constexpr static inline unsigned npos = unsigned(-1);

    virtual ~database() = default;

    [[nodiscard]] virtual bool booleanCapability(code value) const = 0;
    [[nodiscard]] virtual unsigned numericCapability(code value) const = 0;
    [[nodiscard]] virtual std::string_view stringCapability(code value) const = 0;

    [[nodiscard]] virtual bool booleanCapability(std::string_view value) const = 0;
    [[nodiscard]] virtual unsigned numericCapability(std::string_view value) const = 0;
    [[nodiscard]] virtual std::string_view stringCapability(std::string_view value) const = 0;

    [[nodiscard]] virtual std::optional<code> codeFromName(std::string_view name) const = 0;

    [[nodiscard]] virtual std::string terminfo() const = 0;
};

class static_database: public database
{
  public:
    [[nodiscard]] bool booleanCapability(code code) const override;
    [[nodiscard]] unsigned numericCapability(code code) const override;
    [[nodiscard]] std::string_view stringCapability(code code) const override;

    [[nodiscard]] bool booleanCapability(std::string_view name) const override;
    [[nodiscard]] unsigned numericCapability(std::string_view name) const override;
    [[nodiscard]] std::string_view stringCapability(std::string_view name) const override;

    [[nodiscard]] std::optional<code> codeFromName(std::string_view name) const override;

    [[nodiscard]] std::string terminfo() const override;
};

} // namespace terminal::capabilities

template <>
struct fmt::formatter<terminal::capabilities::code>: fmt::formatter<std::string>
{
    auto format(terminal::capabilities::code value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(value.hex(), ctx);
    }
};
