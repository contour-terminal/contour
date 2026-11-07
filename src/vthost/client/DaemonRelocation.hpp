// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Moving the daemons of one installation onto their staged copies: which daemon is the installation's to
/// move, and which entries of a socket directory are daemons at all.

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <vthost/StatePath.hpp>
#include <vthost/client/DaemonControlClient.hpp>

namespace vthost::client
{

/// What becomes of a daemon when its installation is about to be replaced.
enum class RelocationVerdict : uint8_t
{
    Move = 0,           ///< It runs from the installation and allows a move onto its staged copy.
    OtherInstallation,  ///< It runs from another place: the installation's files are not its concern.
    InPlaceRequested,   ///< It was started with `--in-place`: it stays where it is.
    PredatesRelocation, ///< Its release does not state whether it may move, so it cannot be moved.
};

/// Why a daemon is left, in the words `contour daemon relocate` says it in; Move has no reason.
inline constexpr auto RelocationVerdictReasons =
    std::to_array<std::pair<RelocationVerdict, std::string_view>>({
        { RelocationVerdict::Move, "" },
        { RelocationVerdict::OtherInstallation, "it does not run from this installation's folder" },
        { RelocationVerdict::InPlaceRequested, "it was started with --in-place" },
        { RelocationVerdict::PredatesRelocation, "its release cannot be moved" },
    });

/// @param verdict A verdict.
/// @return Why a daemon it is the verdict of is left; empty for Move.
[[nodiscard]] constexpr std::string_view reasonOf(RelocationVerdict verdict) noexcept
{
    for (auto const& [candidate, reason]: RelocationVerdictReasons)
        if (candidate == verdict)
            return reason;
    return {};
}

/// Decides what to do with a running daemon.
/// @param status The daemon's status.
/// @param installationBin The directory of the installation's executable.
/// @param platform Whose path conventions decide whether two directories are the same: a Windows path is
///        compared without regard to case, with either separator. Compared by spelling, after normalizing:
///        a short (8.3) name, a junction or a substituted drive is not seen as the same directory, as the
///        file system has to be asked for that, which this pure decision leaves out. The daemon's path is
///        the one it was launched by, the installation's the one the installer recorded, both long.
/// @return OtherInstallation unless the daemon's executable lies directly in @p installationBin; then
///         PredatesRelocation for a daemon that states no request, InPlaceRequested for one that refuses a
///         move, else Move.
[[nodiscard]] RelocationVerdict relocationVerdictFor(DaemonStatus const& status,
                                                     std::filesystem::path const& installationBin,
                                                     HostPlatform platform);

/// Keeps the control sockets among the entries of a socket directory.
/// @param entries The directory's entries.
/// @return The entries that are no sibling endpoint of a control socket, in their order: one is dropped only
///         where its name is another entry's name and a sibling suffix (SiblingSuffixes), so a label that
///         merely ends in one is kept.
[[nodiscard]] std::vector<std::filesystem::path> controlSocketsAmong(
    std::span<std::filesystem::path const> entries);

} // namespace vthost::client
