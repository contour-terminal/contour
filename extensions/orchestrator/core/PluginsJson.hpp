// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// What `claude plugin list --json` prints, as Plugins.cpp reads it; nothing else includes this header. Glaze
/// reflects only types with linkage, so this cannot sit in Plugins.cpp's anonymous namespace. Only the keys
/// the orchestrator reads are members: the rest are skipped.
namespace orchestrator::plugins::detail
{

/// One installed plugin.
struct JsonPlugin
{
    std::string id; ///< Such as `contour-workflows@contour-terminal`.
    /// Whether Claude Code loads it. A `bool` because the JSON spells it so: the wire boundary's documented
    /// exception, read only by enabledPluginsOf().
    bool enabled = false;
};

} // namespace orchestrator::plugins::detail
