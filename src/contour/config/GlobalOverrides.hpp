// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace contour::config
{

struct Config;
struct YAMLConfigReader;

/// How a global setting is spelled in settings.yml.
enum class GlobalOverrideShape : uint8_t
{
    Scalar,  ///< A top-level key holding the value: `reflow_on_resize: false`.
    Section, ///< A section whose leaves are overridden one by one: `gutter: { line_numbers: relative }`.
};

/// One global setting that settings.yml may override, and how its override is read.
///
/// Both halves of a global override use these rows: the merge of settings.yml over contour.yml applies every
/// scalar row, and a section's row where settings.yml names a leaf of it; and the settings page refuses to
/// save a key that no row covers. A setting is made overridable by adding a row; a leaf of a section that has
/// a row already needs none.
struct GlobalOverride
{
    /// The contour.yml key: a scalar's own, or the section's.
    std::string_view key;
    GlobalOverrideShape shape; ///< How settings.yml spells it.
    /// Reads the override of @p key, a row's key, from settings.yml into @p config, through the loader
    /// contour.yml uses. A section's loader reads only the leaves settings.yml has.
    void (*apply)(YAMLConfigReader& reader, Config& config, std::string const& key);

    /// Whether this row applies the override of @p keyPath: the scalar's own key, or a leaf of the
    /// section, spelled "section.leaf". A scalar where a section belongs (`gutter: foo`) is no leaf of it.
    /// @param keyPath A settings.yml key, as GuiManagedSettings::globalOverrides holds it.
    [[nodiscard]] bool covers(std::string_view keyPath) const noexcept
    {
        if (shape == GlobalOverrideShape::Scalar)
            return keyPath == key;
        return keyPath.size() > key.size() && keyPath.starts_with(key) && keyPath[key.size()] == '.';
    }
};

/// Every global setting that settings.yml may override, in the order the merge applies them.
[[nodiscard]] std::span<GlobalOverride const> globalOverrides() noexcept;

/// The row that applies the override of @p keyPath, or nullptr when settings.yml cannot override it.
/// @param keyPath A scalar's key, or "section.leaf".
[[nodiscard]] GlobalOverride const* globalOverrideFor(std::string_view keyPath) noexcept;

} // namespace contour::config
