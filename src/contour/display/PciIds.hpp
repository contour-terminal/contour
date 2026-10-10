// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/GpuSelector.hpp>
#include <contour/display/GpuSelection.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace contour::display
{

/// What the pci.ids database says about one device.
struct PciNames
{
    std::string vendor;                ///< The vendor line's name, e.g. "NVIDIA Corporation".
    std::optional<std::string> device; ///< The device line's name, when the database has one.
};

/// Looks up @p id in the text of a pci.ids database (the hwdata format).
/// @param database The whole file's text.
/// @param id The device to name.
/// @return The names, or nullopt when the vendor is not in the database.
[[nodiscard]] std::optional<PciNames> lookupPciNames(std::string_view database, config::PciId id);

/// A title a person can read: "<vendor> <device>", or "<vendor> <kind> GPU" when the device has no name.
/// @param id The device.
/// @param kind Integrated, discrete, ...; used only when the device has no name.
/// @param names What lookupPciNames() found, if anything.
/// @return The title; never contains the raw id.
[[nodiscard]] std::string gpuTitle(config::PciId id, GpuKind kind, std::optional<PciNames> const& names);

} // namespace contour::display
