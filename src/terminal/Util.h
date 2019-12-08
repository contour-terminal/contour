/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <numeric>
#include <string>

namespace terminal {

std::string escape(char32_t ch);

template <typename T>
inline std::string escape(T begin, T end)
{
    return std::accumulate(begin, end, std::string{}, [](auto const& a, auto ch) { return a + escape(ch); });
    // auto result = std::string{};
    // for (T cur = begin; cur != end; ++cur)
    //     result += *cur;
    // return result;
}

inline std::string escape(std::string const& s)
{
    return escape(begin(s), end(s));
}

}  // namespace terminal
