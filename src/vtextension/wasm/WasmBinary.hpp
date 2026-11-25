// SPDX-License-Identifier: Apache-2.0
//
// The few rules of the WebAssembly binary format the host reads a module's bytes by itself: the header, the
// LEB128 numbers, and the sections. Everything else is the runtime's to validate.
//
// Pure and dependency-free, so that every framing rule is table-testable.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace vtextension::wasm::binary
{

/// Why a module's bytes could not be read.
enum class BinaryError : uint8_t
{
    NotAModule, ///< No WebAssembly core module header.
    Truncated,  ///< Something runs past the end of the bytes.
    Malformed,  ///< An over-long LEB128 number, or a form no engine of the host accepts.
};

/// The first eight bytes of every core module: `\0asm`, then version 1 as a little-endian u32.
inline constexpr auto ModuleHeader =
    std::to_array<uint8_t>({ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });

/// The id of a custom section, the only kind that carries a name.
inline constexpr auto CustomSectionId = uint8_t { 0 };

/// The id of the table section, which declares the tables a module defines.
inline constexpr auto TableSectionId = uint8_t { 4 };

/// The id of the memory section, which declares the memories a module defines.
inline constexpr auto MemorySectionId = uint8_t { 5 };

/// The id of the export section.
inline constexpr auto ExportSectionId = uint8_t { 7 };

/// The id of the start section, which names a function the runtime calls on instantiation.
inline constexpr auto StartSectionId = uint8_t { 8 };

/// The most bytes a LEB128-encoded u32 takes.
inline constexpr auto MaxU32Bytes = std::size_t { 5 };

/// Reads the binary format's primitives off a span of bytes, front to back, and fails rather than reads
/// past its end.
class Reader
{
  public:
    /// @param bytes What is read.
    /// @param base Where @p bytes start in the module, for position().
    explicit Reader(std::span<std::byte const> bytes, std::size_t base = 0) noexcept:
        _bytes { bytes }, _base { base }
    {
    }

    /// @return Whether every byte has been read.
    [[nodiscard]] bool atEnd() const noexcept { return _offset == _bytes.size(); }

    /// @return Where the next byte is, counted from the module's start.
    [[nodiscard]] std::size_t position() const noexcept { return _base + _offset; }

    /// @return The next @p count bytes; `Truncated` if fewer remain.
    [[nodiscard]] std::expected<std::span<std::byte const>, BinaryError> bytes(std::size_t count) noexcept
    {
        if (count > _bytes.size() - _offset)
            return std::unexpected(BinaryError::Truncated);
        auto const taken = _bytes.subspan(_offset, count);
        _offset += count;
        return taken;
    }

    /// @return The next byte; `Truncated` past the end.
    [[nodiscard]] std::expected<uint8_t, BinaryError> byte() noexcept
    {
        return bytes(1).transform(
            [](std::span<std::byte const> taken) { return std::to_integer<uint8_t>(taken.front()); });
    }

    /// @return The next unsigned LEB128 number; `Malformed` if it takes more than five bytes or exceeds
    ///         32 bits, `Truncated` if it runs past the end.
    [[nodiscard]] std::expected<uint32_t, BinaryError> u32() noexcept
    {
        constexpr auto PayloadMask = uint8_t { 0b0111'1111 };
        constexpr auto ContinueBit = uint8_t { 0b1000'0000 };
        // Of the fifth byte, only the low four bits may be set.
        constexpr auto LastByteMask = uint8_t { 0b0000'1111 };
        auto value = uint32_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, MaxU32Bytes))
        {
            auto const next = byte();
            if (!next)
                return std::unexpected(next.error());
            if (index + 1 == MaxU32Bytes && (*next & static_cast<uint8_t>(~LastByteMask)) != 0)
                return std::unexpected(BinaryError::Malformed);
            value |= static_cast<uint32_t>(*next & PayloadMask) << (index * 7);
            if ((*next & ContinueBit) == 0)
                return value;
        }
        return std::unexpected(BinaryError::Malformed);
    }

    /// @return The next name's bytes: a u32 length, then that many bytes.
    [[nodiscard]] std::expected<std::span<std::byte const>, BinaryError> name() noexcept
    {
        return u32().and_then([this](uint32_t length) { return bytes(length); });
    }

    /// @return Everything that is left.
    [[nodiscard]] std::span<std::byte const> rest() noexcept
    {
        auto const taken = _bytes.subspan(_offset);
        _offset = _bytes.size();
        return taken;
    }

  private:
    std::span<std::byte const> _bytes;
    std::size_t _base;
    std::size_t _offset = 0;
};

/// One section of a module.
struct Section
{
    uint8_t id;                         ///< What kind of section it is.
    std::span<std::byte const> content; ///< Its content, after the id and the size.
    std::size_t offset;                 ///< Where the content starts, counted from the module's start.
};

/// Splits a core module into its sections, in order, without looking inside any.
/// @param module A WebAssembly core module's bytes.
/// @return The sections; why the module could not be split.
[[nodiscard]] inline std::expected<std::vector<Section>, BinaryError> sections(
    std::span<std::byte const> module)
{
    auto reader = Reader { module };
    auto const header = reader.bytes(ModuleHeader.size());
    if (!header || !std::ranges::equal(*header, ModuleHeader, {}, {}, [](uint8_t value) {
            return std::byte { value };
        }))
        return std::unexpected(BinaryError::NotAModule);

    auto result = std::vector<Section> {};
    while (!reader.atEnd())
    {
        auto const id = reader.byte();
        if (!id)
            return std::unexpected(id.error());
        auto const size = reader.u32();
        if (!size)
            return std::unexpected(size.error());
        auto const offset = reader.position();
        auto const content = reader.bytes(*size);
        if (!content)
            return std::unexpected(content.error());
        result.push_back(Section { .id = *id, .content = *content, .offset = offset });
    }
    return result;
}

/// The largest initial size of the memories a module defines, whether it exports them or not.
/// @param module A WebAssembly core module's bytes.
/// @return The largest initial size, in 64 KiB pages; 0 if the module defines no memory. `Malformed` for a
///         memory whose limits are not those of a 32-bit memory with the default page size: no engine of
///         the host accepts another (EngineFeatures.hpp), so the host need not read one.
[[nodiscard]] inline std::expected<uint32_t, BinaryError> largestInitialMemoryPages(
    std::span<std::byte const> module)
{
    // Of a memory's limits flags, only these two: whether a maximum follows, and whether it is shared.
    constexpr auto HasMaximum = uint8_t { 0b0000'0001 };
    constexpr auto KnownFlags = uint8_t { 0b0000'0011 };
    return sections(module).and_then(
        [](std::vector<Section> const& all) -> std::expected<uint32_t, BinaryError> {
            auto largest = uint32_t { 0 };
            for (auto const& section:
                 all | std::views::filter([](Section const& entry) { return entry.id == MemorySectionId; }))
            {
                auto reader = Reader { section.content, section.offset };
                auto const count = reader.u32();
                if (!count)
                    return std::unexpected(count.error());
                for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, *count))
                {
                    auto const flags = reader.byte();
                    if (!flags)
                        return std::unexpected(flags.error());
                    if ((*flags & static_cast<uint8_t>(~KnownFlags)) != 0)
                        return std::unexpected(BinaryError::Malformed);
                    auto const initial = reader.u32();
                    if (!initial)
                        return std::unexpected(initial.error());
                    if ((*flags & HasMaximum) != 0)
                    {
                        if (auto const maximum = reader.u32(); !maximum)
                            return std::unexpected(maximum.error());
                    }
                    largest = std::max(largest, *initial);
                }
            }
            return largest;
        });
}

/// One table a module defines, as its table section declares it.
struct TableDeclaration
{
    uint8_t elementType = 0;         ///< Its reference type: `funcref` (0x70) or `externref` (0x6f).
    uint32_t initialElements = 0;    ///< Its initial size, in elements.
    std::optional<uint32_t> maximum; ///< Its declared maximum size, in elements; nothing if none.

    /// @return Whether both declare the same table.
    bool operator==(TableDeclaration const&) const = default;
};

/// The tables a module defines, in the order it defines them. Imported tables are not among them.
/// @param module A WebAssembly core module's bytes.
/// @return The tables; `Malformed` for a table whose type or limits are of a form only a feature that is
///         Off in EngineFeatures allows (typed references, 64-bit or shared tables, an initializer
///         expression): no engine of the host accepts one, so the host need not read one.
[[nodiscard]] inline std::expected<std::vector<TableDeclaration>, BinaryError> tableDeclarations(
    std::span<std::byte const> module)
{
    // The reference types a table may hold without the function-references proposal.
    constexpr auto FuncRef = uint8_t { 0x70 };
    constexpr auto ExternRef = uint8_t { 0x6f };
    // Of a table's limits flags, only this one: whether a maximum follows.
    constexpr auto HasMaximum = uint8_t { 0b0000'0001 };
    return sections(module).and_then(
        [&](std::vector<Section> const& all) -> std::expected<std::vector<TableDeclaration>, BinaryError> {
            auto result = std::vector<TableDeclaration> {};
            for (auto const& section:
                 all | std::views::filter([](Section const& entry) { return entry.id == TableSectionId; }))
            {
                auto reader = Reader { section.content, section.offset };
                auto const count = reader.u32();
                if (!count)
                    return std::unexpected(count.error());
                for ([[maybe_unused]] auto const index: std::views::iota(uint32_t { 0 }, *count))
                {
                    auto const elementType = reader.byte();
                    if (!elementType)
                        return std::unexpected(elementType.error());
                    if (*elementType != FuncRef && *elementType != ExternRef)
                        return std::unexpected(BinaryError::Malformed);
                    auto const flags = reader.byte();
                    if (!flags)
                        return std::unexpected(flags.error());
                    if ((*flags & static_cast<uint8_t>(~HasMaximum)) != 0)
                        return std::unexpected(BinaryError::Malformed);
                    auto const initial = reader.u32();
                    if (!initial)
                        return std::unexpected(initial.error());
                    auto declaration = TableDeclaration { .elementType = *elementType,
                                                          .initialElements = *initial,
                                                          .maximum = std::nullopt };
                    if ((*flags & HasMaximum) != 0)
                    {
                        auto const maximum = reader.u32();
                        if (!maximum)
                            return std::unexpected(maximum.error());
                        declaration.maximum = *maximum;
                    }
                    result.push_back(declaration);
                }
            }
            return result;
        });
}

/// How many tables a module defines, and how large the largest is to begin with: what an engine must
/// check against its limits before instantiating it.
struct TableShape
{
    uint32_t count = 0;                  ///< How many tables the module defines.
    uint32_t largestInitialElements = 0; ///< The largest initial size among them; 0 if there is none.

    /// @param maxTables The most tables allowed.
    /// @param maxElements The most elements each table may begin with.
    /// @return Whether the tables stay within both.
    [[nodiscard]] constexpr bool fitsWithin(uint32_t maxTables, uint32_t maxElements) const noexcept
    {
        return count <= maxTables && largestInitialElements <= maxElements;
    }

    /// @return Whether both describe the same tables.
    bool operator==(TableShape const&) const = default;
};

/// @return The shape of @p module's tables; why its bytes could not be read (@see tableDeclarations).
[[nodiscard]] inline std::expected<TableShape, BinaryError> tableShape(std::span<std::byte const> module)
{
    return tableDeclarations(module).transform([](std::vector<TableDeclaration> const& tables) {
        auto shape =
            TableShape { .count = static_cast<uint32_t>(tables.size()), .largestInitialElements = 0 };
        for (auto const& table: tables)
            shape.largestInitialElements = std::max(shape.largestInitialElements, table.initialElements);
        return shape;
    });
}

} // namespace vtextension::wasm::binary
