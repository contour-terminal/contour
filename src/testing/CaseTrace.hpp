// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A trace of the test cases a Catch2 run starts and ends, written to standard error as it happens, so that a
/// run which hangs names the case it hangs in. A hung suite killed by a timeout otherwise leaves nothing
/// behind but the name of its binary: the console reporter reports a case only once it has ended.
///
/// Off unless CaseTraceVariable is set. Every first-party suite that links `contour_test_trace` registers the
/// listener (CaseTrace.cpp); CI turns it on where a hang has been seen.

#include <core/Environment.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace contour::testing
{

/// The environment variable that turns the trace on: set to anything but empty or `0`.
inline constexpr auto CaseTraceVariable = std::string_view { "CONTOUR_TEST_TRACE" };

/// @param environment Where CaseTraceVariable is looked up.
/// @return Whether the trace is on.
[[nodiscard]] inline bool caseTraceEnabled(core::Environment const& environment)
{
    auto const value = environment.get(CaseTraceVariable);
    return value && !value->empty() && *value != "0";
}

/// What one line of the trace reports.
enum class CaseTraceEvent : uint8_t
{
    CaseStarting,    ///< A test case starts (once per run of it: a case with sections runs once per leaf).
    SectionStarting, ///< A section within it starts.
    CaseEnded,       ///< The test case ended.
};

/// @param event What happened.
/// @param name The test case's name, or the section's.
/// @param depth How deep a section is nested in its test case, from 1; ignored for the case's own events.
/// @param elapsed How long the case ran; read for CaseTraceEvent::CaseEnded only.
/// @return The trace line, without its newline.
[[nodiscard]] inline std::string caseTraceLine(CaseTraceEvent event,
                                               std::string_view name,
                                               std::size_t depth,
                                               std::chrono::milliseconds elapsed)
{
    switch (event)
    {
        case CaseTraceEvent::CaseStarting: return std::format("[trace] case: {}", name);
        case CaseTraceEvent::SectionStarting:
            return std::format("[trace] {:>{}}section: {}", "", 2 * depth, name);
        case CaseTraceEvent::CaseEnded:
            return std::format("[trace] ended: {} ({} ms)", name, elapsed.count());
    }
    return {};
}

} // namespace contour::testing
