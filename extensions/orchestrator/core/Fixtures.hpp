// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orchestrator
{

/// The source directories test fixtures are read from.
enum class FixtureRoot : uint8_t
{
    Contract, ///< contract/: what Contour's host runs too, such as the manifest corpus.
    Testdata, ///< core/testdata/: recorded output of the tools the extension runs.
    Screens,  ///< view/testdata/: the dashboard's golden screens.
};

/// Reads a fixture file in full; fails the running test case if it cannot.
/// @param root The directory @p relativePath is relative to.
/// @param relativePath The file, '/'-separated.
/// @return The file's bytes, unchanged.
[[nodiscard]] std::string readFixture(FixtureRoot root, std::string_view relativePath);

/// Reads a fixture file in full, if it exists.
/// @param root The directory @p relativePath is relative to.
/// @param relativePath The file, '/'-separated.
/// @return The file's bytes, unchanged; std::nullopt when there is no such file.
[[nodiscard]] std::optional<std::string> readFixtureIfPresent(FixtureRoot root,
                                                              std::string_view relativePath);

/// Writes a fixture file, replacing what it held; fails the running test case if it cannot.
/// @param root The directory @p relativePath is relative to.
/// @param relativePath The file, '/'-separated.
/// @param bytes What the file is to hold.
void writeFixture(FixtureRoot root, std::string_view relativePath, std::string_view bytes);

/// @return Whether golden fixtures are to be written rather than compared: ORCHESTRATOR_UPDATE_GOLDENS is
/// set.
[[nodiscard]] bool updatingGoldens();

/// Lists the regular files of a fixture directory; fails the running test case if it cannot.
/// @param root The directory @p relativeDirectory is relative to.
/// @param relativeDirectory The directory, '/'-separated.
/// @return The file names, without their directory, sorted.
[[nodiscard]] std::vector<std::string> fixtureNames(FixtureRoot root, std::string_view relativeDirectory);

} // namespace orchestrator
