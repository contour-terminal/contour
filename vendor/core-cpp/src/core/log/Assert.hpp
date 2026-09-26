// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The contract checks that report through core::log: fatal(), which logs and ends the process,
/// and SoftRequire(), which logs and lets the caller recover.
///
/// They were part of crispy's Assert.hpp. They live here because they log, and core::base, where
/// Require() and Guarantee() are (`<core/Assert.hpp>`), may not depend on core::log.

#include <core/log/LogStore.hpp>

#include <cassert>
#include <cstdlib>
#include <string_view>

namespace core::log
{

/// Logs @p message to the `fatal` category, which is always enabled, and ends the process.
/// @param message  What went wrong; may be empty.
/// @param location Where; defaults to the caller.
[[noreturn]] inline void fatal(std::string_view message, SourceLocation location = SourceLocation::current())
{
    auto static fatalLog = Category("fatal", "Fatal error Logger", Category::State::Enabled);

    if (!message.empty())
        fatalLog(location)("Fatal error. {}", message);
    else
        fatalLog(location)("Fatal error.");
    std::abort();
}

namespace detail
{
    /// Checks a condition. On failure: logs to errorLog, asserts (debug abort), returns false.
    /// In release builds (NDEBUG), assert compiles out — logs error and returns false.
    [[nodiscard]] inline bool softRequire(bool condition,
                                          char const* conditionText,
                                          SourceLocation location = SourceLocation::current()) noexcept
    {
        if (condition) [[likely]]
            return true;
        // NB: Using a reference to bypass the errorLog() macro and pass a custom source_location.
        auto const& softRequireErrorCategory = ::core::log::errorLog;
        softRequireErrorCategory(location)("Precondition failed: {}", conditionText);
        assert(false && "SoftRequire failed (debug-only abort)");
        return false;
    }
} // namespace detail

} // namespace core::log

/// Soft precondition check. Logs and asserts (debug-only abort) on failure, returns bool.
/// Usage: if (!SoftRequire(ptr != nullptr)) return fallback;
#define SoftRequire(cond) core::log::detail::softRequire(!!(cond), #cond)
