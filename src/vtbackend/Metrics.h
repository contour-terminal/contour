// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <map>
#include <string>

namespace vtbackend
{

/// Used for collecting VT sequence usage metrics.
struct Metrics
{
    // XXX Too bad the key is a string.
    std::map<std::string, uint64_t> sequences {};

    void operator()(Sequence const& seq) { sequences[seq.text()]++; }

    /// @returns an ordered list of collected metrics, with highest frequencey first.
    [[nodiscard]] std::vector<std::pair<std::string, uint64_t>> ordered() const
    {
        std::vector<std::pair<std::string, uint64_t>> vec;
        vec.reserve(sequences.size());
        for (auto const& [name, freq]: sequences)
            vec.emplace_back(name, freq);

        std::sort(vec.begin(), vec.end(), [](auto const& a, auto const& b) {
            if (a.second > b.second)
                return true;
            if (a.second == b.second)
                return a.first > b.first;
            return false;
        });
        return vec;
    }
};

} // namespace vtbackend
