// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The rows `cacheHome` is held to, one table per operating system: each is defined in a
/// CMake-selected `*/CacheHomeCases_test.cpp`, since what an absolute path looks like, and the row
/// below the home directory, differ per OS. Test-only; never part of the module.

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::platform::test
{

/// One environment and the cache directory it gives.
struct CacheHomeCase
{
    std::string_view name;                                       ///< What the row is about.
    std::map<std::string, std::string, std::less<>> environment; ///< The variables set; every other is unset.
    std::optional<std::filesystem::path> expected;               ///< What cacheHome answers.
};

/// @return This operating system's rows.
[[nodiscard]] std::vector<CacheHomeCase> cacheHomeCases();

} // namespace core::platform::test
