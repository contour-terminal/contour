// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: what fails a test binary's run as a whole, for a check a Catch2 listener makes once a case
/// ended -- when the case can no longer fail by an assertion of its own. The binary's main() reports each
/// and fails the run.

#include <string>
#include <vector>

namespace vtextension::testing
{

/// @return What failed the run so far, one line each; written and read on the thread the cases run on.
[[nodiscard]] inline std::vector<std::string>& runGateFailures()
{
    static auto failures = std::vector<std::string> {};
    return failures;
}

} // namespace vtextension::testing
