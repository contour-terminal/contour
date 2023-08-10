/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <crispy/assert.h>

#include <fmt/format.h>

#include <string>

namespace terminal
{

/**
 * Virtual Terminal Types.
 *
 * The integer representation must match the terminalID response encoding.
 *
 * The integer representational values match the one for DA2's first response parameter.
 */
enum class vt_type
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

enum class vt_extension
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
enum class device_attributes : uint16_t
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
};

constexpr device_attributes operator|(device_attributes a, device_attributes b)
{
    return static_cast<device_attributes>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool operator&(device_attributes a, device_attributes b)
{
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

//! Generates human readable string of comma seperated list of attribute names.
std::string to_string(device_attributes v);

//! Generates a parameter list that can be used to generate the CSI response.
std::string to_params(device_attributes v);

} // namespace terminal

// {{{ fmtlib support
template <>
struct fmt::formatter<terminal::vt_type>: fmt::formatter<std::string_view>
{
    auto format(const terminal::vt_type id, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (id)
        {
            case terminal::vt_type::VT100: name = "VT100"; break;
            case terminal::vt_type::VT220: name = "VT220"; break;
            case terminal::vt_type::VT240: name = "VT240"; break;
            case terminal::vt_type::VT320: name = "VT320"; break;
            case terminal::vt_type::VT330: name = "VT330"; break;
            case terminal::vt_type::VT340: name = "VT340"; break;
            case terminal::vt_type::VT420: name = "VT420"; break;
            case terminal::vt_type::VT510: name = "VT510"; break;
            case terminal::vt_type::VT520: name = "VT520"; break;
            case terminal::vt_type::VT525: name = "VT525"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
template <>
struct fmt::formatter<terminal::vt_extension>: fmt::formatter<std::string_view>
{
    auto format(const terminal::vt_extension id, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (id)
        {
            case terminal::vt_extension::None: name = "none"; break;
            case terminal::vt_extension::Unknown: name = "unknown"; break;
            case terminal::vt_extension::XTerm: name = "XTerm"; break;
            case terminal::vt_extension::Contour: name = "Contour"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};
// }}}
