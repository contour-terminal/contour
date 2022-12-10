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
#include <vtbackend/VTType.h>

#include <array>
#include <utility>

using namespace std;

namespace terminal
{

string to_string(DeviceAttributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ",";
        s += v;
    };

    auto constexpr mappings = array<pair<DeviceAttributes, string_view>, 11> {
        pair { DeviceAttributes::AnsiColor, "AnsiColor" },
        pair { DeviceAttributes::AnsiTextLocator, "AnsiTextLocator" },
        pair { DeviceAttributes::Columns132, "Columns132" },
        pair { DeviceAttributes::NationalReplacementCharacterSets, "NationalReplacementCharacterSets" },
        pair { DeviceAttributes::Printer, "Printer" },
        pair { DeviceAttributes::RectangularEditing, "RectangularEditing" },
        pair { DeviceAttributes::SelectiveErase, "SelectiveErase" },
        pair { DeviceAttributes::SixelGraphics, "SixelGraphics" },
        pair { DeviceAttributes::TechnicalCharacters, "TechnicalCharacters" },
        pair { DeviceAttributes::UserDefinedKeys, "UserDefinedKeys" },
        pair { DeviceAttributes::Windowing, "Windowing" },
    };

    for (auto const& mapping: mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

string to_params(DeviceAttributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ";";
        s += v;
    };

    auto constexpr mappings = array<pair<DeviceAttributes, string_view>, 12> {
        pair { DeviceAttributes::AnsiColor, "22" },
        pair { DeviceAttributes::AnsiTextLocator, "29" },
        pair { DeviceAttributes::CaptureScreenBuffer, "314" },
        pair { DeviceAttributes::Columns132, "1" },
        pair { DeviceAttributes::NationalReplacementCharacterSets, "9" },
        pair { DeviceAttributes::Printer, "2" },
        pair { DeviceAttributes::RectangularEditing, "28" },
        pair { DeviceAttributes::SelectiveErase, "6" },
        pair { DeviceAttributes::SixelGraphics, "4" },
        pair { DeviceAttributes::TechnicalCharacters, "15" },
        pair { DeviceAttributes::UserDefinedKeys, "8" },
        pair { DeviceAttributes::Windowing, "18" },
    };

    for (auto const& mapping: mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

} // namespace terminal
