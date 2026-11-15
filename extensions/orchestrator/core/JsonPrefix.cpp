// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/core/JsonPrefix.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

namespace orchestrator::json
{

namespace
{
    /// Where the scan of a JSON text stands.
    enum class Lexeme : uint8_t
    {
        Structure, ///< Outside a string.
        String,    ///< Inside a string.
        Escape,    ///< Just after a backslash inside a string.
    };

    constexpr auto Whitespace = std::string_view { " \t\r\n" };
} // namespace

std::optional<std::string> closedArrayPrefix(std::string_view text)
{
    auto const start = text.find_first_not_of(Whitespace);
    if (start == std::string_view::npos || text[start] != '[')
        return std::nullopt;
    auto lexeme = Lexeme::Structure;
    auto depth = std::size_t { 1 };
    auto closedUpTo = std::optional<std::size_t> {};
    for (auto const index: std::views::iota(start + 1, text.size()))
    {
        auto const c = text[index];
        switch (lexeme)
        {
            case Lexeme::Escape: lexeme = Lexeme::String; continue;
            case Lexeme::String:
                if (c == '\\')
                    lexeme = Lexeme::Escape;
                else if (c == '"')
                    lexeme = Lexeme::Structure;
                continue;
            case Lexeme::Structure: break;
        }
        if (c == '"')
            lexeme = Lexeme::String;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']')
        {
            // The array closed after all: it is whole, and what follows it is not part of it.
            if (--depth == 0)
                return std::string(text.substr(0, index + 1));
        }
        else if (c == ',' && depth == 1)
            closedUpTo = index;
    }
    if (!closedUpTo)
        return std::nullopt;
    return std::format("{}]", text.substr(0, *closedUpTo));
}

} // namespace orchestrator::json
