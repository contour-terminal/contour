// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlError.hpp>

namespace vtcontrol
{

/// A command block finished.
struct BlockFinishedEvent
{
    using ApiRecordTag = void;

    BlockInfo block {}; ///< The block.
};

/// A pane rang the bell.
struct BellEvent
{
    using ApiRecordTag = void;
};

/// A pane's title changed.
struct TitleChangedEvent
{
    using ApiRecordTag = void;

    std::string title {}; ///< The new title.
};

/// A pane's working directory changed.
struct CwdChangedEvent
{
    using ApiRecordTag = void;

    std::string cwd {}; ///< The new working directory.
};

/// A pane's program-status records changed.
struct ProgramStatusChangedEvent
{
    using ApiRecordTag = void;

    std::vector<ProgramStatusInfo> records {};                ///< The pane's records now.
    ProgramStatusReason reason = ProgramStatusReason::Report; ///< Why they changed.
};

/// A pane was created.
struct PaneCreatedEvent
{
    using ApiRecordTag = void;
};

/// A pane was closed.
struct PaneClosedEvent
{
    using ApiRecordTag = void;
};

/// A tab was created.
struct TabCreatedEvent
{
    using ApiRecordTag = void;
};

/// A tab was closed.
struct TabClosedEvent
{
    using ApiRecordTag = void;
};

/// A GUI client attached to the daemon.
struct ClientAttachedEvent
{
    using ApiRecordTag = void;

    std::string client {}; ///< A description of the client.
};

/// A GUI client detached from the daemon.
struct ClientDetachedEvent
{
    using ApiRecordTag = void;

    std::string client {}; ///< A description of the client.
};

/// The subscriber was too slow and events were dropped.
struct EventsDroppedEvent
{
    using ApiRecordTag = void;

    uint32_t count = 0; ///< How many events were dropped.
};

/// Every event payload, in catalog order.
using EventPayload = std::variant<BlockFinishedEvent,
                                  BellEvent,
                                  TitleChangedEvent,
                                  CwdChangedEvent,
                                  ProgramStatusChangedEvent,
                                  PaneCreatedEvent,
                                  PaneClosedEvent,
                                  TabCreatedEvent,
                                  TabClosedEvent,
                                  ClientAttachedEvent,
                                  ClientDetachedEvent,
                                  EventsDroppedEvent>;

/// The objects an event is about; empty where an event has no such object.
struct EventOrigin
{
    using ApiRecordTag = void;

    std::string window {}; ///< The window's id.
    std::string tab {};    ///< The tab's id.
    std::string pane {};   ///< The pane's id.
};

/// An event as it travels.
struct Event
{
    using ApiRecordTag = void;

    EventOrigin origin {};   ///< What it is about.
    EventPayload payload {}; ///< What happened.
};

/// The envelope of an event on the wire: `type` selects the payload, which stays raw until then.
struct EventEnvelope
{
    using ApiRecordTag = void;

    std::string type {};   ///< The catalog name of the payload.
    std::string window {}; ///< The window's id.
    std::string tab {};    ///< The tab's id.
    std::string pane {};   ///< The pane's id.
    RawJson payload {};    ///< The payload's JSON.
};

/// A row of the event catalog: the wire name, summary and required permission of one payload type.
/// @tparam P The payload type.
template <typename P>
struct EventRow
{
    std::string_view name {};                 ///< The wire name, e.g. `block.finished`.
    std::string_view summary {};              ///< A one-line summary.
    Permission permission = Permission::Read; ///< What a subscriber must be granted to be handed it.
};

/// Every event, once, in the order of `EventPayload`.
inline constexpr auto EventCatalog = std::tuple {
    EventRow<BlockFinishedEvent> { "block.finished", "A command block finished.", Permission::Screen },
    EventRow<BellEvent> { "pane.bell", "A pane rang the bell.", Permission::Read },
    EventRow<TitleChangedEvent> { "pane.titleChanged", "A pane's title changed.", Permission::Read },
    EventRow<CwdChangedEvent> { "pane.cwdChanged", "A pane's working directory changed.", Permission::Read },
    EventRow<ProgramStatusChangedEvent> {
        "pane.programStatusChanged", "A pane's program status changed.", Permission::Read },
    EventRow<PaneCreatedEvent> { "pane.created", "A pane was created.", Permission::Read },
    EventRow<PaneClosedEvent> { "pane.closed", "A pane was closed.", Permission::Read },
    EventRow<TabCreatedEvent> { "tab.created", "A tab was created.", Permission::Read },
    EventRow<TabClosedEvent> { "tab.closed", "A tab was closed.", Permission::Read },
    EventRow<ClientAttachedEvent> { "client.attached", "A GUI client attached.", Permission::Read },
    EventRow<ClientDetachedEvent> { "client.detached", "A GUI client detached.", Permission::Read },
    EventRow<EventsDroppedEvent> {
        "events.dropped", "Events were dropped because the subscriber was too slow.", Permission::Read },
};

/// @param name An event's catalog name.
/// @return What a subscriber must be granted to be handed it, or nullopt when no event has that name.
[[nodiscard]] std::optional<Permission> eventPermission(std::string_view name) noexcept;

/// @param payload An event payload.
/// @return Its catalog name.
[[nodiscard]] std::string_view eventName(EventPayload const& payload);

/// @param event An event.
/// @return `{"type":...,"window":...,"tab":...,"pane":...,"payload":{...}}`, `type` first.
[[nodiscard]] std::string encodeEvent(Event const& event);

/// @param text The JSON of one event.
/// @return The event, or `BadRequest` for a malformed envelope or an unknown `type`.
[[nodiscard]] std::expected<Event, ControlError> decodeEvent(std::string_view text);

} // namespace vtcontrol
