// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Color.h>
#include <vtbackend/Functions.h>

#include <crispy/algorithm.h>
#include <crispy/escape.h>
#include <crispy/sort.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <algorithm>
#include <array>
#include <format>
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

Function const* select(FunctionSelector const& selector,
                       gsl::span<Function const> availableDefinitions) noexcept
{
    auto a = size_t { 0 };
    auto b = availableDefinitions.size() - 1;
    while (a <= b)
    {
        auto const i = (a + b) / 2;
        auto const& fui = availableDefinitions[i];
        auto const rel = compare(selector, fui);
        if (rel > 0)
            a = i + 1;
        else if (rel < 0)
        {
            if (i == 0)
                return nullptr;
            b = i - 1;
        }
        else
        {
            // Found a match. Scan left to find the most specific (leftmost) match.
            // Multiple definitions can match the same selector (e.g. DCS with 0 args
            // and the same final byte). The leftmost match
            // in the sorted array has the tightest parameter range.
            auto result = &fui;
            for (auto j = i; j > 0; --j)
            {
                if (compare(selector, availableDefinitions[j - 1]) == 0)
                    result = &availableDefinitions[j - 1];
                else
                    break;
            }
            return result;
        }
    }
    return nullptr;
}

} // namespace vtbackend
