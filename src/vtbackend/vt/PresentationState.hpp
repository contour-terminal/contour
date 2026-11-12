// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CellFlags.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/vt/Charset.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vtbackend
{

/// The two presentation state reports a host may hand back with DECRSPS (`DCS Ps $ t ... ST`), named by
/// the Ps that DECRQPSR asked for them with.
enum class PresentationStateReport : uint8_t
{
    CursorInformation = 1, ///< DECCIR: the cursor's position, rendition and character sets.
    TabStops = 2,          ///< DECTABSR: the horizontal tab stops.
};

/// The SGR renditions DECCIR's Srend field carries, one bit each above its 0x40 base. A table, so the
/// report and its restore read the same bits; the VT520's invisible bit (0x10) is not among them, for
/// Contour's report does not send it.
inline constexpr auto CursorInformationRenditions = std::array {
    std::pair { uint8_t { 0x01 }, CellFlag::Bold },
    std::pair { uint8_t { 0x02 }, CellFlag::Underline },
    std::pair { uint8_t { 0x04 }, CellFlag::Blinking },
    std::pair { uint8_t { 0x08 }, CellFlag::Inverse },
};

/// The bits of DECCIR's Sflag field, above its 0x40 base.
enum class CursorInformationFlag : uint8_t
{
    OriginMode = 0x01,   ///< DECOM is set.
    SingleShift2 = 0x02, ///< SS2 maps the next graphic character through G2.
    SingleShift3 = 0x04, ///< SS3 maps the next graphic character through G3.
    WrapPending = 0x08,  ///< The cursor stands past the right margin, wrapping at the next character.
};

/// A DECCIR payload, decoded but not yet checked against a page: what a terminal reported of its cursor.
/// Its bit fields are kept as the wire carries them, isProtected included: a decoded wire format.
struct CursorInformation
{
    int line = 1;                       ///< Pr: the cursor's line on the page, 1-based.
    int column = 1;                     ///< Pc: the cursor's column, 1-based.
    int page = 1;                       ///< Pp: the page the cursor is on, 1-based.
    uint8_t rendition = 0;              ///< Srend without its 0x40 base. @see CursorInformationRenditions
    bool isProtected = false;           ///< Satt: DECSCA protects what is written next.
    uint8_t flags = 0;                  ///< Sflag without its 0x40 base. @see CursorInformationFlag
    CharsetTable gl = CharsetTable::G0; ///< Pgl: the G-set invoked into GL.
    CharsetTable gr = CharsetTable::G2; ///< Pgr: the G-set invoked into GR.
    uint8_t charsetSizes = 0; ///< Scss without its 0x40 base: bit g set when G(g) holds a 96-character set.
    std::array<std::string, 4>
        designations {}; ///< Sdesig: each G-set's SCS designator, intermediates included.

    /// @param flag A bit of Sflag.
    /// @return true if the report sets @p flag.
    [[nodiscard]] constexpr bool has(CursorInformationFlag flag) const noexcept
    {
        return (flags & std::to_underlying(flag)) != 0;
    }

    /// @param table A G-set.
    /// @return true if the report says @p table holds a 96-character set.
    [[nodiscard]] constexpr bool is96Charset(CharsetTable table) const noexcept
    {
        return (charsetSizes & (1U << std::to_underlying(table))) != 0;
    }

    [[nodiscard]] bool operator==(CursorInformation const&) const = default;
};

/// Why a DECRSPS payload was not accepted.
enum class PresentationStateRejection : uint8_t
{
    MissingField,     ///< The payload ends before every field was given.
    MalformedNumber,  ///< A numeric field is empty, holds a non-digit, or is out of its range.
    MalformedFlags,   ///< A one-character bit field lies outside the range its base allows.
    MalformedCharset, ///< An SCS designator lacks its final byte, or something follows the fourth.
};

/// @param rejection A rejection reason.
/// @return Its spelling, for logs.
[[nodiscard]] std::string_view toString(PresentationStateRejection rejection) noexcept;

/// Decodes the payload of a DECCIR report, as DECRSPS 1 hands it back:
/// `Pr;Pc;Pp;Srend;Satt;Sflag;Pgl;Pgr;Scss;Sdesig`.
/// @param payload The DCS payload, after `1$t`.
/// @return The report's fields, or why they cannot be read.
[[nodiscard]] std::expected<CursorInformation, PresentationStateRejection> decodeCursorInformation(
    std::string_view payload);

/// Decodes the payload of a DECTABSR report, as DECRSPS 2 hands it back: the 1-based columns of the
/// stops, separated by `/`. An empty payload names no stops.
/// @param payload The DCS payload, after `2$t`.
/// @return The stops as columns, ascending and without duplicates, or why they cannot be read.
[[nodiscard]] std::expected<std::vector<ColumnOffset>, PresentationStateRejection> decodeTabStops(
    std::string_view payload);

/// Finds the character set an SCS designator names, as DECCIR reports it.
/// @param designator The designator: its intermediates, if any, then its final byte.
/// @param is96 Whether it designates a 96-character set: Latin-1's `A` is British's as a 94-set.
/// @return The character set, or nothing for one Contour has no map for (and for `<`, which names UPSS).
[[nodiscard]] std::optional<CharsetId> charsetOfDesignator(std::string_view designator, bool is96) noexcept;

} // namespace vtbackend
