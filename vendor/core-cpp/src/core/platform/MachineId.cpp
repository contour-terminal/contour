// SPDX-License-Identifier: Apache-2.0
#include <core/platform/MachineId.hpp>

#include <core/platform/StringUtils.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace core::platform
{

namespace
{
    constexpr auto MachineIdPath = std::string_view { "/etc/machine-id" };

    /// @brief The length of a machine id: 128 bits as hexadecimal digits.
    constexpr auto MachineIdLength = std::size_t { 32 };

    [[nodiscard]] constexpr bool isHexDigit(char ch) noexcept
    {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
    }

    [[nodiscard]] constexpr char toLowerHex(char ch) noexcept
    {
        return (ch >= 'A' && ch <= 'F') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
} // namespace

std::optional<std::string> readMachineId(FileSystem const& fileSystem)
{
    return fileSystem.readFile(std::filesystem::path { MachineIdPath })
        .transform([](std::string content) {
            trimInPlace(content);
            return content;
        })
        .transform([](std::string content) -> std::optional<std::string> {
            if (content.size() != MachineIdLength || !std::ranges::all_of(content, isHexDigit))
                return std::nullopt;
            std::ranges::transform(content, content.begin(), toLowerHex);
            return content;
        })
        .value_or(std::nullopt);
}

} // namespace core::platform
