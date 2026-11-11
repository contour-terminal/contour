// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A release string's numbers, and how two releases order by them: what a client weighs a daemon's release
/// by, and what the PTY keeper picks the newest image to restart a daemon from by.

#include <compare>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace vthost
{

/// @param release A release string such as "0.8.1-master+abc".
/// @return The leading dot-separated decimal run ({0, 8, 1}); nullopt when it does not start with a digit.
[[nodiscard]] std::optional<std::vector<std::uint32_t>> parseReleaseNumbers(std::string_view release);

/// Orders two releases by their numbers, a missing number counting as zero ("0.8" and "0.8.0" are equal).
/// @param a A release string. @param b Another.
/// @return How @p a's numbers order against @p b's; nullopt when either has none (@see parseReleaseNumbers).
[[nodiscard]] std::optional<std::strong_ordering> compareReleaseNumbers(std::string_view a,
                                                                        std::string_view b);

} // namespace vthost
