// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace contour::config
{

/// Whether the operating system can draw the chrome's materials (Mica, vibrancy) behind a window.
///
/// Kept here rather than in ChromeTokenGroups.hpp so that platform::PlatformAppearance, which publishes it,
/// can name it without pulling in the token rows.
enum class MaterialSupport : uint8_t
{
    Unknown = 0, //!< Not published: changes nothing (every OS but Windows).
    Supported,   //!< The OS draws them.
    Unsupported, //!< The OS cannot (Windows before 11 22H2): a material resolves to Solid.
};

/// Number of @c MaterialSupport enumerators, derived from the last one.
inline constexpr size_t MaterialSupportCount = static_cast<size_t>(MaterialSupport::Unsupported) + 1;

} // namespace contour::config
