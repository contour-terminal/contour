// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/primitives.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <stdexcept>

using std::invalid_argument;
using std::string;
using std::transform;

namespace terminal
{

CursorShape makeCursorShape(string const& name)
{
    auto const lowerName = [](auto const& input) -> std::string {
        string output;
        transform(begin(input), end(input), back_inserter(output), [](auto ch) { return tolower(ch); });
        return output;
    }(name);

    if (lowerName == "block")
        return CursorShape::Block;
    else if (lowerName == "rectangle")
        return CursorShape::Rectangle;
    else if (lowerName == "underscore")
        return CursorShape::Underscore;
    else if (lowerName == "bar")
        return CursorShape::Bar;
    else
        throw invalid_argument { "Invalid cursor shape: " + name };
}

} // namespace terminal
