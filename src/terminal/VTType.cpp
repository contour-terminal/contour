#include <terminal/VTType.h>

using namespace std;

namespace terminal {

string to_string(DeviceAttributes v)
{
    string s;

    auto const append = [&s](string_view const& v) {
        if (!s.empty())
            s += ",";
        s += v;
    };

    if (v & DeviceAttributes::Columns132)
        s += "Columns132";
    if (v & DeviceAttributes::Printer)
        append("Printer");
    if (v & DeviceAttributes::SelectiveErase)
        append("SelectiveErase");
    if (v & DeviceAttributes::UserDefinedKeys)
        append("UserDefinedKeys");
    if (v & DeviceAttributes::NationalReplacementCharacterSets)
        append("NationalReplacementCharacterSets");
    if (v & DeviceAttributes::TechnicalCharacters)
        append("TechnicalCharacters");
    if (v & DeviceAttributes::AnsiColor)
        append("AnsiColor");
    if (v & DeviceAttributes::AnsiTextLocator)
        append("AnsiTextLocator");

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

    if (v & DeviceAttributes::Columns132)
        s += "1";
    if (v & DeviceAttributes::Printer)
        append("2");
    if (v & DeviceAttributes::SelectiveErase)
        append("6");
    if (v & DeviceAttributes::UserDefinedKeys)
        append("8");
    if (v & DeviceAttributes::NationalReplacementCharacterSets)
        append("9");
    if (v & DeviceAttributes::TechnicalCharacters)
        append("15");
    if (v & DeviceAttributes::AnsiColor)
        append("22");
    if (v & DeviceAttributes::AnsiTextLocator)
        append("29");

    return s;
}

} // namespace terminal
