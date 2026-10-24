// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether the daemon a client meets runs the client's own release, and what to tell the user when not.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vthost/proto/Negotiation.hpp>

namespace vthost::client
{

/// How a daemon's release stands against the client's.
enum class DriftKind : uint8_t
{
    None = 0,      ///< The same release.
    DaemonOlder,   ///< The daemon's release number is lower.
    DaemonNewer,   ///< The daemon's release number is higher.
    Differs,       ///< Same or incomparable numbers, different text.
    DaemonUnknown, ///< The daemon states no release (it predates the field).
};

/// @param release A release string such as "0.8.1-master+abc".
/// @return The leading dot-separated decimal run ({0, 8, 1}); nullopt when it does not start with a digit.
[[nodiscard]] std::optional<std::vector<std::uint32_t>> parseReleaseNumbers(std::string_view release);

/// @param client The client's release.
/// @param daemon The daemon's release; empty when it states none.
/// @return How the daemon's release stands against the client's.
[[nodiscard]] DriftKind compareReleases(std::string_view client, std::string_view daemon);

/// Whether the user's windows and tabs come back after the daemon restarts.
enum class LayoutAfterRestart : uint8_t
{
    Lost = 0, ///< Every shell ends and a fresh one opens.
    Restored, ///< The daemon snapshots its layout and restores it.
};

/// @param negotiated What the handshake with the daemon agreed.
/// @return What a restart keeps of the daemon's layout: Restored when the daemon offered
///         Capability::WorkspaceRestore, else Lost.
[[nodiscard]] LayoutAfterRestart layoutSurvivesRestart(proto::Negotiated const& negotiated) noexcept;

/// What to tell the user about a drift.
struct DriftNotice
{
    DriftKind kind = DriftKind::None; ///< The drift.
    std::string text;                 ///< One line naming it; empty when there is none.
    std::string confirmation;         ///< What a restart costs, for the confirmation dialog.
};

/// The label of the action that restarts the daemon.
inline constexpr std::string_view RestartActionLabel = "Restart daemon on the new version";

/// @param clientRelease The client's release.
/// @param daemonRelease The daemon's release; empty when it states none.
/// @param layout What a restart keeps.
/// @return The notice; its text is empty when the releases match.
[[nodiscard]] DriftNotice assessDrift(std::string_view clientRelease,
                                      std::string_view daemonRelease,
                                      LayoutAfterRestart layout);

} // namespace vthost::client
