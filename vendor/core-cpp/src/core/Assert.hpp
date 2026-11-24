// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Contract checks that end the process: Require() for a precondition, Guarantee() for a
/// postcondition, and todo() and unreachable().
///
/// The checks are named Require and Guarantee rather than Expects and Ensures, which the GSL
/// defines as macros of its own. The checks that report through core::log, fatal() and
/// SoftRequire(), are in `<core/log/Assert.hpp>`.

#include <core/BaseExport.hpp>

#include <cstdlib>
#include <format>
#include <functional>
#include <iostream>
#include <source_location>
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
    /// The process-wide handler, defined in the library so that every image linking core-cpp
    /// shares one: a handler set in an executable has to reach a check compiled into a DLL.
    /// @return The handler slot; empty when none is set.
    CORE_BASE_API FailHandler& failHandler() noexcept;

    [[noreturn]] inline void fail(std::string_view text,
                                  std::string_view message,
                                  std::string_view file,
                                  int line) noexcept
    {
        // A handler or a stream that throws ends the process all the same: the exception cannot
        // leave this noexcept function, so it reaches std::terminate(). No try block catches it
        // here, which is what lets this header compile with exceptions off (a WebAssembly guest).
        if (failHandler())
            failHandler()(text, message, file, line);
        else
            std::cerr << std::format("[{}:{}] {} {}\n", file, line, message, text);
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

/// @brief Throws @p exception, or, where exceptions are compiled out, fails as Require() does.
///
/// For the few throws in code a WebAssembly guest compiles with `-fno-exceptions`: there a
/// `throw` does not compile, and would have ended the process if it did. So with exceptions on
/// this is exactly `throw exception;`, and with them off it reports the exception's what() through
/// the fail handler and aborts. This is the one place that asks which of the two it is.
///
/// @param exception The exception to throw; a std::exception, for its what().
/// @param where Where the throw is, for the report when exceptions are off.
template <typename Exception>
[[noreturn]] void throwOrFail(Exception&& exception,
                              [[maybe_unused]] std::source_location where = std::source_location::current())
{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    throw std::forward<Exception>(exception);
#else
    detail::fail("exception thrown with exceptions disabled",
                 exception.what(),
                 where.file_name(),
                 static_cast<int>(where.line()));
#endif
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
