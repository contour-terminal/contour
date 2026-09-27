// SPDX-License-Identifier: Apache-2.0
#include <core/testing/ExitCode.hpp>

namespace core::testing
{

int normalisedExitCode(Catch::Totals const& totals, int rawExitCode) noexcept
{
    if (totals.assertions.failed > 0 || totals.testCases.failed > 0)
        return 1;
    if (totals.testCases.total() > 0 && totals.testCases.skipped == totals.testCases.total())
        return SkipExitCode;
    if (totals.testCases.total() == 0 && rawExitCode != 0)
        return 2;
    return rawExitCode == 0 ? 0 : 1;
}

} // namespace core::testing
