// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include <vtextension/Manifest.hpp>

namespace vtextension::testing
{

/// @return The agent orchestrator's manifest, parsed from the conformance corpus
///         (`VTEXTENSION_MANIFEST_CORPUS_DIR`, defined for vtextension_test only); the case fails without it.
[[nodiscard]] inline Manifest orchestratorManifest()
{
    auto file = std::ifstream(std::filesystem::path { VTEXTENSION_MANIFEST_CORPUS_DIR } / "valid"
                                  / "orchestrator.manifest",
                              std::ios::binary);
    REQUIRE(file);
    auto text = std::ostringstream {};
    text << file.rdbuf();
    auto manifest = parseManifestText(std::move(text).str());
    REQUIRE(manifest.has_value());
    return std::move(*manifest);
}

} // namespace vtextension::testing
