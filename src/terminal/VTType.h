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

#include <string>

namespace terminal {

/**
 * Virtual Terminal Types.
 *
 * The integer representation must match the terminalID response encoding.
 *
 * The integer representational values match the one for DA2's first response parameter.
 */
enum class VTType {
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

/**
 * Defines a set of feature flags a virtual terminal can support.
 *
 * Used in response to SendDeviceAttributes.
 */
enum class DeviceAttributes : uint16_t {
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
};

constexpr DeviceAttributes operator|(DeviceAttributes a, DeviceAttributes b)
{
    return static_cast<DeviceAttributes>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool operator&(DeviceAttributes a, DeviceAttributes b)
{
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

//! Generates human readable string of comma seperated list of attribute names.
std::string to_string(DeviceAttributes v);

//! Generates a parameter list that can be used to generate the CSI response.
std::string to_params(DeviceAttributes v);

}  // namespace terminal
