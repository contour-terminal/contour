// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigFwd.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace YAML
{
class Node;
}

namespace contour::config
{

/// One entry of `daemon.extensions.<name>.programs`: a program the extension's commands and panes run from a
/// given executable rather than from the daemon's PATH, which a service manager often leaves without the
/// user's own bin directories.
struct DaemonExtensionProgram
{
    std::string name;           ///< The program as the extension's argv names it, such as `claude`: no `/`.
    std::filesystem::path path; ///< The executable: absolute, `~` and variables expanded.

    bool operator==(DaemonExtensionProgram const&) const = default;
};

/// One entry of `daemon.extensions.<name>.env_sets.<set>.<variable>`: a variable a set sets, as written.
struct DaemonEnvironmentVariable
{
    std::string name;  ///< The variable's name, such as `CLAUDE_CONFIG_DIR`.
    std::string value; ///< Its value as written: `~` is expanded by the host, not here.

    bool operator==(DaemonEnvironmentVariable const&) const = default;
};

/// One entry of `daemon.extensions.<name>.env_sets`: a named set of variables the extension's workers run
/// with.
struct DaemonEnvironmentSet
{
    std::string name; ///< The set's name: `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`.
    std::vector<DaemonEnvironmentVariable> variables = {}; ///< In the configuration's order.

    bool operator==(DaemonEnvironmentSet const&) const = default;
};

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
    std::vector<DaemonExtensionProgram> programs {}; ///< Its `programs`, in the configuration's order.
    std::vector<DaemonEnvironmentSet>
        environmentSets {}; ///< Its `env_sets`, in the configuration's order; values as written.

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
