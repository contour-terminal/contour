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
#include <vtbackend/Color.h>
#include <vtbackend/Functions.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/indexed.h>
#include <crispy/sort.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <sstream>
#include <string>

using crispy::for_each;
using crispy::times;

using std::accumulate;
using std::array;
using std::for_each;
using std::pair;
using std::sort;
using std::string;
using std::string_view;
using std::stringstream;

namespace terminal
{

function_definition const* select(function_selector const& selector) noexcept
{
    auto static const& funcs = functions();

    // std::cout << fmt::format("select: {}\n", selector);

    auto a = size_t { 0 };
    auto b = funcs.size() - 1;
    while (a <= b)
    {
        auto const i = (a + b) / 2;
        auto const& I = funcs[i];
        auto const rel = compare(selector, I);
        // std::cout << fmt::format(" - a:{:>2} b:{:>2} i:{} rel:{} I: {}\n", a, b, i, rel < 0 ? '<' : rel > 0
        // ? '>' : '=', I);
        if (rel > 0)
            a = i + 1;
        else if (rel < 0)
        {
            if (i == 0)
                return nullptr;
            b = i - 1;
        }
        else
            return &I;
    }
    return nullptr;
}

} // namespace terminal
