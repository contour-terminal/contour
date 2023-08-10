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

string to_string(device_attributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ",";
        s += v;
    };

    auto constexpr mappings = array<pair<device_attributes, string_view>, 11> {
        pair { device_attributes::AnsiColor, "AnsiColor" },
        pair { device_attributes::AnsiTextLocator, "AnsiTextLocator" },
        pair { device_attributes::Columns132, "Columns132" },
        pair { device_attributes::NationalReplacementCharacterSets, "NationalReplacementCharacterSets" },
        pair { device_attributes::Printer, "Printer" },
        pair { device_attributes::RectangularEditing, "RectangularEditing" },
        pair { device_attributes::SelectiveErase, "SelectiveErase" },
        pair { device_attributes::SixelGraphics, "SixelGraphics" },
        pair { device_attributes::TechnicalCharacters, "TechnicalCharacters" },
        pair { device_attributes::UserDefinedKeys, "UserDefinedKeys" },
        pair { device_attributes::Windowing, "Windowing" },
    };

    for (auto const& mapping: mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

string to_params(device_attributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ";";
        s += v;
    };

    auto constexpr mappings = array<pair<device_attributes, string_view>, 12> {
        pair { device_attributes::AnsiColor, "22" },
        pair { device_attributes::AnsiTextLocator, "29" },
        pair { device_attributes::CaptureScreenBuffer, "314" },
        pair { device_attributes::Columns132, "1" },
        pair { device_attributes::NationalReplacementCharacterSets, "9" },
        pair { device_attributes::Printer, "2" },
        pair { device_attributes::RectangularEditing, "28" },
        pair { device_attributes::SelectiveErase, "6" },
        pair { device_attributes::SixelGraphics, "4" },
        pair { device_attributes::TechnicalCharacters, "15" },
        pair { device_attributes::UserDefinedKeys, "8" },
        pair { device_attributes::Windowing, "18" },
    };

    for (auto const& mapping: mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

} // namespace terminal
