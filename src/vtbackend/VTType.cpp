// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/VTType.h>

#include <array>
#include <utility>

using namespace std;

namespace vtbackend
{

string to_string(DeviceAttributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ",";
        s += v;
    };

    auto constexpr Mappings = array<pair<DeviceAttributes, string_view>, 11> {
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

    for (auto const& mapping: Mappings)
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

    auto constexpr Mappings = array<pair<DeviceAttributes, string_view>, 12> {
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

    for (auto const& mapping: Mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

} // namespace vtbackend
