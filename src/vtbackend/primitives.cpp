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
