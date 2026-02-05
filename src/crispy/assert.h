// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/logstore.h>

#include <exception>
#include <format>
#include <functional>
#include <iostream>
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
                                  int line) noexcept
    {
        try
        {
            if (fail_handler())
                fail_handler()(text, message, file, line);
            else
                std::cerr << std::format("[{}:{}] {} {}\n", file, line, message, text);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
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
    std::cerr << std::format("TODO: We have reached some code that is missing an implementation.\n");
    if (!message.empty())
        std::cerr << std::format("{}\n", message);
    std::abort();
}

[[noreturn]] inline void fatal(std::string_view message,
                               logstore::source_location location = logstore::source_location::current())
{
    auto static fatalLog =
        logstore::category("fatal", "Fatal error Logger", logstore::category::state::Enabled);

    if (!message.empty())
        fatalLog(location)("Fatal error. {}", message);
    else
        fatalLog(location)("Fatal error.");
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

namespace detail
{
    /// Checks a condition. On failure: logs to errorLog, asserts (debug abort), returns false.
    /// In release builds (NDEBUG), assert compiles out — logs error and returns false.
    [[nodiscard]] inline bool softRequire(
        bool condition,
        char const* conditionText,
        logstore::source_location location = logstore::source_location::current()) noexcept
    {
        if (condition) [[likely]]
            return true;
        // NB: Using a reference to bypass the errorLog() macro and pass a custom source_location.
        auto const& softRequireErrorCategory = ::logstore::errorLog;
        softRequireErrorCategory(location)("Precondition failed: {}", conditionText);
        assert(false && "SoftRequire failed (debug-only abort)");
        return false;
    }
} // namespace detail

/// Soft precondition check. Logs and asserts (debug-only abort) on failure, returns bool.
/// Usage: if (!SoftRequire(ptr != nullptr)) return fallback;
#define SoftRequire(cond) crispy::detail::softRequire(!!(cond), #cond)

} // namespace crispy
