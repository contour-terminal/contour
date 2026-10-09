// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string_view>

namespace contour::config
{

/// A PCI vendor:device identifier: the id sysfs, Vulkan, DXGI and Mesa's DRI_PRIME all report.
struct PciId
{
    std::uint16_t vendor = 0; ///< PCI vendor id, e.g. 0x10de.
    std::uint16_t device = 0; ///< PCI device id, e.g. 0x2820.

    [[nodiscard]] constexpr bool operator==(PciId const&) const noexcept = default;
};

/// How `renderer.gpu` asks for a GPU.
enum class GpuPreference : std::uint8_t
{
    Auto,       ///< Power first: integrated, then discrete.
    Integrated, ///< Integrated if present.
    Discrete,   ///< Discrete if present, else integrated.
    Specific,   ///< The GPU named by GpuSelector::id.
};

/// A parsed `renderer.gpu` value.
struct GpuSelector
{
    GpuPreference preference = GpuPreference::Auto; ///< What kind of GPU is asked for.
    std::optional<PciId> id;                        ///< Set exactly when preference is Specific.

    [[nodiscard]] constexpr bool operator==(GpuSelector const&) const noexcept = default;
};

/// Why a `renderer.gpu` value could not be parsed.
enum class GpuSelectorError : std::uint8_t
{
    Malformed, ///< Neither a keyword nor a vvvv:dddd id.
};

/// Parses a PCI id written as `vvvv:dddd` (hexadecimal, one to four digits each, any case).
/// @param text The id text, without surrounding whitespace.
/// @return The id, or nullopt when @p text is not one.
[[nodiscard]] std::optional<PciId> parsePciId(std::string_view text) noexcept;

/// Parses a `renderer.gpu` value. Surrounding whitespace is ignored and keywords are case-insensitive.
/// @param text `auto`, `integrated`, `discrete`, a `vvvv:dddd` id, or empty (meaning `auto`).
/// @return The selector, or GpuSelectorError::Malformed.
[[nodiscard]] std::expected<GpuSelector, GpuSelectorError> parseGpuSelector(std::string_view text);

/// The keyword spelling of @p preference; empty for GpuPreference::Specific.
[[nodiscard]] std::string_view keywordOf(GpuPreference preference) noexcept;

} // namespace contour::config

template <>
struct std::formatter<contour::config::PciId>: formatter<std::string_view>
{
    auto format(contour::config::PciId const& id, auto& ctx) const
    {
        return formatter<std::string_view>::format(std::format("{:04x}:{:04x}", id.vendor, id.device), ctx);
    }
};

template <>
struct std::formatter<contour::config::GpuSelector>: formatter<std::string_view>
{
    auto format(contour::config::GpuSelector const& selector, auto& ctx) const
    {
        if (selector.preference == contour::config::GpuPreference::Specific && selector.id)
            return formatter<std::string_view>::format(std::format("{}", *selector.id), ctx);
        return formatter<std::string_view>::format(contour::config::keywordOf(selector.preference), ctx);
    }
};
