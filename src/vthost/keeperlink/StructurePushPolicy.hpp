// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// When a change to the daemon's structure is pushed to its PTY keeper: a table, by the change's kind.
///
/// An id a client may see is in the keeper's structure before the client hears of it, so that a daemon
/// restarted after a crash serves every id a client holds. A cosmetic change -- a ratio dragged, a title
/// typed -- waits a moment, so that a drag costs one push rather than one per step; a crash in that moment
/// costs a ratio or a title, never a binding.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ranges>
#include <utility>

namespace vthost::keeperlink
{

/// A change to what the structure holds, by kind.
enum class StructureEvent : uint8_t
{
    TabAdded = 0,
    TabClosed,
    TabMoved,
    TabMovedToWorkspace,
    ActiveTabChanged,
    PaneSplit,
    PaneClosed,
    ActivePaneChanged,
    PaneRatioChanged,
    PaneOrientationChanged,
    PaneSwapped,
    PaneSessionReplaced,
    PaneZoomChanged,
    PaneTreeRestructured,
    TabTitleChanged,
    TabColorChanged,
    SessionCreated,
    SessionClosed,
    SettingsChanged,
    ColorPreferenceChanged,
    InterruptionChanged,
};

/// When a change is pushed.
enum class PushTiming : uint8_t
{
    Synchronous = 0, ///< At once, before any client hears of it: queued on the KeeperLink by then.
    Debounced,       ///< Once no change of the kind came for StructureDebounce.
};

/// How long a debounced change waits for the next.
inline constexpr auto StructureDebounce = std::chrono::seconds { 1 };

/// One row of the push table.
struct StructurePush
{
    StructureEvent event;
    PushTiming timing;
};

/// Every structure event, with when it is pushed; one row each, in the enum's order.
inline constexpr auto StructurePushTable = std::array {
    StructurePush { .event = StructureEvent::TabAdded, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::TabClosed, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::TabMoved, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::TabMovedToWorkspace, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::ActiveTabChanged, .timing = PushTiming::Debounced },
    StructurePush { .event = StructureEvent::PaneSplit, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::PaneClosed, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::ActivePaneChanged, .timing = PushTiming::Debounced },
    StructurePush { .event = StructureEvent::PaneRatioChanged, .timing = PushTiming::Debounced },
    StructurePush { .event = StructureEvent::PaneOrientationChanged, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::PaneSwapped, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::PaneSessionReplaced, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::PaneZoomChanged, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::PaneTreeRestructured, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::TabTitleChanged, .timing = PushTiming::Debounced },
    StructurePush { .event = StructureEvent::TabColorChanged, .timing = PushTiming::Debounced },
    StructurePush { .event = StructureEvent::SessionCreated, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::SessionClosed, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::SettingsChanged, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::ColorPreferenceChanged, .timing = PushTiming::Synchronous },
    StructurePush { .event = StructureEvent::InterruptionChanged, .timing = PushTiming::Synchronous },
};

/// How many structure events there are: one past the last.
inline constexpr auto StructureEventCount =
    std::size_t { std::to_underlying(StructureEvent::InterruptionChanged) } + 1;

static_assert(StructurePushTable.size() == StructureEventCount, "every structure event has one row");
static_assert(
    [] {
        for (auto const index: std::views::iota(std::size_t { 0 }, StructurePushTable.size()))
            if (std::to_underlying(StructurePushTable[index].event) != index)
                return false;
        return true;
    }(),
    "the rows are in the enum's order");

/// @param event A change to the structure.
/// @return When it is pushed.
[[nodiscard]] constexpr PushTiming pushTimingOf(StructureEvent event) noexcept
{
    return StructurePushTable[std::to_underlying(event)].timing;
}

} // namespace vthost::keeperlink
