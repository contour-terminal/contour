#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <ostream>

namespace crispy::text
{

enum class FontWeight
{
    Normal,
    Bold,
};

enum class FontSlant
{
    Normal,
    Italic,
};

struct FontDescription
{
    std::string path;
    std::string postscriptName;
    std::string familyName;
    std::string styleName;
    FontWeight weight;
    FontSlant slant;
    int width;
    bool monospace;
};

struct FontPattern
{
    std::string family;
    FontWeight weight = FontWeight::Normal;
    FontSlant slant = FontSlant::Normal;
    bool monospace = false;
};

std::vector<FontDescription> findFonts(FontPattern const& _pattern);

}
