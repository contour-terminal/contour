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

#include <crispy/logstore.h>

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
    inline fail_handler_t& fail_handler() noexcept
    {
        static fail_handler_t storage {};
        return storage;
    }

    [[noreturn]] inline void fail(std::string_view text,
                                  std::string_view message,
                                  std::string_view file,
                                  int line) noexcept // NOLINT(bugprone-exception-escape)
    {
        if (fail_handler())
            fail_handler()(text, message, file, line);
        else
            fmt::print("[{}:{}] {} {}\n", file, line, message, text);
        std::abort();
    }
} // namespace detail

/// Sets a custom fail handler to be invoked when Expects() or Ensures() fails.
///
/// This handler is supposed to report and terminate but may very well
/// just ignore either or both.
inline void set_fail_handler(fail_handler_t handler)
{
    detail::fail_handler() = std::move(handler);
}

/// This method prints an error message and then terminates the program.
[[noreturn]] inline void todo(std::string_view message = {})
{
    fmt::print("TODO: We have reached some code that is missing an implementation.\n");
    if (!message.empty())
        fmt::print("{}\n", message);
    std::abort();
}

[[noreturn]] inline void fatal(std::string_view message,
                               logstore::source_location location = logstore::source_location::current())
{
    auto static FatalLog =
        logstore::Category("fatal", "Fatal error Logger", logstore::Category::State::Enabled);

    if (!message.empty())
        FatalLog(location)("Fatal error. {}", message);
    else
        FatalLog(location)("Fatal error.");
    std::abort();
}

#define Require(cond)                                                                \
    do                                                                               \
    {                                                                                \
        if (!(cond))                                                                 \
        {                                                                            \
            crispy::detail::fail(#cond, "Precondition failed.", __FILE__, __LINE__); \
        }                                                                            \
    } while (0)

#define Guarantee(cond)                                                               \
    do                                                                                \
    {                                                                                 \
        if (!(cond))                                                                  \
        {                                                                             \
            crispy::detail::fail(#cond, "Postcondition failed.", __FILE__, __LINE__); \
        }                                                                             \
    } while (0)

} // namespace crispy
