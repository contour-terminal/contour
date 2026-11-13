// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wamr/HiddenExports.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <vector>

#include <vtextension/wasm/WasmBinary.hpp>

namespace vtextension::wasm::wamr
{

namespace
{
    /// @return What @p error of the binary reader is to hideExports().
    [[nodiscard]] constexpr ModuleBytesError moduleBytesErrorOf(binary::BinaryError error) noexcept
    {
        switch (error)
        {
            case binary::BinaryError::NotAModule: return ModuleBytesError::NotAModule;
            case binary::BinaryError::Truncated: return ModuleBytesError::Truncated;
            case binary::BinaryError::Malformed: return ModuleBytesError::Malformed;
        }
        return ModuleBytesError::Malformed;
    }

    /// @return Whether @p bytes spell @p text.
    [[nodiscard]] bool spells(std::span<std::byte const> bytes, std::string_view text) noexcept
    {
        return std::ranges::equal(
            bytes, text, [](std::byte byte, char c) { return byte == static_cast<std::byte>(c); });
    }

    /// One export name to rename: where it starts in the module, and what it becomes.
    struct Rename
    {
        std::size_t offset;     ///< The name's first byte, counted from the module's start.
        std::string_view alias; ///< What it is renamed to.
    };

    /// Reads an export section's entries.
    /// @param section The section's content.
    /// @return The names in it to rename; `ReservedExportName` if one is an alias already.
    [[nodiscard]] std::expected<std::vector<Rename>, ModuleBytesError> renamesIn(binary::Reader section)
    {
        auto const count = section.u32();
        if (!count)
            return std::unexpected(moduleBytesErrorOf(count.error()));
        auto renames = std::vector<Rename> {};
        // A count beyond what the section holds ends at the first entry that runs past it.
        for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, *count))
        {
            auto const nameLength = section.u32();
            if (!nameLength)
                return std::unexpected(moduleBytesErrorOf(nameLength.error()));
            auto const offset = section.position();
            auto const name = section.bytes(*nameLength);
            if (!name)
                return std::unexpected(moduleBytesErrorOf(name.error()));
            // The kind, then the index of what is exported: neither matters here.
            if (auto const kind = section.byte(); !kind)
                return std::unexpected(moduleBytesErrorOf(kind.error()));
            if (auto const exported = section.u32(); !exported)
                return std::unexpected(moduleBytesErrorOf(exported.error()));

            if (std::ranges::any_of(HiddenExports,
                                    [&](HiddenExport const& row) { return spells(*name, row.alias); }))
                return std::unexpected(ModuleBytesError::ReservedExportName);
            auto const row = std::ranges::find_if(
                HiddenExports, [&](HiddenExport const& candidate) { return spells(*name, candidate.name); });
            if (row != HiddenExports.end())
                renames.push_back(Rename { .offset = offset, .alias = row->alias });
        }
        if (!section.atEnd())
            return std::unexpected(ModuleBytesError::Malformed);
        return renames;
    }

    /// Finds the export section of @p module and reads what in it to rename.
    /// @return The renames, none if the module has no export section; `DuplicateSection` if it has two.
    [[nodiscard]] std::expected<std::vector<Rename>, ModuleBytesError> renamesOf(
        std::span<std::byte const> module)
    {
        auto const all = binary::sections(module);
        if (!all)
            return std::unexpected(moduleBytesErrorOf(all.error()));
        auto exportSections = *all | std::views::filter([](binary::Section const& section) {
            return section.id == binary::ExportSectionId;
        });
        auto const first = exportSections.begin();
        if (first == exportSections.end())
            return std::vector<Rename> {};
        if (std::ranges::next(first) != exportSections.end())
            return std::unexpected(ModuleBytesError::DuplicateSection);
        return renamesIn(binary::Reader { first->content, first->offset });
    }
} // namespace

std::expected<ExportsHidden, ModuleBytesError> hideExports(std::span<std::byte> module)
{
    // Every rename is found before the first is written, so that bytes it refuses stay as they were.
    return renamesOf(module).transform([&](std::vector<Rename> const& renames) {
        for (auto const& [offset, alias]: renames)
            std::ranges::transform(
                alias, module.subspan(offset).begin(), [](char c) { return static_cast<std::byte>(c); });
        return renames.empty() ? ExportsHidden::No : ExportsHidden::Yes;
    });
}

bool isInstantiationHook(std::string_view runtimeName) noexcept
{
    return std::ranges::contains(InstantiationHooks, runtimeName);
}

std::string_view hostExportName(std::string_view runtimeName) noexcept
{
    auto const row = std::ranges::find(HiddenExports, runtimeName, &HiddenExport::alias);
    return row != HiddenExports.end() ? row->name : runtimeName;
}

std::string_view runtimeExportName(std::string_view hostName) noexcept
{
    auto const row = std::ranges::find(HiddenExports, hostName, &HiddenExport::name);
    return row != HiddenExports.end() ? row->alias : hostName;
}

} // namespace vtextension::wasm::wamr
