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
    Malformed,  ///< An over-long LEB128 number.
};

/// The first eight bytes of every core module: `\0asm`, then version 1 as a little-endian u32.
inline constexpr auto ModuleHeader =
    std::to_array<uint8_t>({ 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 });

/// The id of a custom section, the only kind that carries a name.
inline constexpr auto CustomSectionId = uint8_t { 0 };

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

} // namespace vtextension::wasm::binary
