// SPDX-License-Identifier: Apache-2.0
#include <vtconformance/EscTestLog.h>

#include <crispy/utils.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace vtconformance
{

namespace
{
    /// esctest closes with, e.g., `*** 27 tests passed, 1 known bug, 0 tests failed ***`. It shouts
    /// the last field when it is non-zero (`3 TESTS FAILED`), so match on the numbers, not the words.
    constexpr auto VerdictPrefix = std::string_view { "*** " };
    constexpr auto VerdictSuffix = std::string_view { " ***" };
    constexpr auto FailingTestsHeader = std::string_view { "Failing tests:" };

    [[nodiscard]] constexpr bool isDigit(char ch) noexcept
    {
        return ch >= '0' && ch <= '9';
    }

    [[nodiscard]] bool isVerdictLine(std::string_view line) noexcept
    {
        return line.starts_with(VerdictPrefix) && line.ends_with(VerdictSuffix);
    }

    /// @return The decimal numbers appearing in @p line, in order.
    [[nodiscard]] std::vector<size_t> numbersIn(std::string_view line)
    {
        auto const toNumber = [](std::string_view digits) {
            auto value = size_t { 0 };
            std::from_chars(digits.data(), digits.data() + digits.size(), value);
            return value;
        };

        // Group the line into runs of digits and runs of non-digits, then keep the former.
        return std::ranges::to<std::vector>(
            line | std::views::chunk_by([](char a, char b) { return isDigit(a) == isDigit(b); })
            | std::views::transform([](auto&& run) { return std::string_view { run }; })
            | std::views::filter([](std::string_view run) { return isDigit(run.front()); })
            | std::views::transform(toNumber));
    }

    /// A test name, as esctest prints it in its failure list: `EDTests.test_ED_0`.
    ///
    /// The list is not terminated by a blank line -- esctest goes straight on to log the sequences it
    /// sends while tearing down -- so the shape of the line is what ends it.
    [[nodiscard]] bool looksLikeTestName(std::string_view line) noexcept
    {
        return !line.empty() && line.find(' ') == std::string_view::npos
               && line.find('.') != std::string_view::npos;
    }
} // namespace

EscTestSummary parseEscTestLog(std::string_view text)
{
    auto summary = EscTestSummary {};
    auto const lines = crispy::split(text, '\n');

    // The last verdict is the run's verdict: esctest prints one when it finishes, and a resumed or
    // re-run log could hold an earlier one.
    auto const reversed = lines | std::views::reverse;
    auto const verdict = std::ranges::find_if(
        reversed, [](std::string_view line) { return isVerdictLine(line) && numbersIn(line).size() >= 3; });

    if (verdict == reversed.end())
    {
        summary.parseError = "esctest printed no verdict -- it crashed, or never started";
        return summary;
    }

    auto const numbers = numbersIn(*verdict);
    summary.passed = numbers[0];
    summary.knownBugs = numbers[1];
    summary.failed = numbers[2];

    // Everything after the verdict line, in forward order again.
    auto const tail = std::ranges::subrange { verdict.base(), lines.end() };
    auto const header = std::ranges::find(tail, FailingTestsHeader);
    if (header != tail.end())
        summary.failingTests = std::ranges::to<std::vector>(
            std::ranges::subrange { std::next(header), tail.end() }
            | std::views::take_while(looksLikeTestName)
            | std::views::transform([](std::string_view name) { return std::string { name }; }));

    return summary;
}

} // namespace vtconformance
