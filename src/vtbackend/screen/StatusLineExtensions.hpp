// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Image.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/vt/ProgressState.hpp>

#include <core/Flags.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace vtbackend
{

/// One published frame of a status-line extension: what its private terminal showed after a render.
///
/// Immutable once published. The worker that rendered it builds it, the terminal swaps the latest one in
/// and reads it while it draws, so neither waits for the other.
struct StatusLineExtensionSnapshot
{
    /// The surface's rows, top first, copied from the extension's private grid. Their images are detached
    /// from it (@see detachedImage): same data, no remover, so the frame may outlive that terminal. What
    /// the take-over placement draws, after re-creating the images in its own pool.
    std::vector<Line> lines;

    /// The top row re-serialized as UTF-8 text with interleaved SGR, image cells dropped and trailing
    /// default cells trimmed. What the extend placement splices into the indicator status line, which
    /// carries text and SGR only.
    std::string sgrText;

    /// Which render this frame came from, counting up per extension instance, so a consumer can tell a
    /// new frame from one it already drew.
    uint64_t generation = 0;
};

/// The kinds of terminal state a status-line extension may follow, as bits of one StatusLineEventKinds.
///
/// The terminal's mirror of `vtextension::EventKind`, bit for bit (vtbackend cannot include vtextension):
/// the statusline world's `event-kinds` flags in their declaration order. Narrower than EventKind, whose
/// further bits are the kinds every extension receives anyway, which a terminal never reports.
enum class StatusLineEventKind : uint16_t
{
    Title = 1U << 0U,             ///< The window title changed.
    Cwd = 1U << 1U,               ///< The working directory changed.
    ForegroundProcess = 1U << 2U, ///< The pane's foreground process changed.
    InputMode = 1U << 3U,         ///< The input mode changed.
    Tabs = 1U << 4U,              ///< The set of tabs, or the active one, changed.
    UserVar = 1U << 5U,           ///< A program set a user variable.
    Progress = 1U << 6U,          ///< The progress indicator changed.
    Focus = 1U << 7U,             ///< The terminal gained or lost focus.
    Font = 1U << 8U,              ///< The font's size, or the size of a cell in pixels, changed.
};

/// A set of status-line event kinds.
using StatusLineEventKinds = core::Flags<StatusLineEventKind>;

/// Every status-line event kind, in bit order.
inline constexpr auto StatusLineEventKindList = std::array {
    StatusLineEventKind::Title,     StatusLineEventKind::Cwd,   StatusLineEventKind::ForegroundProcess,
    StatusLineEventKind::InputMode, StatusLineEventKind::Tabs,  StatusLineEventKind::UserVar,
    StatusLineEventKind::Progress,  StatusLineEventKind::Focus, StatusLineEventKind::Font,
};

/// Whether a tab is the window's active one.
enum class StatusLineTabActivity : uint8_t
{
    Inactive,
    Active,
};

/// One tab of the window the terminal is shown in.
struct StatusLineTab
{
    std::string title;                                                ///< The tab's title.
    StatusLineTabActivity activity = StatusLineTabActivity::Inactive; ///< Whether it is the active one.

    [[nodiscard]] bool operator==(StatusLineTab const&) const = default;
};

/// The font a terminal's cells are drawn in, as far as a status-line extension needs it.
struct StatusLineFontMetrics
{
    /// The font's size in points; 0 where the terminal draws in no font of its own (a daemon session).
    float pointSize = 0;

    /// One cell, in device pixels: the unit a cell is a whole number of. Empty while unknown.
    ImageSize cellPixelSize {};

    [[nodiscard]] bool operator==(StatusLineFontMetrics const&) const = default;
};

/// Whether the terminal has the input focus.
enum class StatusLineFocus : uint8_t
{
    Unfocused,
    Focused,
};

/// The events a terminal hands its status-line extensions, one struct per StatusLineEventKind. Each names
/// its own kind, so the kind of an event is read off its type rather than kept in a second list.
namespace StatusLineEvents
{
    /// The window title changed.
    struct TitleChanged
    {
        static constexpr auto Kind = StatusLineEventKind::Title;
        std::string title; ///< The new title.

        [[nodiscard]] bool operator==(TitleChanged const&) const = default;
    };

    /// The working directory changed (OSC 7).
    struct CwdChanged
    {
        static constexpr auto Kind = StatusLineEventKind::Cwd;
        std::string cwd; ///< The new working directory's path, decoded.

        [[nodiscard]] bool operator==(CwdChanged const&) const = default;
    };

    /// The pane's foreground process changed.
    struct ForegroundProcessChanged
    {
        static constexpr auto Kind = StatusLineEventKind::ForegroundProcess;
        std::string name; ///< The process's name.

        [[nodiscard]] bool operator==(ForegroundProcessChanged const&) const = default;
    };

    /// The input mode changed.
    struct InputModeChanged
    {
        static constexpr auto Kind = StatusLineEventKind::InputMode;
        ViMode mode = ViMode::Insert; ///< The new mode.

        [[nodiscard]] bool operator==(InputModeChanged const&) const = default;
    };

    /// The set of tabs, or which one is active, changed.
    struct TabsChanged
    {
        static constexpr auto Kind = StatusLineEventKind::Tabs;
        std::vector<StatusLineTab> tabs; ///< Every tab of the window, in order.

        [[nodiscard]] bool operator==(TabsChanged const&) const = default;
    };

    /// A program set a user variable (OSC 1337 ; SetUserVar).
    struct UserVarChanged
    {
        static constexpr auto Kind = StatusLineEventKind::UserVar;
        std::string name;  ///< The variable's name.
        std::string value; ///< Its new value, decoded.

        [[nodiscard]] bool operator==(UserVarChanged const&) const = default;
    };

    /// The progress indicator changed (OSC 9 ; 4).
    struct ProgressChanged
    {
        static constexpr auto Kind = StatusLineEventKind::Progress;
        Progress progress; ///< The new indicator.

        [[nodiscard]] bool operator==(ProgressChanged const&) const = default;
    };

    /// The terminal gained or lost focus.
    struct FocusChanged
    {
        static constexpr auto Kind = StatusLineEventKind::Focus;
        StatusLineFocus state = StatusLineFocus::Unfocused; ///< The new focus state.

        [[nodiscard]] bool operator==(FocusChanged const&) const = default;
    };

    /// The font's size, or the size of a cell in pixels, changed: a zoom, a configuration reload, or a
    /// move to a screen of another pixel density.
    struct FontChanged
    {
        static constexpr auto Kind = StatusLineEventKind::Font;
        StatusLineFontMetrics metrics; ///< The new metrics.

        [[nodiscard]] bool operator==(FontChanged const&) const = default;
    };
} // namespace StatusLineEvents

/// Something the terminal tells its status-line extensions about.
using StatusLineEvent = std::variant<StatusLineEvents::TitleChanged,
                                     StatusLineEvents::CwdChanged,
                                     StatusLineEvents::ForegroundProcessChanged,
                                     StatusLineEvents::InputModeChanged,
                                     StatusLineEvents::TabsChanged,
                                     StatusLineEvents::UserVarChanged,
                                     StatusLineEvents::ProgressChanged,
                                     StatusLineEvents::FocusChanged,
                                     StatusLineEvents::FontChanged>;

/// @param event An event.
/// @return The kind of @p event.
[[nodiscard]] constexpr StatusLineEventKind kindOf(StatusLineEvent const& event) noexcept
{
    return std::visit([](auto const& alternative) { return std::decay_t<decltype(alternative)>::Kind; },
                      event);
}

/// The status-line extensions of one terminal, as the terminal sees them: something to tell about state
/// changes, and something to read frames from. vtbackend runs no extension; `vtextension` implements this.
///
/// Every member may be called with the terminal's state lock held, so none may wait for an extension.
class IStatusLineExtensions
{
  public:
    IStatusLineExtensions() = default;
    IStatusLineExtensions(IStatusLineExtensions const&) = delete;
    IStatusLineExtensions(IStatusLineExtensions&&) = delete;
    IStatusLineExtensions& operator=(IStatusLineExtensions const&) = delete;
    IStatusLineExtensions& operator=(IStatusLineExtensions&&) = delete;
    virtual ~IStatusLineExtensions() = default;

    /// @return The event kinds any extension wants delivered.
    [[nodiscard]] virtual StatusLineEventKinds subscriptions() const noexcept = 0;

    /// Hands an event to every extension. Never blocks.
    ///
    /// A terminal posts an event only when some extension subscribed to its kind, except FontChanged,
    /// which it posts to every extension whenever the font changes: each extension's own surface takes
    /// its cell size from it, so that images are cut into cells of the size the terminal draws.
    /// @param event The event.
    virtual void post(StatusLineEvent event) noexcept = 0;

    /// Tells every extension the status line's new size. Never blocks.
    /// @param statusLineSize The status line's size in cells.
    virtual void resize(PageSize statusLineSize) noexcept = 0;

    /// @param name What the configuration calls the extension.
    /// @return Its latest frame; null if there is no such extension or it has not rendered yet. Lock-free.
    [[nodiscard]] virtual std::shared_ptr<StatusLineExtensionSnapshot const> snapshot(
        std::string_view name) const noexcept = 0;

    /// @return The extension that draws the whole indicator status line instead of its template, if any.
    ///         A terminal reads it once, and again only when told the extensions were replaced
    ///         (Terminal::statusLineExtensionsReplaced()).
    [[nodiscard]] virtual std::optional<std::string> takeOverName() const noexcept = 0;
};

/// The terminal's gate in front of its extensions: posts an event only when some extension subscribed to
/// its kind, and builds it only then.
///
/// The subscriptions are cached rather than asked for per event, which would be a virtual call (and the
/// host's walk over its runners) on every title or cwd change. Without extensions they are zero forever.
class StatusLineEventGate
{
  public:
    /// @param extensions Where events go; null for none, which makes every post() one untaken branch.
    explicit StatusLineEventGate(IStatusLineExtensions* extensions) noexcept:
        _extensions { extensions },
        _subscriptions { extensions ? extensions->subscriptions().value()
                                    : StatusLineEventKinds::value_type {} }
    {
    }

    /// @param kind An event kind.
    /// @return Whether an extension wants events of @p kind; one relaxed load.
    [[nodiscard]] bool isSubscribedTo(StatusLineEventKind kind) const noexcept
    {
        return (_subscriptions.load(std::memory_order_relaxed) & std::to_underlying(kind)) != 0;
    }

    /// Posts the event @p make builds, if an extension is subscribed to its kind. Otherwise @p make is not
    /// called, so nothing is built, decoded or allocated.
    /// @param make Builds the event; its result type names the kind.
    template <typename Make>
    void post(Make const& make)
    {
        if (isSubscribedTo(std::invoke_result_t<Make const&>::Kind)) [[unlikely]]
            _extensions->post(make());
    }

    /// Re-reads the subscriptions. May be called from any thread; takes no lock.
    /// @return The kinds subscribed now that were not before: what the extensions have not been told yet.
    StatusLineEventKinds refresh() noexcept
    {
        if (!_extensions)
            return {};
        auto const now = _extensions->subscriptions().value();
        auto const before = _subscriptions.exchange(now, std::memory_order_relaxed);
        return StatusLineEventKinds::fromValue(now & ~before);
    }

    /// @return The kinds subscribed to, as last read.
    [[nodiscard]] StatusLineEventKinds subscriptions() const noexcept
    {
        return StatusLineEventKinds::fromValue(_subscriptions.load(std::memory_order_relaxed));
    }

  private:
    IStatusLineExtensions* _extensions;
    std::atomic<StatusLineEventKinds::value_type> _subscriptions;
};

/// Watches, from the frames status-line extensions publish, whether they follow one event kind: what their
/// host must start or stop doing for them -- polling the foreground process, say -- is known only once they
/// have loaded, which a frame published then tells. Lock-free, so a worker can ask on every frame and hand
/// its host's thread only the changes.
class StatusLineSubscriptionWatch
{
  public:
    /// @param kind The event kind to watch.
    explicit StatusLineSubscriptionWatch(StatusLineEventKind kind) noexcept: _kind { kind } {}

    /// @param subscriptions What the extensions subscribe to now.
    /// @return Whether they now follow the watched kind, or no longer do, since the previous call (or since
    ///         construction, when none is followed). Any thread.
    [[nodiscard]] bool changed(StatusLineEventKinds subscriptions) noexcept
    {
        auto const now = subscriptions.test(_kind) ? Following::Yes : Following::No;
        return _following.exchange(now, std::memory_order_acq_rel) != now;
    }

    /// Forgets what was followed: for extensions that replaced the watched ones, and have not loaded yet, so
    /// that the frame they publish once they follow the kind counts as the change it is. Any thread.
    void reset() noexcept { _following.exchange(Following::No, std::memory_order_acq_rel); }

  private:
    /// Whether the extensions follow the watched kind.
    enum class Following : uint8_t
    {
        No,
        Yes,
    };

    StatusLineEventKind _kind;
    std::atomic<Following> _following = Following::No;
};

/// What a StatusLinePublishSink delivers to: the terminal.
class IStatusLinePublishTarget
{
  public:
    IStatusLinePublishTarget() = default;
    IStatusLinePublishTarget(IStatusLinePublishTarget const&) = delete;
    IStatusLinePublishTarget(IStatusLinePublishTarget&&) = delete;
    IStatusLinePublishTarget& operator=(IStatusLinePublishTarget const&) = delete;
    IStatusLinePublishTarget& operator=(IStatusLinePublishTarget&&) = delete;
    virtual ~IStatusLinePublishTarget() = default;

    /// An extension published a frame or changed its subscriptions. Called on an extension worker; must
    /// take no lock that a thread waiting for that worker may hold.
    virtual void statusLineExtensionsPublished() noexcept = 0;
};

/// The revocable handle the extensions' workers tell of a published frame through.
///
/// The two sides are wired in a cycle: the extensions exist before the terminal they show on, so the
/// terminal attaches itself once it is built (the one attach-style call after construction the cycle
/// needs), and revokes itself when it is destroyed. revoke() waits for a delivery in flight, so none
/// reaches a terminal that is being torn down; a delivery after it reaches nobody. No lock is taken on
/// either side.
class StatusLinePublishSink
{
  public:
    StatusLinePublishSink() = default;
    StatusLinePublishSink(StatusLinePublishSink const&) = delete;
    StatusLinePublishSink(StatusLinePublishSink&&) = delete;
    StatusLinePublishSink& operator=(StatusLinePublishSink const&) = delete;
    StatusLinePublishSink& operator=(StatusLinePublishSink&&) = delete;
    ~StatusLinePublishSink() = default;

    /// Delivers from now on to @p target, which must stay alive until it revokes.
    /// @param target What deliveries reach.
    void attach(IStatusLinePublishTarget& target) noexcept;

    /// Stops delivering, and waits until no delivery is in flight.
    ///
    /// The wait has no bound under publishers that never stop: each delivery that started before the
    /// target was cleared is waited for, and a publish after it costs only a couple of atomics and reaches
    /// nobody. The owner must not call this -- nor destroy the Terminal, which calls it -- while holding a
    /// lock that a delivery takes (TerminalSession's render-notify mutex, for one): that waits forever.
    void revoke() noexcept;

    /// Tells the target, if one is attached, that a frame was published. Never blocks.
    void publish() noexcept;

  private:
    std::atomic<IStatusLinePublishTarget*> _target = nullptr;
    std::atomic<uint32_t> _inFlight = 0;
};

/// What a terminal needs to show status-line extensions.
///
/// Both are borrowed and must outlive the terminal. Whatever publishes the extensions' frames does so
/// through @ref published, which the terminal attaches itself to and revokes before it is destroyed.
struct StatusLineExtensionServices
{
    IStatusLineExtensions& extensions; ///< The terminal's extensions.
    StatusLinePublishSink& published;  ///< Where their workers tell of a published frame.

    /// How many distinct user variables the terminal remembers to replay to extensions that load or
    /// subscribe later; a name beyond them is not remembered (known names still update). The extensions'
    /// own bound on what they track, so that the replay never tells more than they keep.
    std::size_t maxUserVars;
};

/// Makes the image a fragment holds in place of the one it held: which pool, which id, which remover.
using ImageRehome = std::function<std::shared_ptr<Image const>(Image const& image)>;

/// Replaces the image behind every image fragment of @p line with what @p rehome makes of it -- once
/// per rasterized image, so an image spanning several cells stays one image -- so that the line holds no
/// image another terminal created, nor its remover. Placement, policy and offsets are kept.
/// @param line A line, typically a copy taken from another terminal's grid.
/// @param rehome Makes each replacement image.
void rehomeImageFragments(Line& line, ImageRehome const& rehome);

/// @param image An image.
/// @return An image with @p image's id, format, size and data, whose remover reaches nothing: one that
///         may outlive every terminal.
[[nodiscard]] std::shared_ptr<Image const> detachedImage(Image const& image);

/// Clears every hyperlink of @p line: its ids name entries of another terminal's hyperlink store.
/// @param line A line copied from another terminal's grid.
void clearHyperlinks(Line& line) noexcept;

} // namespace vtbackend
