// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/assert.h>

#include <format>
#include <string>

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

//! Generates human readable string of comma seperated list of attribute names.
std::string to_string(DeviceAttributes v);

//! Generates a parameter list that can be used to generate the CSI response.
std::string to_params(DeviceAttributes v);

} // namespace vtbackend

// {{{ fmtlib support
template <>
struct std::formatter<vtbackend::VTType>: std::formatter<std::string_view>
{
    auto format(const vtbackend::VTType id, auto& ctx) const
    {
        string_view name;
        switch (id)
        {
            case vtbackend::VTType::VT100: name = "VT100"; break;
            case vtbackend::VTType::VT220: name = "VT220"; break;
            case vtbackend::VTType::VT240: name = "VT240"; break;
            case vtbackend::VTType::VT320: name = "VT320"; break;
            case vtbackend::VTType::VT330: name = "VT330"; break;
            case vtbackend::VTType::VT340: name = "VT340"; break;
            case vtbackend::VTType::VT420: name = "VT420"; break;
            case vtbackend::VTType::VT510: name = "VT510"; break;
            case vtbackend::VTType::VT520: name = "VT520"; break;
            case vtbackend::VTType::VT525: name = "VT525"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
template <>
struct std::formatter<vtbackend::VTExtension>: std::formatter<std::string_view>
{
    auto format(const vtbackend::VTExtension id, auto& ctx) const
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
