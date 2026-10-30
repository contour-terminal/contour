// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace contour::platform
{

/// The family the application bundles and lists first, whatever the system offers.
inline constexpr std::string_view BundledMonospaceFamily = "JetBrains Mono";

/// Lists the font families a terminal font can be picked from.
///
/// The Settings window's font picker reads it through the controller, so it never reaches the font
/// database itself and a test names the families it expects.
class IFontFamilySource
{
  public:
    virtual ~IFontFamilySource() = default;

    /// @return The monospace families on offer, the bundled one first, each name once.
    /// @pre Called on the GUI thread.
    [[nodiscard]] virtual std::vector<std::string> monospaceFamilies() const = 0;
};

/// The source for a host that cannot enumerate fonts: the bundled family alone.
class NullFontFamilySource final: public IFontFamilySource
{
  public:
    /// @return Just the bundled family.
    [[nodiscard]] std::vector<std::string> monospaceFamilies() const override
    {
        return { std::string { BundledMonospaceFamily } };
    }
};

/// The source that asks Qt's font database.
///
/// The bundled family first, then every fixed-pitch family the database knows, sorted and without
/// duplicates.
class QtFontFamilySource final: public IFontFamilySource
{
  public:
    /// @return The families, as described on the class.
    [[nodiscard]] std::vector<std::string> monospaceFamilies() const override;
};

} // namespace contour::platform
