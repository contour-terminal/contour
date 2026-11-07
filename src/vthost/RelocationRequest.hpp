// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether a daemon may move onto its staged image: the one vocabulary the daemon, its status reply and the
/// CLI read. A dependency-free header, so none of them needs the image-staging machinery for a two-valued
/// enum.

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

namespace vthost
{

/// Whether the user lets a daemon move onto its staged image (`contour daemon --in-place` refuses).
enum class RelocationRequest : uint8_t
{
    Allowed = 0,
    Refused,
};

/// The requests' text in `contour daemon status`, single-sourced.
inline constexpr auto RelocationRequestNames = std::to_array<std::pair<std::string_view, RelocationRequest>>({
    { "allowed", RelocationRequest::Allowed },
    { "refused (--in-place)", RelocationRequest::Refused },
});

/// @param request The request to name.
/// @return Its text in the status.
[[nodiscard]] constexpr std::string_view nameOf(RelocationRequest request) noexcept
{
    for (auto const& [name, value]: RelocationRequestNames)
        if (value == request)
            return name;
    return "allowed";
}

} // namespace vthost
