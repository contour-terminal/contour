// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>

#include <string>
#include <unordered_map>

#include <vtworkspace/LayoutTree.hpp>

namespace contour::config
{

// The layout tree model (structs, realize, serialize) lives in vtworkspace::LayoutTree so the Qt-free
// daemon shares it. Callers name it there; this header used to re-export those names under
// `contour::` to keep the GUI's historical spellings working, which only hid where they come from.
// Only the YAML emission below is contour-specific (it is the sole yaml-cpp-coupled piece).

/// Renders a full `layouts:` YAML document (the exact text later written to `layouts.yml`) from
/// @p layouts, via yaml-cpp's YAML::Emitter — so quoting/escaping of names, commands, arguments and
/// paths is the emitter's job, not ours. Meant to be parsed back by the same config reader that
/// reads `layouts:` from `contour.yml`; layout names are emitted in sorted order so repeated saves
/// produce stable, diff-friendly output.
/// @param layouts The layouts to persist, keyed by name.
/// @return The complete YAML document text.
[[nodiscard]] std::string emitLayoutsYaml(std::unordered_map<std::string, config::Layout> const& layouts);

} // namespace contour::config
