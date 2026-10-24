// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/Negotiation.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <ranges>

namespace vthost::proto
{

namespace
{
    bool isWellFormed(VersionRange range) noexcept
    {
        return range.min <= range.max;
    }

    std::string formatRange(VersionRange range)
    {
        return std::format("{}-{}", range.min, range.max);
    }
} // namespace

bool CapabilitySet::contains(Capability capability) const noexcept
{
    return std::ranges::contains(idents, static_cast<uint64_t>(capability));
}

CapabilitySet CapabilitySet::of(std::initializer_list<Capability> capabilities)
{
    auto result = CapabilitySet {};
    for (auto const capability: capabilities)
        result.idents.push_back(static_cast<uint64_t>(capability));
    return result;
}

CapabilitySet intersect(CapabilitySet const& a, CapabilitySet const& b)
{
    auto result = CapabilitySet {};
    for (auto const ident: a.idents)
        if (std::ranges::contains(b.idents, ident))
            result.idents.push_back(ident);
    std::ranges::sort(result.idents);
    auto const duplicates = std::ranges::unique(result.idents);
    result.idents.erase(duplicates.begin(), duplicates.end());
    return result;
}

bool isPreNegotiation(VersionRange range) noexcept
{
    return range.max < NegotiatingHelloMarker;
}

std::expected<uint32_t, HandshakeRefusal> negotiate(VersionRange ours, VersionRange theirs)
{
    auto const refusal = std::unexpected { HandshakeRefusal { .ours = ours, .theirs = theirs } };
    if (!isWellFormed(ours) || !isWellFormed(theirs) || isPreNegotiation(theirs))
        return refusal;
    auto const highest = std::min(ours.max, theirs.max);
    auto const lowest = std::max(ours.min, theirs.min);
    if (highest < lowest)
        return refusal;
    return highest;
}

std::string describe(HandshakeRefusal const& refusal, PeerRole peer)
{
    auto const peerName =
        peer == PeerRole::Daemon ? std::string_view { "daemon" } : std::string_view { "client" };
    auto const ourName =
        peer == PeerRole::Daemon ? std::string_view { "this client" } : std::string_view { "this daemon" };

    if (negotiate(refusal.ours, refusal.theirs))
        return peer == PeerRole::Daemon
                   ? "the daemon refused the connection: wrong token or insufficient rights"
                   : "the client was refused: wrong token or insufficient rights";

    if (isPreNegotiation(refusal.theirs))
        return std::format(
            "the {} speaks protocol {}, from a release before version negotiation; {} speaks {}",
            peerName,
            refusal.theirs.max,
            ourName,
            formatRange(refusal.ours));
    return std::format("the {} speaks protocol {}, {} speaks {}",
                       peerName,
                       formatRange(refusal.theirs),
                       ourName,
                       formatRange(refusal.ours));
}

} // namespace vthost::proto
