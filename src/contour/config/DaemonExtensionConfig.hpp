// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigFwd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>

namespace YAML
{
class Node;
}

namespace contour::config
{

/// One entry of `daemon.extensions`: a WebAssembly extension the daemon runs with no window attached.
///
/// `autostart` is a YAML boolean, converted to DaemonExtensionAutostart here and nowhere further in: the
/// configuration's fields are the documented exception to the enum-over-bool rule.
struct DaemonExtensionConfig
{
    std::string name;           ///< The key under `daemon.extensions`.
    std::filesystem::path path; ///< The module on disk; empty names a bundled extension, found by @ref name.
    DaemonExtensionAutostart autostart = DaemonExtensionAutostart::OnDemand; ///< When it starts.
    std::string configJson = "{}"; ///< Its `config` subtree, as JSON: what the extension is loaded with.

    bool operator==(DaemonExtensionConfig const&) const = default;
};

/// Why a YAML subtree has no JSON spelling.
enum class YamlToJsonError : uint8_t
{
    TooDeep,          ///< Lists and maps nest deeper than allowed.
    NotRepresentable, ///< A key that is no scalar, a key given twice, or a number JSON cannot spell.
};

/// Converts a YAML subtree to JSON. Plain scalars are read by YAML 1.2's core schema (null, bool, int,
/// float), quoted ones as strings; maps keep their order.
/// @param node The subtree.
/// @param maxDepth How deeply lists and maps may nest: a map at the top is depth 1.
/// @return The JSON text; or why there is none.
[[nodiscard]] std::expected<std::string, YamlToJsonError> yamlToJson(YAML::Node const& node,
                                                                     std::size_t maxDepth = 16);

} // namespace contour::config
