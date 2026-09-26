// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Contract checks that end the process: Require() for a precondition, Guarantee() for a
/// postcondition, and todo() and unreachable().
///
/// The checks are named Require and Guarantee rather than Expects and Ensures, which the GSL
/// defines as macros of its own. The checks that report through core::log, fatal() and
/// SoftRequire(), are in `<core/log/Assert.hpp>`.

#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <string_view>
#include <utility>

namespace core
{

/// Function signature for custom assertion failure handlers.
using FailHandler = std::function<void(std::string_view, std::string_view, std::string_view, int)>;

/// Tells the compiler that this point is never reached; reaching it is undefined behaviour.
[[noreturn]] inline void unreachable()
{
    std::unreachable();
}

namespace detail
{
    inline FailHandler& failHandler() noexcept
    {
        static FailHandler storage {};
        return storage;
    }

    [[noreturn]] inline void fail(std::string_view text,
                                  std::string_view message,
                                  std::string_view file,
                                  int line) noexcept
    {
        try
        {
            if (failHandler())
                failHandler()(text, message, file, line);
            else
                std::cerr << std::format("[{}:{}] {} {}\n", file, line, message, text);
        }
        catch (...)
        {
            // A handler or a stream that throws ends the process all the same.
            std::abort();
        }
        std::abort();
    }
} // namespace detail

/// Sets a custom fail handler to be invoked when Require() or Guarantee() fails.
///
/// This handler is supposed to report and terminate but may very well
/// just ignore either or both.
inline void setFailHandler(FailHandler handler)
{
    detail::failHandler() = std::move(handler);
}

/// This method prints an error message and then terminates the program.
[[noreturn]] inline void todo(std::string_view message = {})
{
    std::cerr << std::format("TODO: We have reached some code that is missing an implementation.\n");
    if (!message.empty())
        std::cerr << std::format("{}\n", message);
    std::abort();
}

// The condition is reduced to a named bool before being negated. Negating `(cond)` directly
// would put a `!(a && b)` into every expansion, which readability-simplify-boolean-expr then
// reports against the *call site* — a warning the caller cannot act on.
#define Require(cond)                                                              \
    do                                                                             \
    {                                                                              \
        bool const conditionHolds = static_cast<bool>(cond);                       \
        if (!conditionHolds)                                                       \
        {                                                                          \
            core::detail::fail(#cond, "Precondition failed.", __FILE__, __LINE__); \
        }                                                                          \
    } while (0)

#define Guarantee(cond)                                                             \
    do                                                                              \
    {                                                                               \
        bool const conditionHolds = static_cast<bool>(cond);                        \
        if (!conditionHolds)                                                        \
        {                                                                           \
            core::detail::fail(#cond, "Postcondition failed.", __FILE__, __LINE__); \
        }                                                                           \
    } while (0)

} // namespace core
