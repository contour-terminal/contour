// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Why a live upgrade did not happen: the vocabulary the orchestrator, its readiness probe and the protocol
/// that reports the outcome share.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <system_error>
#include <variant>

namespace vthost::handover
{

/// Why a live upgrade was refused. A refusal leaves the daemon serving as it did: every session running,
/// every listener accepting. Clients told the daemon was upgrading reconnect to the same daemon.
///
/// Zero is InProgress, as the protocol that reports a refusal numbers it (no refusal is benign; the zero one
/// is the one a second request most often meets). New refusals are appended.
enum class UpgradeRefusal : uint8_t
{
    InProgress = 0,   ///< Another upgrade of this daemon is under way.
    NotStaged,        ///< The target names no complete image of the daemon's store, or it cannot be leased.
    Sandboxed,        ///< The daemon runs in a sandbox (Flatpak) that cannot reach a new release's binary.
    FormatUnreadable, ///< The target cannot read the handover or terminal state formats this daemon writes.
    InFlightTailTooLarge, ///< A session's escape sequence in flight outgrew what a capture keeps.
    TargetUnresponsive,   ///< The target did not answer its readiness probe in time, or could not be started.
    NotSupported,         ///< This platform cannot replace a process image in place.
    UnsupportedSession,   ///< A session runs on a device nothing could adopt after the exec (SSH, a channel).
    SessionStarting,      ///< A session kept starting through every retry: its input could not be carried.
    QuiesceTimedOut,      ///< The clients did not go, or the sessions' pumps did not settle, in time.
    /// The handover image outgrew what a new image reads (MaxHandoverImageSize): images in scrollback.
    ImageTooLarge,
    /// The target cannot load the daemon's configuration file, or it no longer names the daemon's profile.
    ConfigUnreadable,
    LocalOnly, ///< Asked over the network: only a peer on the daemon's machine may replace its binary.
};

/// The exec into the new image failed after everything was stopped; the daemon resumed, and serves on.
struct ExecFailure
{
    std::error_code cause; ///< Why: the exec's own error, or what preparing the descriptors for it failed on.

    bool operator==(ExecFailure const&) const = default;
};

/// Why a live upgrade did not happen.
using UpgradeError = std::variant<UpgradeRefusal, ExecFailure>;

/// One row per refusal: what the user is told.
struct UpgradeRefusalRow
{
    UpgradeRefusal refusal; ///< The refusal.
    std::string_view text;  ///< Its explanation, one sentence fragment, lower case.
};

/// What each refusal tells the user, in enumerator order.
inline constexpr auto UpgradeRefusalRows = std::array {
    UpgradeRefusalRow { UpgradeRefusal::InProgress, "an upgrade of this daemon is already under way" },
    UpgradeRefusalRow { UpgradeRefusal::NotStaged,
                        "the new release is not staged in the daemon's image store" },
    UpgradeRefusalRow { UpgradeRefusal::Sandboxed,
                        "the daemon runs in a sandbox that cannot reach the new release" },
    UpgradeRefusalRow { UpgradeRefusal::FormatUnreadable,
                        "the new release cannot read the state this daemon hands over" },
    UpgradeRefusalRow { UpgradeRefusal::InFlightTailTooLarge,
                        "a program is in the middle of an escape sequence too long to carry over" },
    UpgradeRefusalRow { UpgradeRefusal::TargetUnresponsive,
                        "the new release did not answer its readiness check" },
    UpgradeRefusalRow { UpgradeRefusal::NotSupported, "live upgrade is not supported on this platform" },
    UpgradeRefusalRow { UpgradeRefusal::UnsupportedSession,
                        "a session runs on a connection that cannot be carried over (SSH or a channel)" },
    UpgradeRefusalRow { UpgradeRefusal::SessionStarting, "a session is still starting" },
    UpgradeRefusalRow { UpgradeRefusal::QuiesceTimedOut,
                        "the daemon's clients and sessions did not settle in time" },
    UpgradeRefusalRow { UpgradeRefusal::ImageTooLarge,
                        "the sessions' state is too large to hand over (too many images in scrollback)" },
    UpgradeRefusalRow { UpgradeRefusal::ConfigUnreadable,
                        "the new release cannot load the daemon's configuration or profile" },
    UpgradeRefusalRow { UpgradeRefusal::LocalOnly, "live upgrade is local only" },
};

static_assert(std::ranges::all_of(std::views::iota(std::size_t { 0 }, UpgradeRefusalRows.size()),
                                  [](std::size_t index) {
                                      return static_cast<std::size_t>(UpgradeRefusalRows.at(index).refusal)
                                             == index;
                                  }),
              "UpgradeRefusalRows must list every refusal in enumerator order");

/// What the user is told of an ExecFailure: the one outcome that is no refusal (UpgradeRefusal has no
/// enumerator for it). The daemon stopped its sessions' pumps, could not exec, and resumed them.
inline constexpr std::string_view ExecFailureText =
    "the daemon could not start the new release and keeps serving on the old one";

/// @return The refusal with the highest number: the bound a wire decoder holds a byte to.
[[nodiscard]] constexpr UpgradeRefusal lastRefusal() noexcept
{
    return UpgradeRefusalRows.back().refusal;
}

/// @param refusal A refusal.
/// @return What it tells the user.
[[nodiscard]] constexpr std::string_view describe(UpgradeRefusal refusal) noexcept
{
    return UpgradeRefusalRows.at(static_cast<std::size_t>(refusal)).text;
}

} // namespace vthost::handover
