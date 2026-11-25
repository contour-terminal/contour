// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The ways the hostile extension host misbehaves, one row each: what the crash and hostility tests start in
/// place of the real one, to prove Contour survives a host that does not play by the rules. Never installed.
///
/// A row whose misbehaviour is an incident runs the real host behind a relay: every channel Contour handed
/// over, both ways, is split into frames (proto::frameSizeOf()) and forwarded as it came, until the first
/// frame the row's trigger names -- once per process, so once per incarnation. There the row's action
/// happens in place of, or after, forwarding it. Adding a misbehaviour is adding a row; only a new kind of
/// action needs code (HostileHost.cpp).

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace contour::extension_host::testing
{

namespace proto = vtextension::wasm::remote::proto;

/// The wake signal the real host listens on.
enum class WakeSource : uint8_t
{
    Contour, ///< The one Contour handed over: an interrupt reaches the host.
    Silent,  ///< One of its own that nobody signals: the host never reads the interrupt page.
};

/// What the host does once it ended because Contour's channels did.
enum class AfterContourEnds : uint8_t
{
    Exit,             ///< Exits, as the real host does.
    SleepUntilKilled, ///< Neither exits nor heeds SIGTERM: it sleeps until it is killed.
};

/// What the hostile host does at the frame its trigger names.
enum class HostileAction : uint8_t
{
    Abort,                   ///< std::abort(): the host crashes.
    Segfault,                ///< Dies as a segmentation fault does, whatever handler a sanitizer installed.
    ForwardThenExit,         ///< Forwards the frame, then exits at once with code 3.
    StopReadingChannel,      ///< Reads nothing more of that channel, the frame included; stays alive.
    StopReadingEveryChannel, ///< Reads nothing more of any of Contour's channels; stays alive.
    AnswerGarbage,           ///< Swallows the frame and answers 64 pseudo-random bytes on its channel.
    AnswerOverCapMemory,     ///< Swallows a MemoryRead and answers a MemoryData header past the size cap.
    AnswerShortMemory,       ///< Swallows a MemoryRead and answers a well-formed MemoryData one byte short.
    AnswerHalfMemory,        ///< Swallows a MemoryRead and answers a MemoryData header and half its body.
    HoldBack,                ///< Holds the frame, and every one after it that way, back forever.
};

/// The frame an incident happens at: the first one of `kind` travelling `direction` on a channel of kind
/// `channel`, once a frame of kind `after` passed on any channel either way.
struct HostileTrigger
{
    proto::Direction direction; ///< The way the frame travels.
    proto::ChannelKind channel; ///< The kind of channel it travels on.
    proto::PduKind kind;        ///< Its kind.
    std::optional<proto::PduKind>
        after {}; ///< Only once a frame of this kind passed; nothing: from the start.
};

/// One thing the host does wrong, once.
struct HostileIncident
{
    HostileTrigger trigger; ///< Where it happens.
    HostileAction action;   ///< What happens there.
};

/// One way the hostile host misbehaves, and its name on the command line.
struct HostileModeRow
{
    std::string_view name;                              ///< What `--hostile=<name>` names it.
    WakeSource wake = WakeSource::Contour;              ///< The wake signal the host listens on.
    AfterContourEnds afterEnd = AfterContourEnds::Exit; ///< What it does once Contour's channels ended.
    std::optional<HostileIncident> incident {}; ///< What it does wrong in the protocol; nothing to relay.
};

namespace detail
{
    /// @return The trigger of a frame of @p kind, sent by Contour on a lane.
    [[nodiscard]] constexpr HostileTrigger contourOnLane(proto::PduKind kind) noexcept
    {
        return HostileTrigger { .direction = proto::Direction::ToHelper,
                                .channel = proto::ChannelKind::Lane,
                                .kind = kind };
    }

    /// @return The trigger of a frame of @p kind, sent by the host on the control channel.
    [[nodiscard]] constexpr HostileTrigger hostOnControl(proto::PduKind kind) noexcept
    {
        return HostileTrigger { .direction = proto::Direction::ToParent,
                                .channel = proto::ChannelKind::Control,
                                .kind = kind };
    }
} // namespace detail

/// Every way the hostile host misbehaves.
inline constexpr auto HostileModes = std::array {
    HostileModeRow { .name = "ignore-interrupt", .wake = WakeSource::Silent },
    HostileModeRow { .name = "ignore-eof", .afterEnd = AfterContourEnds::SleepUntilKilled },
    HostileModeRow {
        .name = "stop-reading-after-instantiate",
        .incident = HostileIncident { .trigger = HostileTrigger { .direction = proto::Direction::ToHelper,
                                                                  .channel = proto::ChannelKind::Control,
                                                                  .kind = proto::PduKind::Load,
                                                                  .after = proto::PduKind::Instantiated },
                                      .action = HostileAction::StopReadingChannel } },
    HostileModeRow { .name = "abort",
                     .incident = HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::Call),
                                                   .action = HostileAction::Abort } },
    HostileModeRow { .name = "segfault",
                     .incident = HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::Call),
                                                   .action = HostileAction::Segfault } },
    HostileModeRow {
        .name = "exit-in-callhost",
        .incident = HostileIncident { .trigger = HostileTrigger { .direction = proto::Direction::ToParent,
                                                                  .channel = proto::ChannelKind::Lane,
                                                                  .kind = proto::PduKind::CallHost },
                                      .action = HostileAction::ForwardThenExit } },
    HostileModeRow { .name = "stop-reading",
                     .incident = HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::Call),
                                                   .action = HostileAction::StopReadingEveryChannel } },
    HostileModeRow { .name = "garbage",
                     .incident = HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::Call),
                                                   .action = HostileAction::AnswerGarbage } },
    HostileModeRow { .name = "over-cap",
                     .incident =
                         HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::MemoryRead),
                                           .action = HostileAction::AnswerOverCapMemory } },
    HostileModeRow { .name = "bogus-size",
                     .incident =
                         HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::MemoryRead),
                                           .action = HostileAction::AnswerShortMemory } },
    HostileModeRow { .name = "stall-after-header",
                     .incident =
                         HostileIncident { .trigger = detail::contourOnLane(proto::PduKind::MemoryRead),
                                           .action = HostileAction::AnswerHalfMemory } },
    HostileModeRow { .name = "hang-at-handshake",
                     .incident = HostileIncident { .trigger = detail::hostOnControl(proto::PduKind::Hello),
                                                   .action = HostileAction::HoldBack } },
    HostileModeRow { .name = "hang-at-load",
                     .incident = HostileIncident { .trigger = detail::hostOnControl(proto::PduKind::Loaded),
                                                   .action = HostileAction::HoldBack } },
    HostileModeRow { .name = "hang-at-instantiate",
                     .incident =
                         HostileIncident { .trigger = detail::hostOnControl(proto::PduKind::Instantiated),
                                           .action = HostileAction::HoldBack } },
};

/// The only argument the hostile host takes beyond the real one's. Without it, it serves as the real host
/// does: how a test makes the host started after the first one behave.
inline constexpr auto HostileOption = std::string_view { "--hostile=" };

/// @param name A mode's name.
/// @return The mode of that name; nothing if there is none.
[[nodiscard]] constexpr HostileModeRow const* hostileModeNamed(std::string_view name) noexcept
{
    auto const row = std::ranges::find(HostileModes, name, &HostileModeRow::name);
    return row != HostileModes.end() ? &*row : nullptr;
}

} // namespace contour::extension_host::testing
