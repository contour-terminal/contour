// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Version-range and capability negotiation of the native protocol. Pure and dependency-free: the
/// hello codec and the handshakes are built on it.

#include <array>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace vthost::proto
{

/// An inclusive range of protocol versions a peer speaks.
struct VersionRange
{
    uint32_t min = 0; ///< Lowest version spoken.
    uint32_t max = 0; ///< Highest version spoken.

    bool operator==(VersionRange const&) const = default;
};

/// The `u32` every negotiating hello leads with. A hello leading with any other value is decoded as a
/// pre-negotiation codec's: as far as that value, into the range of it alone (@see isPreNegotiation for
/// which such ranges predate negotiation; a larger one is simply a range this build does not speak).
constexpr uint32_t NegotiatingHelloMarker = 4;

/// The version range this build speaks.
constexpr VersionRange SupportedVersions { .min = 4, .max = 4 };

/// An optional feature a peer may offer on top of the negotiated version.
enum class Capability : uint8_t
{
    ExtensionPermissions = 1,
    DaemonControl = 2, ///< The peer serves DaemonStatusRequest and StopDaemon.
    /// The peer spawns the first session at the ClientHello's client area. Informational only: the field
    /// travels in the ClientHello, before the ServerHello says what the daemon offers, so neither end can
    /// gate on it. A daemon applies a present client area regardless; a client sends it regardless. Do not
    /// make either end require it -- that breaks every peer from before it.
    InitialClientArea = 3,
    AttachIntent = 4,  ///< The peer honours the ClientHello's attach intent (which windows to follow).
    ProfileByName = 5, ///< The peer resolves the profile name a hello or spawning verb carries.
    TabEdits = 6,      ///< The peer serves EditTab.
    Heartbeat = 7,     ///< The peer echoes Heartbeat.
    /// The peer keeps its workspace across a restart: it snapshots its windows, tabs and panes, restores
    /// them at its next start, and honours StopReason::UserKeepingWorkspace. A daemon offers it while its
    /// persistence is on.
    WorkspaceRestore = 8,
    ControlApi = 9,     ///< The peer serves ControlRequest, ControlReply and ControlEvent.
    ProgramStatus = 10, ///< The peer pushes, or mirrors, a session's OSC 7501 records (PDU ProgramStatus).
    /// The peer shows a PermissionRequest's control permissions in its prompt. A daemon sends a question
    /// that asks for any only to a client sharing it: one that would prompt without them would have its user
    /// grant what they were never shown.
    ControlPermissions = 11,
};

/// One row of the table naming a Capability.
struct CapabilityRow
{
    Capability capability;
    std::string_view name;
};

/// Every known Capability, with its name.
constexpr std::array CapabilityTable {
    CapabilityRow { .capability = Capability::ExtensionPermissions, .name = "extension-permissions" },
    CapabilityRow { .capability = Capability::DaemonControl, .name = "daemon-control" },
    CapabilityRow { .capability = Capability::InitialClientArea, .name = "initial-client-area" },
    CapabilityRow { .capability = Capability::AttachIntent, .name = "attach-intent" },
    CapabilityRow { .capability = Capability::ProfileByName, .name = "profile-by-name" },
    CapabilityRow { .capability = Capability::TabEdits, .name = "tab-edits" },
    CapabilityRow { .capability = Capability::Heartbeat, .name = "heartbeat" },
    CapabilityRow { .capability = Capability::WorkspaceRestore, .name = "workspace-restore" },
    CapabilityRow { .capability = Capability::ControlApi, .name = "control-api" },
    CapabilityRow { .capability = Capability::ProgramStatus, .name = "program-status" },
    CapabilityRow { .capability = Capability::ControlPermissions, .name = "control-permissions" },
};

/// A set of capability idents. Raw idents, so a newer peer's unknown ones survive decoding and tracing.
struct CapabilitySet
{
    std::vector<uint64_t> idents; ///< The idents, as received or built.

    /// Tells whether @p capability is in the set.
    /// @param capability The capability to look for.
    /// @return true if its ident is present.
    [[nodiscard]] bool contains(Capability capability) const noexcept;

    /// Builds a set from known capabilities.
    /// @param capabilities The capabilities to include.
    /// @return The set.
    [[nodiscard]] static CapabilitySet of(std::initializer_list<Capability> capabilities);

    bool operator==(CapabilitySet const&) const = default;
};

/// Intersects two sets.
/// @param a First set.
/// @param b Second set.
/// @return The common idents, sorted and de-duplicated.
[[nodiscard]] CapabilitySet intersect(CapabilitySet const& a, CapabilitySet const& b);

/// The outcome of a successful handshake.
struct Negotiated
{
    uint32_t version = 0;           ///< The agreed protocol version.
    CapabilitySet peerCapabilities; ///< What the peer offered.
    CapabilitySet shared;           ///< What both offered.
};

/// A refused handshake: both ranges, so either end can name them. A pre-negotiation peer of codec `v`
/// is `theirs = { v, v }`.
struct HandshakeRefusal
{
    VersionRange ours;   ///< The range of the end reporting.
    VersionRange theirs; ///< The range of the peer.

    bool operator==(HandshakeRefusal const&) const = default;
};

/// Tells whether a range belongs to a peer that predates version negotiation.
/// @param range The peer's range.
/// @return true if `range.max < NegotiatingHelloMarker`.
[[nodiscard]] bool isPreNegotiation(VersionRange range) noexcept;

/// Picks the highest version both ranges contain.
/// @param ours Our range.
/// @param theirs The peer's range.
/// @return The version, or the refusal naming both ranges (disjoint, malformed, or pre-negotiation).
[[nodiscard]] std::expected<uint32_t, HandshakeRefusal> negotiate(VersionRange ours, VersionRange theirs);

/// Which end is being described.
enum class PeerRole : uint8_t
{
    Daemon, ///< The peer is the daemon; the text is for the client's user.
    Client, ///< The peer is the client; the text is for the daemon's log.
};

/// Phrases a refusal for a human. When the ranges overlap, the refusal was not about the version.
/// @param refusal The refusal.
/// @param peer The role of the peer that refused or was refused.
/// @return The text.
[[nodiscard]] std::string describe(HandshakeRefusal const& refusal, PeerRole peer);

} // namespace vthost::proto
