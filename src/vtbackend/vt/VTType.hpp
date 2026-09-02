// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/Assert.hpp>

#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace vtbackend
{

/**
 * Virtual Terminal Types.
 *
 * The integer representation must match the terminalID response encoding.
 *
 * The integer representational values match the one for DA2's first response parameter.
 */
enum class VTType : uint8_t
{
    VT100 = 0,
    VT220 = 1,
    VT240 = 2,
    VT330 = 18,
    VT340 = 19,
    VT320 = 24,
    VT420 = 41,
    VT510 = 61,
    VT520 = 64,
    VT525 = 65,
};

/// One row of the @ref VTTypes table: a @ref VTType and its human-readable name.
///
/// The wire/DA2 number is not a separate field because for this enum it IS the enumerator's value
/// (see the note above) — unlike @ref DECModeNumbering, where the two are genuinely independent.
struct VTTypeInfo
{
    VTType type;           ///< The terminal type.
    std::string_view name; ///< Its canonical spelling, as configuration and logs use it.
};

/// Every @ref VTType, in ascending numeric order. Read by @ref fromVTTypeNum and
/// @ref nameOfVTType, so adding a terminal type is one new row.
constexpr inline auto VTTypes = std::to_array<VTTypeInfo>({
    { .type = VTType::VT100, .name = "VT100" },
    { .type = VTType::VT220, .name = "VT220" },
    { .type = VTType::VT240, .name = "VT240" },
    { .type = VTType::VT330, .name = "VT330" },
    { .type = VTType::VT340, .name = "VT340" },
    { .type = VTType::VT320, .name = "VT320" },
    { .type = VTType::VT420, .name = "VT420" },
    { .type = VTType::VT510, .name = "VT510" },
    { .type = VTType::VT520, .name = "VT520" },
    { .type = VTType::VT525, .name = "VT525" },
});

/// The @ref VTType a numeric encoding denotes.
///
/// Needed wherever a VTType arrives as a plain number from outside the program — a configuration
/// file, the native protocol — because the numbering is sparse AND not monotonic in capability
/// (`VT330 = 18` precedes `VT320 = 24`), so neither a cast nor a range check is correct.
/// @param typeNum The numeric encoding (matching the terminalID / DA2 response).
/// @return The type, or std::nullopt when no VTType uses @p typeNum.
[[nodiscard]] constexpr std::optional<VTType> fromVTTypeNum(unsigned typeNum) noexcept
{
    for (auto const& row: VTTypes)
        if (std::cmp_equal(std::to_underlying(row.type), typeNum))
            return row.type;
    return std::nullopt;
}

/// The canonical name of @p type ("VT525"), as the formatter and logs spell it.
/// @param type The terminal type.
/// @return Its name, or an empty view for a value outside the enumeration.
[[nodiscard]] constexpr std::string_view nameOfVTType(VTType type) noexcept
{
    for (auto const& row: VTTypes)
        if (row.type == type)
            return row.name;
    return {};
}

enum class VTExtension : uint8_t
{
    None,
    Unknown,
    XTerm,
    Contour,
};

/**
 * Defines a set of feature flags a virtual terminal can support.
 *
 * Used in response to SendDeviceAttributes.
 */
enum class DeviceAttributes : uint32_t
{
    Columns132 = (1 << 0),
    Printer = (1 << 1),
    SelectiveErase = (1 << 2),
    UserDefinedKeys = (1 << 3),
    NationalReplacementCharacterSets = (1 << 4),
    TechnicalCharacters = (1 << 5),
    AnsiColor = (1 << 6),
    AnsiTextLocator = (1 << 7),
    SixelGraphics = (1 << 8),
    RectangularEditing = (1 << 9),
    Windowing = (1 << 10),
    CaptureScreenBuffer = (1 << 11),
    ClipboardExtension = (1 << 12),
    GoodImageProtocol = (1 << 13),
    StatusDisplay = (1 << 14),
    HorizontalScrolling = (1 << 15),
    TextMacros = (1 << 16),
    SoftCharacterSet = (1 << 17),
    RegisGraphics = (1 << 18),
};

constexpr DeviceAttributes operator|(DeviceAttributes a, DeviceAttributes b)
{
    return static_cast<DeviceAttributes>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool operator&(DeviceAttributes a, DeviceAttributes b)
{
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

/// Returns the architectural conformance level (1-5) for a given VTType.
constexpr int conformanceLevelOf(VTType vt) noexcept
{
    switch (vt)
    {
        case VTType::VT100: return 1;
        case VTType::VT220:
        case VTType::VT240: return 2;
        case VTType::VT320:
        case VTType::VT330:
        case VTType::VT340: return 3;
        case VTType::VT420: return 4;
        case VTType::VT510:
        case VTType::VT520:
        case VTType::VT525: return 5;
    }
    return 1;
}

/// Filters out DA1 extensions that are required at the given operating level.
/// Required extensions are implied by the conformance level and should not be listed.
/// Based on the DEC VSRM registered extensions table.
DeviceAttributes filterRequiredExtensions(DeviceAttributes attrs, VTType operatingLevel) noexcept;

//! Generates human readable string of comma separated list of attribute names.
std::string toString(DeviceAttributes v);

//! Generates a parameter list that can be used to generate the CSI response.
std::string toParams(DeviceAttributes v);

} // namespace vtbackend

// {{{ fmt support
template <>
struct std::formatter<vtbackend::VTType>: std::formatter<std::string_view>
{
    auto format(vtbackend::VTType const id, auto& ctx) const
    {
        // Reads the same VTTypes table fromVTTypeNum does, so a new terminal type cannot be
        // rendered by one and unknown to the other.
        return formatter<string_view>::format(vtbackend::nameOfVTType(id), ctx);
    }
};
template <>
struct std::formatter<vtbackend::VTExtension>: std::formatter<std::string_view>
{
    auto format(vtbackend::VTExtension const id, auto& ctx) const
    {
        string_view name;
        switch (id)
        {
            case vtbackend::VTExtension::None: name = "none"; break;
            case vtbackend::VTExtension::Unknown: name = "unknown"; break;
            case vtbackend::VTExtension::XTerm: name = "XTerm"; break;
            case vtbackend::VTExtension::Contour: name = "Contour"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
