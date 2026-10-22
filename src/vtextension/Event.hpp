// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Flags.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace vtextension
{

/// The kinds of event the host delivers to an extension, as bits of one EventKinds set.
///
/// The first eight are the statusline world's `event-kinds` flags, bit for bit, in their
/// declaration order: the set an extension's `load` returns is these bits. The last three have no
/// flag in the world because they are always delivered.
enum class EventKind : uint16_t
{
    Title = 1U << 0U,             ///< The pane's or window's title changed.
    Cwd = 1U << 1U,               ///< The working directory changed.
    ForegroundProcess = 1U << 2U, ///< The pane's foreground process changed.
    InputMode = 1U << 3U,         ///< The input mode changed.
    Tabs = 1U << 4U,              ///< The set of tabs, or the active one, changed.
    UserVar = 1U << 5U,           ///< A program set a user variable.
    Progress = 1U << 6U,          ///< The progress indicator changed.
    Focus = 1U << 7U,             ///< The pane gained or lost focus.
    Font = 1U << 8U,              ///< The font's size, or the size of a cell in pixels, changed.
    Resized = 1U << 9U,           ///< The extension's own surface changed size. Always delivered.
    Timer = 1U << 10U,            ///< A frame the extension requested is due. Always delivered.
    Reply = 1U << 11U, ///< The terminal answered a query in the extension's output. Always delivered.
};

/// A set of event kinds.
using EventKinds = core::Flags<EventKind>;

/// The event kinds every extension receives, whatever it subscribed to.
inline constexpr auto AlwaysDelivered = EventKinds { EventKind::Resized, EventKind::Timer, EventKind::Reply };

/// The event kinds an extension may subscribe to: those the world's `event-kinds` flags carry.
inline constexpr auto SubscribableKinds =
    EventKinds::fromValue(static_cast<uint16_t>((std::to_underlying(EventKind::Font) * 2U) - 1U));

/// What an extension answers to an event: whether the host should call `render` next; the world's
/// `render-request`.
enum class RenderRequest : uint8_t
{
    None,   ///< The event changed nothing the extension shows.
    Render, ///< The extension's output is stale.
};

/// The input mode of the terminal's pane; the world's `input-mode`.
enum class InputMode : uint8_t
{
    Insert,
    Normal,
    Visual,
    VisualLine,
    VisualBlock,
    Hint,
};

/// The state of a progress indicator; the world's `progress-state`.
enum class ProgressState : uint8_t
{
    Hidden,        ///< No progress is shown.
    Normal,        ///< Under way; the percentage says how far.
    Error,         ///< The tracked operation failed.
    Indeterminate, ///< Under way, extent unknown.
    Warning,       ///< The tracked operation needs attention.
};

/// A progress indicator as the running program reports it; the world's `progress`.
struct Progress
{
    ProgressState state = ProgressState::Hidden; ///< What the indicator shows.
    uint8_t percent = 0;                         ///< 0 to 100; meaningful for Normal, Error and Warning.

    [[nodiscard]] bool operator==(Progress const&) const noexcept = default;
};

/// Whether a tab is the window's active one (the world's `tab.active`, a bool on the wire).
enum class TabActivity : uint8_t
{
    Inactive,
    Active,
};

/// One tab of the window the pane lives in; the world's `tab`.
struct Tab
{
    std::string title;                            ///< The tab's title.
    TabActivity activity = TabActivity::Inactive; ///< Whether it is the active tab.

    [[nodiscard]] bool operator==(Tab const&) const noexcept = default;
};

/// Whether the pane has the input focus (the world's `focus` event and `focused`, a bool on the wire).
enum class FocusState : uint8_t
{
    Unfocused,
    Focused,
};

/// The font the terminal's cells are drawn in; the world's `font-metrics`.
struct FontMetrics
{
    float pointSize = 0;     ///< The font's size in points; 0 where the host draws in no font of its own.
    uint32_t cellWidth = 0;  ///< One cell's width in device pixels; 0 while unknown.
    uint32_t cellHeight = 0; ///< One cell's height in device pixels; 0 while unknown.

    [[nodiscard]] bool operator==(FontMetrics const&) const = default;
};

/// The size of the extension's own surface, in character cells; the world's `size`.
struct SurfaceSize
{
    uint32_t cols = 0; ///< Columns.
    uint32_t rows = 0; ///< Rows.

    [[nodiscard]] bool operator==(SurfaceSize const&) const noexcept = default;
};

/// Bytes with no encoding implied: the world's `list<u8>`.
using ByteList = std::vector<uint8_t>;

/// The title of the pane or window changed.
struct TitleChanged
{
    std::string title; ///< The new title.

    [[nodiscard]] bool operator==(TitleChanged const&) const = default;
};

/// The working directory changed.
struct CwdChanged
{
    std::string cwd; ///< The new working directory.

    [[nodiscard]] bool operator==(CwdChanged const&) const = default;
};

/// The pane's foreground process changed.
struct ForegroundProcessChanged
{
    std::string name; ///< The process's name.

    [[nodiscard]] bool operator==(ForegroundProcessChanged const&) const = default;
};

/// The input mode changed.
struct InputModeChanged
{
    InputMode mode = InputMode::Insert; ///< The new mode.

    [[nodiscard]] bool operator==(InputModeChanged const&) const = default;
};

/// The set of tabs, or which one is active, changed.
struct TabsChanged
{
    std::vector<Tab> tabs; ///< Every tab of the window, in order.

    [[nodiscard]] bool operator==(TabsChanged const&) const = default;
};

/// A program set a user variable.
struct UserVarChanged
{
    std::string name;  ///< The variable's name.
    std::string value; ///< Its new value.

    [[nodiscard]] bool operator==(UserVarChanged const&) const = default;
};

/// The progress indicator changed.
struct ProgressChanged
{
    Progress progress; ///< The new indicator.

    [[nodiscard]] bool operator==(ProgressChanged const&) const = default;
};

/// The pane gained or lost focus.
struct FocusChanged
{
    FocusState state = FocusState::Unfocused; ///< The new focus state.

    [[nodiscard]] bool operator==(FocusChanged const&) const = default;
};

/// The font's size, or the size of a cell in pixels, changed.
struct FontChanged
{
    FontMetrics metrics; ///< The new metrics.

    [[nodiscard]] bool operator==(FontChanged const&) const = default;
};

/// The extension's own surface changed size.
struct Resized
{
    SurfaceSize size; ///< The new size.

    [[nodiscard]] bool operator==(Resized const&) const = default;
};

/// The delay the extension asked for with `request-frame` has elapsed.
struct TimerFired
{
    [[nodiscard]] bool operator==(TimerFired const&) const = default;
};

/// The terminal's answer to a query the extension wrote in its own previous output.
struct ReplyBytes
{
    ByteList bytes; ///< The answer, as the terminal sent it.

    [[nodiscard]] bool operator==(ReplyBytes const&) const = default;
};

/// Something the host tells an extension about: the world's `event`. The alternatives are in the
/// order of the world's cases, which is the discriminant the guest receives.
using Event = std::variant<TitleChanged,
                           CwdChanged,
                           ForegroundProcessChanged,
                           InputModeChanged,
                           TabsChanged,
                           UserVarChanged,
                           ProgressChanged,
                           FocusChanged,
                           FontChanged,
                           Resized,
                           TimerFired,
                           ReplyBytes>;

/// The kind of each Event alternative, indexed by the alternative's index.
inline constexpr auto EventKindsByAlternative = std::array {
    EventKind::Title,     EventKind::Cwd,   EventKind::ForegroundProcess,
    EventKind::InputMode, EventKind::Tabs,  EventKind::UserVar,
    EventKind::Progress,  EventKind::Focus, EventKind::Font,
    EventKind::Resized,   EventKind::Timer, EventKind::Reply,
};

static_assert(EventKindsByAlternative.size() == std::variant_size_v<Event>,
              "every Event alternative has a kind");

/// @param event An event.
/// @return The kind of @p event.
[[nodiscard]] constexpr EventKind kindOf(Event const& event) noexcept
{
    return EventKindsByAlternative.at(event.index());
}

/// Whether a pending event of @p kind may be replaced by a newer one of the same kind before it is
/// delivered. State kinds coalesce: the extension wants the latest value, not every intermediate
/// one. A reply does not: each answers its own query. A user variable coalesces per variable name
/// only, which the queue does by key, so the kind as a whole does not.
/// @param kind An event kind.
/// @return Whether a newer event of @p kind supersedes a pending one.
[[nodiscard]] constexpr bool coalesces(EventKind kind) noexcept
{
    return kind != EventKind::Reply && kind != EventKind::UserVar;
}

} // namespace vtextension
