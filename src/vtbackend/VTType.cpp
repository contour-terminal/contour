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

    auto constexpr Mappings = std::array {
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
        pair { DeviceAttributes::CaptureScreenBuffer, "CaptureScreenBuffer" },
        pair { DeviceAttributes::ClipboardExtension, "ClipboardExtension" },
        pair { DeviceAttributes::GoodImageProtocol, "GoodImageProtocol" },
        pair { DeviceAttributes::StatusDisplay, "StatusDisplay" },
        pair { DeviceAttributes::HorizontalScrolling, "HorizontalScrolling" },
        pair { DeviceAttributes::TextMacros, "TextMacros" },
        pair { DeviceAttributes::SoftCharacterSet, "SoftCharacterSet" },
    };

    for (auto const& mapping: Mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

DeviceAttributes filterRequiredExtensions(DeviceAttributes attrs, VTType operatingLevel) noexcept
{
    // DEC VSRM table: each entry maps an extension to the conformance level
    // at which it becomes REQUIRED (and thus should not be listed in DA1).
    // Extensions not in this table are always optional when supported.
    static constexpr auto RequiredAtLevel = std::array {
        pair { DeviceAttributes::StatusDisplay, 3 },       // optional at level 2 only
        pair { DeviceAttributes::RectangularEditing, 4 },  // optional at levels 2-3
        pair { DeviceAttributes::SelectiveErase, 5 },      // optional at levels 2-4
        pair { DeviceAttributes::UserDefinedKeys, 5 },     // optional at levels 2-4
        pair { DeviceAttributes::TechnicalCharacters, 5 }, // optional at levels 2-4
        pair { DeviceAttributes::TextMacros, 5 },          // optional at levels 2-4
    };

    auto const level = conformanceLevelOf(operatingLevel);
    auto result = static_cast<uint32_t>(attrs);
    for (auto const& [attr, reqLevel]: RequiredAtLevel)
        if (level >= reqLevel)
            result &= ~static_cast<uint32_t>(attr);
    return static_cast<DeviceAttributes>(result);
}

string to_params(DeviceAttributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ";";
        s += v;
    };

    auto constexpr Mappings = std::array {
        pair { DeviceAttributes::Columns132, "1" },
        pair { DeviceAttributes::Printer, "2" },
        pair { DeviceAttributes::SixelGraphics, "4" },
        pair { DeviceAttributes::SelectiveErase, "6" },
        pair { DeviceAttributes::SoftCharacterSet, "7" },
        pair { DeviceAttributes::UserDefinedKeys, "8" },
        pair { DeviceAttributes::NationalReplacementCharacterSets, "9" },
        pair { DeviceAttributes::StatusDisplay, "11" },
        pair { DeviceAttributes::TechnicalCharacters, "15" },
        pair { DeviceAttributes::Windowing, "18" },
        pair { DeviceAttributes::HorizontalScrolling, "21" },
        pair { DeviceAttributes::AnsiColor, "22" },
        pair { DeviceAttributes::RectangularEditing, "28" },
        pair { DeviceAttributes::AnsiTextLocator, "29" },
        pair { DeviceAttributes::TextMacros, "32" },
        pair { DeviceAttributes::ClipboardExtension, "52" },
        pair { DeviceAttributes::GoodImageProtocol, "90" },
        pair { DeviceAttributes::CaptureScreenBuffer, "314" },
    };

    for (auto const& mapping: Mappings)
        if (v & mapping.first)
            append(mapping.second);

    return s;
}

} // namespace vtbackend
