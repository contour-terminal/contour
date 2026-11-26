// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/TableCaps.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace vtextension::wasm::wamr
{

namespace
{
    /// The limits flag saying that a maximum follows the initial size.
    constexpr auto HasMaximum = std::byte { 0b0000'0001 };

    /// Appends @p value to @p out as an unsigned LEB128 number, in as few bytes as it takes.
    void appendU32(std::vector<std::byte>& out, uint32_t value)
    {
        constexpr auto PayloadMask = uint32_t { 0b0111'1111 };
        constexpr auto ContinueBit = uint32_t { 0b1000'0000 };
        while (value > PayloadMask)
        {
            out.push_back(static_cast<std::byte>((value & PayloadMask) | ContinueBit));
            value >>= 7;
        }
        out.push_back(static_cast<std::byte>(value));
    }

    /// @return Whether @p table may grow past @p maxElements as it is declared, and begins within it.
    [[nodiscard]] bool needsCap(binary::TableDeclaration const& table, uint32_t maxElements) noexcept
    {
        return table.initialElements <= maxElements && (!table.maximum || *table.maximum > maxElements);
    }

    /// @return The content of a table section declaring @p tables, each capped at @p maxElements.
    [[nodiscard]] std::vector<std::byte> cappedTableSection(std::span<binary::TableDeclaration const> tables,
                                                            uint32_t maxElements)
    {
        auto content = std::vector<std::byte> {};
        appendU32(content, static_cast<uint32_t>(tables.size()));
        for (auto const& table: tables)
        {
            auto const maximum = needsCap(table, maxElements) ? std::optional { maxElements } : table.maximum;
            content.push_back(std::byte { table.elementType });
            content.push_back(maximum ? HasMaximum : std::byte { 0 });
            appendU32(content, table.initialElements);
            if (maximum)
                appendU32(content, *maximum);
        }
        return content;
    }
} // namespace

std::expected<std::vector<std::byte>, binary::BinaryError> capTableMaxima(
    std::vector<std::byte> module, std::span<binary::TableDeclaration const> tables, uint32_t maxElements)
{
    if (std::ranges::none_of(tables, [&](auto const& table) { return needsCap(table, maxElements); }))
        return module;
    auto const all = binary::sections(module);
    if (!all)
        return std::unexpected(all.error());

    auto result = std::vector<std::byte>(module.begin(), module.begin() + binary::ModuleHeader.size());
    auto previousEnd = binary::ModuleHeader.size();
    for (auto const& section: *all)
    {
        auto const end = section.offset + section.content.size();
        if (section.id == binary::TableSectionId)
        {
            auto const content = cappedTableSection(tables, maxElements);
            result.push_back(std::byte { binary::TableSectionId });
            appendU32(result, static_cast<uint32_t>(content.size()));
            result.insert(result.end(), content.begin(), content.end());
        }
        else
            result.insert(result.end(),
                          module.begin() + static_cast<std::ptrdiff_t>(previousEnd),
                          module.begin() + static_cast<std::ptrdiff_t>(end));
        previousEnd = end;
    }
    return result;
}

} // namespace vtextension::wasm::wamr
