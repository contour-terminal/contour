// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/ReleaseDrift.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <compare>
#include <ranges>

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
        "Restarting the daemon on {c} ends every shell it runs. Your windows, tabs and scrollback are not "
        "kept; a fresh shell opens.",
        "Restarting the daemon on {c} ends the programs running in it. Your windows, tabs, directories and "
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

std::optional<std::vector<std::uint32_t>> parseReleaseNumbers(std::string_view release)
{
    auto numbers = std::vector<std::uint32_t> {};
    while (!release.empty() && release.front() >= '0' && release.front() <= '9')
    {
        auto value = std::uint32_t {};
        auto const [end, error] = std::from_chars(release.data(), release.data() + release.size(), value);
        if (error != std::errc {})
            return std::nullopt;
        numbers.push_back(value);
        release.remove_prefix(static_cast<size_t>(end - release.data()));
        if (release.size() >= 2 && release.front() == '.' && release[1] >= '0' && release[1] <= '9')
            release.remove_prefix(1);
        else
            break;
    }
    if (numbers.empty())
        return std::nullopt;
    return numbers;
}

DriftKind compareReleases(std::string_view client, std::string_view daemon)
{
    if (daemon.empty())
        return DriftKind::DaemonUnknown;
    if (client == daemon)
        return DriftKind::None;
    auto const clientNumbers = parseReleaseNumbers(client);
    auto const daemonNumbers = parseReleaseNumbers(daemon);
    if (!clientNumbers || !daemonNumbers)
        return DriftKind::Differs;
    auto const width = std::max(clientNumbers->size(), daemonNumbers->size());
    for (auto const index: std::views::iota(size_t { 0 }, width))
    {
        auto const c = index < clientNumbers->size() ? (*clientNumbers)[index] : std::uint32_t { 0 };
        auto const d = index < daemonNumbers->size() ? (*daemonNumbers)[index] : std::uint32_t { 0 };
        if (d < c)
            return DriftKind::DaemonOlder;
        if (d > c)
            return DriftKind::DaemonNewer;
    }
    return DriftKind::Differs;
}

LayoutAfterRestart layoutSurvivesRestart(proto::Negotiated const& /*negotiated*/) noexcept
{
    return LayoutAfterRestart::Lost;
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
