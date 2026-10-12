// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/Functions.hpp>

#include <vtbackend/core/Color.hpp>

#include <crispy/Sort.hpp>

#include <core/Escape.hpp>
#include <core/Times.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <numeric>
#include <sstream>
#include <string>

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
            auto const* result = &fui;
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

Function const* SupportedSequences::select(FunctionSelector const& selector) const noexcept
{
    auto const finalByte = static_cast<uint8_t>(selector.finalSymbol);
    if (finalByte >= FinalSymbolCount || static_cast<size_t>(selector.category) >= CategoryCount)
        return nullptr;

    auto const& bucket = _buckets[bucketIndex(selector.category, finalByte)];

    // An empty bucket answers on its own -- and keeps select() off an empty span, which it does not handle.
    if (bucket.count == 0)
        return nullptr;

    // The bucket is a slice of the sorted active table, so precedence inside it resolves exactly as it
    // would over the whole table.
    return vtbackend::select(selector, activeSequences().subspan(bucket.first, bucket.count));
}

} // namespace vtbackend
