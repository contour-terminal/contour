// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <span>
#include <string_view>

namespace orchestrator
{

/// A code the daemon world answers a host call with, and what it tells the user.
struct FailureWords
{
    std::string_view code = {};   ///< As the daemon world spells it, such as `not-supported-yet`.
    std::string_view plain = {};  ///< What the summary and the dashboard say instead.
    std::string_view detail = {}; ///< What a details panel adds, such as a likely cause; empty: nothing.
};

/// @return Every error code the daemon world answers a host call with, each in plain words: the host's
///         errors and the file errors, in one table.
[[nodiscard]] std::span<FailureWords const> failureWords() noexcept;

/// @param why Why a run failed: a host call's error code, or words of the engine's or a command's own.
/// @return The row for @p why when it is one of failureWords()' codes; else nullptr.
[[nodiscard]] FailureWords const* failureWordsOf(std::string_view why) noexcept;

/// @param why Why a run failed: a host call's error code, or words of the engine's or a command's own.
/// @return The plain words for @p why when it is one of failureWords()' codes; else @p why itself.
[[nodiscard]] std::string_view plainFailure(std::string_view why) noexcept;

} // namespace orchestrator
