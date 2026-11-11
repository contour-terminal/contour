// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The platform half of `<core/Random.hpp>`: one definition per platform, chosen by the source lists
/// in `src/core/CMakeLists.txt`.
///
/// Private to core::base: listed under SOURCES, not HEADERS, and not for a consumer to include.

#include <cstddef>
#include <span>

namespace core::detail
{

/// Overwrites @p out entirely with bytes from the operating system's random source.
///
/// Retries what the system allows to be retried (a short read, an interrupted call); a hard failure
/// ends the process, since there is no safe way to go on without randomness.
/// @param out The bytes to fill.
void fillFromSystemRandom(std::span<std::byte> out);

} // namespace core::detail
