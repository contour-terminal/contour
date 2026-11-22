// SPDX-License-Identifier: Apache-2.0
//
// Test-only: what a daemon-world guest module must be as built, read from its bytes alone -- the checks a
// toolchain's output is held to before any runtime sees it.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/DaemonImports.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/WasmBinary.hpp>

namespace vtextension::testing
{

namespace detail
{
    /// The id of the import section.
    inline constexpr auto ImportSectionId = uint8_t { 2 };

    /// The export a reactor must not have: the static constructors, which WAMR would run by itself while
    /// instantiating a module that imports no WASI, before any deadline guards it.
    inline constexpr auto CallCtorsExport = std::string_view { "__wasm_call_ctors" };

    /// An import's kind byte, which says what follows the import's names.
    enum class ImportKind : uint8_t
    {
        Function = 0x00, ///< A type index.
        Table = 0x01,    ///< A reference type, then limits.
        Memory = 0x02,   ///< Limits.
        Global = 0x03,   ///< A value type, then mutability.
        Tag = 0x04,      ///< An attribute, then a type index.
    };

    using Reader = wasm::binary::Reader;
    using Error = wasm::binary::BinaryError;

    /// @return @p bytes as text.
    [[nodiscard]] inline std::string text(std::span<std::byte const> bytes)
    {
        auto result = std::string(bytes.size(), '\0');
        std::ranges::transform(
            bytes, result.begin(), [](std::byte value) { return static_cast<char>(value); });
        return result;
    }

    /// Skips a table's or a memory's limits: a flags byte, a minimum, and a maximum if flag bit 0 is set.
    [[nodiscard]] inline std::expected<void, Error> skipLimits(Reader& reader)
    {
        auto const flags = reader.byte();
        if (!flags)
            return std::unexpected(flags.error());
        auto const minimum = reader.u32();
        if (!minimum)
            return std::unexpected(minimum.error());
        if ((*flags & 0b1U) != 0)
            if (auto const maximum = reader.u32(); !maximum)
                return std::unexpected(maximum.error());
        return {};
    }

    /// Skips what follows an import's names, by its kind.
    [[nodiscard]] inline std::expected<void, Error> skipImportDescription(Reader& reader, uint8_t kind)
    {
        auto const skipBytes = [&reader](std::size_t count) -> std::expected<void, Error> {
            return reader.bytes(count).transform([](auto) {});
        };
        auto const skipIndex = [&reader]() -> std::expected<void, Error> {
            return reader.u32().transform([](auto) {});
        };
        switch (static_cast<ImportKind>(kind))
        {
            case ImportKind::Function: return skipIndex();
            case ImportKind::Table: return skipBytes(1).and_then([&reader] { return skipLimits(reader); });
            case ImportKind::Memory: return skipLimits(reader);
            case ImportKind::Global: return skipBytes(2);
            case ImportKind::Tag: return skipBytes(1).and_then(skipIndex);
        }
        return std::unexpected(Error::Malformed);
    }

    /// @return Whether the daemon world or WASI's allow-list names the import @p module.@p name.
    [[nodiscard]] inline bool isAllowedImport(std::string_view module, std::string_view name) noexcept
    {
        if (module == DaemonImportModule)
            return std::ranges::contains(DaemonImportRows, name, &DaemonImportRow::name);
        if (module == wasm::WasiModule)
            return std::ranges::contains(wasm::AllowedWasiFunctions, name);
        return false;
    }

    /// Adds a problem for every import of an import section outside the daemon world and WASI's allow-list.
    [[nodiscard]] inline std::expected<void, Error> checkImports(std::span<std::byte const> content,
                                                                 std::vector<std::string>& problems)
    {
        auto reader = Reader { content };
        auto const count = reader.u32();
        if (!count)
            return std::unexpected(count.error());
        for ([[maybe_unused]] auto const _: std::views::iota(uint32_t { 0 }, *count))
        {
            auto const module = reader.name();
            if (!module)
                return std::unexpected(module.error());
            auto const name = reader.name();
            if (!name)
                return std::unexpected(name.error());
            auto const kind = reader.byte();
            if (!kind)
                return std::unexpected(kind.error());
            if (!isAllowedImport(text(*module), text(*name)))
                problems.push_back(
                    std::format("an import outside the daemon world: {}.{}", text(*module), text(*name)));
            if (auto const skipped = skipImportDescription(reader, *kind); !skipped)
                return skipped;
        }
        return {};
    }

    /// Adds a problem if an export section exports CallCtorsExport.
    [[nodiscard]] inline std::expected<void, Error> checkExports(std::span<std::byte const> content,
                                                                 std::vector<std::string>& problems)
    {
        auto reader = Reader { content };
        auto const count = reader.u32();
        if (!count)
            return std::unexpected(count.error());
        for ([[maybe_unused]] auto const _: std::views::iota(uint32_t { 0 }, *count))
        {
            auto const name = reader.name();
            if (!name)
                return std::unexpected(name.error());
            if (auto const kindAndIndex = reader.byte().and_then([&reader](uint8_t) { return reader.u32(); });
                !kindAndIndex)
                return std::unexpected(kindAndIndex.error());
            if (text(*name) == CallCtorsExport)
                problems.push_back(std::format("exports {}", CallCtorsExport));
        }
        return {};
    }
} // namespace detail

/// Problems of a daemon-world module as built: not exactly one manifest section, a start section, an
/// `__wasm_call_ctors` export, an import outside DaemonImportRows and WASI's allow-list. Empty: none.
/// @param module A WebAssembly core module's bytes.
/// @return Each problem, in words, in the order the module's sections show them; one problem alone if the
///         module cannot be read.
[[nodiscard]] inline std::vector<std::string> daemonGuestModuleProblems(std::span<std::byte const> module)
{
    auto const sections = wasm::binary::sections(module);
    if (!sections)
        return { "not a readable WebAssembly module" };

    auto problems = std::vector<std::string> {};
    auto manifestSections = std::size_t { 0 };
    for (auto const& section: *sections)
    {
        auto checked = std::expected<void, wasm::binary::BinaryError> {};
        switch (section.id)
        {
            case wasm::binary::CustomSectionId: {
                auto reader = detail::Reader { section.content };
                auto const name = reader.name();
                if (!name)
                    checked = std::unexpected(name.error());
                else if (detail::text(*name) == ManifestSectionName)
                    ++manifestSections;
                break;
            }
            case detail::ImportSectionId: checked = detail::checkImports(section.content, problems); break;
            case wasm::binary::ExportSectionId:
                checked = detail::checkExports(section.content, problems);
                break;
            case wasm::binary::StartSectionId: problems.emplace_back("a start section"); break;
            default: break;
        }
        if (!checked)
            problems.push_back(std::format("a section at byte {} that cannot be read", section.offset));
    }
    if (manifestSections != 1)
        problems.push_back(std::format("{} manifest sections, not exactly one", manifestSections));
    return problems;
}

} // namespace vtextension::testing
