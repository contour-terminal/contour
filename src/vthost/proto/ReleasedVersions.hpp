// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The protocol range each release of contour shipped, and the compatibility window it implies: a build
/// speaks at least the protocol versions of the previous two releases, counted from the first release that
/// ships version negotiation.
///
/// The table ends in the UnreleasedProtocol row, the build under development. Cutting a release inserts the
/// release's row before it and records that version's transcripts (vthost/testing/RecordedTranscripts.hpp).
/// `Compatibility_test.cpp` reads `metainfo.xml` and fails a release whose newest dated entry has no row,
/// so neither step can be forgotten.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <vthost/proto/Negotiation.hpp>

namespace vthost::proto
{

/// The protocol versions one release of contour speaks.
struct ReleasedProtocol
{
    std::string_view release; ///< The release's version, as `metainfo.xml` names it; or UnreleasedProtocol.
    VersionRange versions;    ///< The protocol versions that release speaks.
};

/// The `release` of the last row, which stands for the build under development: it always speaks
/// SupportedVersions, and cutting a release leaves it in place, inserting the release's row before it.
constexpr std::string_view UnreleasedProtocol = "unreleased";

/// The newest release of contour that predates version negotiation: as long as it is the newest dated
/// release in `metainfo.xml`, no release has shipped negotiation and the only row is UnreleasedProtocol.
constexpr std::string_view LastReleaseBeforeNegotiation = "0.8.0";

/// One row per release that shipped version negotiation, oldest first, then the UnreleasedProtocol row.
/// A release's row is never edited once the release has shipped.
constexpr std::array ReleasedProtocols {
    ReleasedProtocol { .release = UnreleasedProtocol, .versions = SupportedVersions },
};

static_assert(!ReleasedProtocols.empty(), "ReleasedProtocols ends in the UnreleasedProtocol row");
static_assert(ReleasedProtocols.back().release == UnreleasedProtocol,
              "ReleasedProtocols ends in the UnreleasedProtocol row");
static_assert(ReleasedProtocols.back().versions == SupportedVersions,
              "the UnreleasedProtocol row is this build: it speaks SupportedVersions");

/// How many releases before the newest one a build must still speak the protocol of.
constexpr std::size_t CompatibilityWindowReleases = 2;

/// The lowest protocol version the newest row in @p releases must still speak: the highest version of the
/// row CompatibilityWindowReleases rows before it, or of the oldest row when fewer exist. Over
/// ReleasedProtocols the newest row is this build, and the floor is that of the older of the previous two
/// releases.
/// @param releases The released protocols, oldest first; not empty.
/// @return That release's `versions.max`.
[[nodiscard]] constexpr uint32_t compatibilityFloor(std::span<ReleasedProtocol const> releases) noexcept
{
    auto const index =
        releases.size() > CompatibilityWindowReleases ? releases.size() - 1 - CompatibilityWindowReleases : 0;
    return releases[index].versions.max;
}

} // namespace vthost::proto
