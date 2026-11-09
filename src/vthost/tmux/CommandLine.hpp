// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vthost::tmux
{

/// Splits a command line into arguments, honouring single and double quotes
/// (double quotes keep backslash escapes for `\"` and `\\`); outside quotes a backslash makes the next
/// character literal.
/// @param line The raw command line.
/// @return The argument vector (possibly empty).
[[nodiscard]] std::vector<std::string> splitCommandLine(std::string_view line);

/// Quotes @p argument as one word of a tmux command line: in single quotes, which keep everything literal
/// (a backslash included), with a quote inside closed, escaped and reopened (`'\''`). The inverse of
/// splitCommandLine() for one argument.
/// @param argument The argument text.
/// @return The quoted word.
[[nodiscard]] std::string quoteArgument(std::string_view argument);

} // namespace vthost::tmux
