// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>
#include <vtbackend/Functions.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
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

namespace vtbackend
{

FunctionDefinition const* select(FunctionSelector const& selector,
                                 gsl::span<FunctionDefinition const> availableDefinitions) noexcept
{
    auto a = size_t { 0 };
    auto b = availableDefinitions.size() - 1;
    while (a <= b)
    {
        auto const i = (a + b) / 2;
        auto const& fui = availableDefinitions[i];
        auto const rel = compare(selector, fui);
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
            return &fui;
    }
    return nullptr;
}

} // namespace vtbackend
