/**
 * This file is part of the "contour" project.
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <exception>
#include <string_view>

#include <fmt/format.h>

namespace crispy
{

// XXX Too bad gsl_assert.h is imported SOMEWHERE THAT IS NOT ME and hence
//     I cannot *just* define Expects() nor Ensures().

#if defined(Expects)
#undef Expects
#endif

#if defined(Ensure)
#undef Ensure
#endif

inline void check(bool _cond, std::string_view _text,
                  std::string_view _message,
                  std::string_view _file, int _line)
{
    if (_cond)
        return;

    fmt::print("[{}:{}] {} {}\n", _file, _line, _message, _text);
    std::terminate();
}

#define Expects(cond) \
    do { \
        ::crispy::check( \
            (cond), #cond, \
            "Precondition failed.", \
            __FILE__, __LINE__ \
        ); \
    } while (0)

#define Ensure(cond) \
    do { \
        ::crispy::check( \
            (cond), #cond, \
            "Postcondition failed.", \
            __FILE__, __LINE__ \
        ); \
    } while (0)

}
