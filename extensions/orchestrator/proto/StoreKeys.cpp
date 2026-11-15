// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/StoreKeys.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace orchestrator::proto
{

namespace
{
    constexpr auto HexDigits = std::string_view { "0123456789ABCDEF" };

    [[nodiscard]] constexpr bool isKept(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'
               || c == '-';
    }

    [[nodiscard]] constexpr std::optional<int> hexValue(char c) noexcept
    {
        auto const at = HexDigits.find(c);
        if (at == std::string_view::npos)
            return std::nullopt;
        return static_cast<int>(at);
    }

} // namespace

std::string escapeKey(std::string_view key)
{
    auto name = std::string {};
    for (auto const c: key)
    {
        if (isKept(c))
        {
            name += c;
            continue;
        }
        auto const byte = static_cast<unsigned char>(c);
        name += '%';
        name += HexDigits[byte >> 4];
        name += HexDigits[byte & 0xF];
    }
    return name;
}

std::optional<std::string> unescapeKey(std::string_view fileName)
{
    auto key = std::string {};
    while (!fileName.empty())
    {
        if (fileName.front() != '%')
        {
            if (!isKept(fileName.front()))
                return std::nullopt;
            key += fileName.front();
            fileName.remove_prefix(1);
            continue;
        }
        if (fileName.size() < 3)
            return std::nullopt;
        auto const high = hexValue(fileName[1]);
        auto const low = hexValue(fileName[2]);
        if (!high || !low)
            return std::nullopt;
        auto const decoded = static_cast<char>((*high << 4) | *low);
        // escapeKey() never escapes a kept byte: a name that does is not one of its own.
        if (isKept(decoded))
            return std::nullopt;
        key += decoded;
        fileName.remove_prefix(3);
    }
    return key;
}

} // namespace orchestrator::proto
