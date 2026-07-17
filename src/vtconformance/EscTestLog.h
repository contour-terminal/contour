// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vtconformance
{

/// What esctest reported about itself.
///
/// esctest needs no oracle of ours: it asserts, per test case, what the screen and the terminal's
/// replies should be, and prints a verdict. Our whole job is to read it back honestly.
struct EscTestSummary
{
    size_t passed = 0;

    /// Tests esctest itself knows the reference terminal fails. Neither a pass nor a failure.
    size_t knownBugs = 0;

    size_t failed = 0;

    /// The fully-qualified names of the failing tests, e.g. "EDTests.test_ED_0".
    std::vector<std::string> failingTests;

    /// Set when esctest never printed a verdict at all — it crashed, or never started.
    std::string parseError;

    /// @return Whether esctest ran to completion and printed a verdict we could read.
    [[nodiscard]] bool valid() const noexcept { return parseError.empty(); }

    /// @return How many tests esctest actually decided.
    [[nodiscard]] size_t total() const noexcept { return passed + knownBugs + failed; }
};

/// Parses esctest's `--logfile` output.
///
/// The two things worth reading are its closing verdict line
///
///     *** 27 tests passed, 1 known bug, 0 tests failed ***
///
/// and, when anything failed, the `Failing tests:` block that follows it. Everything else in the log
/// is a transcript of the sequences it sent, which is useful to a human and to nobody else.
[[nodiscard]] EscTestSummary parseEscTestLog(std::string_view text);

} // namespace vtconformance
