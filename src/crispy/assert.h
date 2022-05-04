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

#include <fmt/format.h>

#include <exception>
#include <functional>
#include <string_view>

namespace crispy
{

// XXX Too bad gsl_assert.h is imported SOMEWHERE THAT IS NOT ME and hence
//     I cannot *just* define Expects() nor Ensures().
// XXX Also (rant!), Expects() and Ensures() is hard-defined (WTF!!!) in GSL.
//     Microsoft, please fix this!
//
// So instead we use:
//   Require()
//   Guarantee()

/// Function signature for custom assertion failure handlers.
using fail_handler_t = std::function<void(std::string_view, std::string_view, std::string_view, int)>;

#if defined(__GNUC__)
// GCC 4.8+, Clang, Intel and other compilers compatible with GCC (-std=c++0x or above)
[[noreturn]] inline __attribute__((always_inline)) void unreachable()
{
    __builtin_unreachable();
}

#elif defined(_MSC_VER) // MSVC

[[noreturn]] __forceinline void unreachable()
{
    __assume(false);
}

#else

[[noreturn]] inline void unreachable()
{
}

#endif

namespace detail
{
    inline fail_handler_t& fail_handler()
    {
        static fail_handler_t storage {};
        return storage;
    }

    [[noreturn]] inline void fail(std::string_view _text,
                                  std::string_view _message,
                                  std::string_view _file,
                                  int _line)
    {
        if (fail_handler())
            fail_handler()(_text, _message, _file, _line);
        else
            fmt::print("[{}:{}] {} {}\n", _file, _line, _message, _text);
        std::abort();
    }

    inline void check(
        bool _cond, std::string_view _text, std::string_view _message, std::string_view _file, int _line)
    {
        if (!_cond)
        {
            fail(_text, _message, _file, _line);
        }
    }
} // namespace detail

/// Sets a custom fail handler to be invoked when Expects() or Ensures() fails.
///
/// This handler is supposed to report and terminate but may very well
/// just ignore either or both.
inline void set_fail_handler(fail_handler_t _handler)
{
    detail::fail_handler() = std::move(_handler);
}

#define Require(cond)                                                                       \
    do                                                                                      \
    {                                                                                       \
        ::crispy::detail::check((cond), #cond, "Precondition failed.", __FILE__, __LINE__); \
    } while (0)

#define Guarantee(cond)                                                                      \
    do                                                                                       \
    {                                                                                        \
        ::crispy::detail::check((cond), #cond, "Postcondition failed.", __FILE__, __LINE__); \
    } while (0)

} // namespace crispy
