// SPDX-License-Identifier: Apache-2.0
//
// The listener behind CaseTrace.hpp. Compiled into every suite that links `contour_test_trace` (an INTERFACE
// library, so the registration below is part of the suite's own objects and no linker drops it).

#include <testing/CaseTrace.hpp>

#include <core/Environment.hpp>

#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace
{

/// Writes the trace lines of CaseTrace.hpp to C's stderr, unbuffered, while CaseTraceVariable is set.
///
/// C's stream rather than std::cerr: Catch2 redirects the C++ streams of a reporter that captures output,
/// and a captured line is reported only once its case has ended -- which a hung case never does.
class CaseTraceListener final: public Catch::EventListenerBase
{
  public:
    /// @param config What Catch2 hands every listener.
    explicit CaseTraceListener(Catch::IConfig const* config):
        Catch::EventListenerBase { config },
        _isEnabled { contour::testing::caseTraceEnabled(core::LiveEnvironment {}) }
    {
    }

    void testCaseStarting(Catch::TestCaseInfo const& testInfo) override
    {
        _caseStarted = std::chrono::steady_clock::now();
        write(contour::testing::CaseTraceEvent::CaseStarting, testInfo.name);
    }

    void sectionStarting(Catch::SectionInfo const& sectionInfo) override
    {
        // The outermost section is the test case itself, already reported as such.
        if (_depth > 0)
            write(contour::testing::CaseTraceEvent::SectionStarting, sectionInfo.name);
        ++_depth;
    }

    void sectionEnded(Catch::SectionStats const& /*stats*/) override { --_depth; }

    void testCaseEnded(Catch::TestCaseStats const& stats) override
    {
        write(contour::testing::CaseTraceEvent::CaseEnded, stats.testInfo->name);
    }

  private:
    void write(contour::testing::CaseTraceEvent event, std::string_view name) const
    {
        if (!_isEnabled)
            return;
        auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - _caseStarted);
        auto const line = contour::testing::caseTraceLine(event, name, _depth, elapsed) + '\n';
        std::fputs(line.c_str(), stderr);
        std::fflush(stderr);
    }

    bool _isEnabled; ///< Whether CaseTraceVariable asked for the trace; read once, at the start of the run.
    std::size_t _depth = 0;
    std::chrono::steady_clock::time_point _caseStarted {};
};

} // namespace

CATCH_REGISTER_LISTENER(CaseTraceListener)
