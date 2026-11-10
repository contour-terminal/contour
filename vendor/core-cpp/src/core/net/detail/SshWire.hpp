// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The SSH wire encoding (RFC 4251 §5) of the few types an OpenSSH public key and an SSHSIG are made
/// of: `byte`, `uint32` and `string`.
///
/// The reader parses attacker-supplied bytes, so every read is bounds-checked against what is left
/// and a short or oversized length answers `std::nullopt` rather than reading past the end. It is
/// shared by `SshKeys.cpp` and the software security key the tests sign with, so both speak exactly
/// one encoding.
///
/// Private to core::net_tls (and its tests): listed under SOURCES, not HEADERS.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace core::net::detail
{

/// Appends @p value to @p out.
/// @param out The buffer to grow.
/// @param value The byte.
inline void appendSshByte(std::vector<std::byte>& out, std::uint8_t value)
{
    out.push_back(std::byte { value });
}

/// Appends @p value to @p out as a big-endian `uint32`.
/// @param out The buffer to grow.
/// @param value The number.
inline void appendSshUint32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (auto const shift: { 24U, 16U, 8U, 0U })
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
}

/// Appends @p bytes to @p out as an SSH `string`: a `uint32` length, then the bytes.
/// @param out The buffer to grow.
/// @param bytes The contents; longer than a `uint32` can count is a programmer error and throws.
inline void appendSshString(std::vector<std::byte>& out, std::span<std::byte const> bytes)
{
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("an SSH string holds at most 2^32-1 bytes");
    appendSshUint32(out, static_cast<std::uint32_t>(bytes.size()));
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// Appends @p text to @p out as an SSH `string`.
/// @param out The buffer to grow.
/// @param text The contents, each `char` as the byte it holds.
inline void appendSshString(std::vector<std::byte>& out, std::string_view text)
{
    appendSshString(out, std::as_bytes(std::span { text.data(), text.size() }));
}

/// Reads SSH wire types off the front of a byte span, never past its end.
class SshWireReader
{
  public:
    /// @param input The bytes to read; must outlive the reader and every span it returns.
    explicit SshWireReader(std::span<std::byte const> input) noexcept: _rest(input) {}

    /// @return The next byte, or @c std::nullopt if none is left.
    [[nodiscard]] std::optional<std::uint8_t> byte() noexcept
    {
        auto const taken = raw(1);
        if (!taken)
            return std::nullopt;
        return std::to_integer<std::uint8_t>((*taken)[0]);
    }

    /// @return The next big-endian `uint32`, or @c std::nullopt if fewer than four bytes are left.
    [[nodiscard]] std::optional<std::uint32_t> uint32() noexcept
    {
        auto const taken = raw(4);
        if (!taken)
            return std::nullopt;
        auto value = std::uint32_t { 0 };
        for (auto const octet: *taken)
            value = (value << 8U) | std::to_integer<std::uint32_t>(octet);
        return value;
    }

    /// @return The contents of the next `string`, or @c std::nullopt if its length field is short
    ///         or counts more bytes than are left. Nothing is consumed on failure of the contents.
    [[nodiscard]] std::optional<std::span<std::byte const>> string() noexcept
    {
        auto const saved = _rest;
        auto const length = uint32();
        if (!length)
            return std::nullopt;
        auto contents = raw(*length);
        if (!contents)
            _rest = saved;
        return contents;
    }

    /// @param count How many bytes to take.
    /// @return The next @p count bytes, or @c std::nullopt (consuming nothing) if fewer are left.
    [[nodiscard]] std::optional<std::span<std::byte const>> raw(std::size_t count) noexcept
    {
        if (count > _rest.size())
            return std::nullopt;
        auto const taken = _rest.first(count);
        _rest = _rest.subspan(count);
        return taken;
    }

    /// @return Whether every byte has been read.
    [[nodiscard]] bool atEnd() const noexcept { return _rest.empty(); }

  private:
    std::span<std::byte const> _rest;
};

} // namespace core::net::detail
