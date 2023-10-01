// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend::capabilities
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
constexpr auto inline AutoLeftMargin = Def { Code { "am" }, "am" };
constexpr auto inline CanChange = Def { Code { "cc" }, "ccc" };
constexpr auto inline EatNewlineGlitch = Def { Code { "xn" }, "xenl" };
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
    constexpr static inline unsigned Npos = unsigned(-1);

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

} // namespace vtbackend::capabilities

template <>
struct fmt::formatter<vtbackend::capabilities::Code>: fmt::formatter<std::string>
{
    auto format(vtbackend::capabilities::Code value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(value.hex(), ctx);
    }
};
