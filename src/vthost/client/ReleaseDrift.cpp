// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ReleaseDrift.hpp>

#include <array>
#include <compare>

namespace vthost::client
{

namespace
{
    /// The wording per DriftKind, indexed by it; `{d}` is the daemon's release, `{c}` the client's.
    constexpr auto DriftTexts = std::array<std::string_view, 5> {
        "",                                                    // None
        "daemon {d} is older than this client",                // DaemonOlder
        "daemon {d} is newer than this client",                // DaemonNewer
        "daemon {d} differs from this client ({c})",           // Differs
        "daemon of an earlier release than this client ({c})", // DaemonUnknown
    };

    /// The cost of a restart per LayoutAfterRestart, indexed by it.
    constexpr auto ConfirmationTexts = std::array<std::string_view, 2> {
        "Restarting the daemon on {c} ends every shell it runs. Your workspaces, tabs and scrollback are not "
        "kept; a fresh shell opens.",
        "Restarting the daemon on {c} ends the programs running in it. Your workspaces, tabs, directories "
        "and "
        "scrollback come back.",
    };

    [[nodiscard]] std::string fill(std::string_view pattern, std::string_view daemon, std::string_view client)
    {
        auto result = std::string {};
        while (!pattern.empty())
        {
            if (pattern.starts_with("{d}"))
            {
                result += daemon;
                pattern.remove_prefix(3);
            }
            else if (pattern.starts_with("{c}"))
            {
                result += client;
                pattern.remove_prefix(3);
            }
            else
            {
                result += pattern.front();
                pattern.remove_prefix(1);
            }
        }
        return result;
    }
} // namespace

DriftKind compareReleases(std::string_view client, std::string_view daemon)
{
    if (daemon.empty())
        return DriftKind::DaemonUnknown;
    if (client == daemon)
        return DriftKind::None;
    auto const order = compareReleaseNumbers(daemon, client);
    if (!order)
        return DriftKind::Differs;
    if (std::is_lt(*order))
        return DriftKind::DaemonOlder;
    if (std::is_gt(*order))
        return DriftKind::DaemonNewer;
    return DriftKind::Differs;
}

LayoutAfterRestart layoutSurvivesRestart(proto::Negotiated const& negotiated) noexcept
{
    // What the daemon offered, not what both share: the restore is the daemon's alone, and a client needs
    // no capability of its own to get its workspaces back.
    return negotiated.peerCapabilities.contains(proto::Capability::WorkspaceRestore)
               ? LayoutAfterRestart::Restored
               : LayoutAfterRestart::Lost;
}

DriftNotice assessDrift(std::string_view clientRelease,
                        std::string_view daemonRelease,
                        LayoutAfterRestart layout)
{
    auto const kind = compareReleases(clientRelease, daemonRelease);
    return DriftNotice {
        .kind = kind,
        .text = fill(DriftTexts.at(static_cast<size_t>(kind)), daemonRelease, clientRelease),
        .confirmation = fill(ConfirmationTexts.at(static_cast<size_t>(layout)), daemonRelease, clientRelease),
    };
}

} // namespace vthost::client
