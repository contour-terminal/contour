// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which window shows an ssh prompt. The broker names the route a prompt belongs to -- the window that
/// started the attach -- and the router finds the window's sink; it holds a prompt back while no window can
/// show it.

#include <contour/askpass/AskpassPromptSink.hpp>

#include <QtCore/QString>

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace contour::askpass
{

/// Names where a prompt is shown: the id of the window that started the attach.
using AskpassRouteId = std::uint64_t;

/// The route of an attach that no window started: the first window that is there, or the first to appear.
inline constexpr AskpassRouteId AnyWindow = 0;

/// The route of the stand-in window that shows prompts while no real window exists yet (`contour client
/// --host`'s first attach runs before its first window does). It is the last resort: once a real window has a
/// sink, the prompts that have no window of their own go there.
inline constexpr AskpassRouteId BootstrapRoute = std::numeric_limits<AskpassRouteId>::max();

/// Picks the sink that shows the prompts of @p route.
/// @param attached The routes that have a sink, oldest first.
/// @param route The route a prompt asks for.
/// @return @p route if it has a sink; else the oldest real window's; else the stand-in's; none for no sink.
[[nodiscard]] std::optional<AskpassRouteId> pickAskpassSinkRoute(std::span<AskpassRouteId const> attached,
                                                                 AskpassRouteId route);

/// What the application publishes about one window that has a prompt sink.
struct SinkExposure
{
    AskpassRouteId route = AnyWindow; ///< The window's route.
    /// Whether the window is on screen for the user.
    enum class State : std::uint8_t
    {
        OnScreen = 0,
        Hidden, ///< Hidden, minimised, or parked off screen.
    } state = State::OnScreen;
};

/// Finds the sink of the window a prompt belongs to. GUI thread only, as the sinks are.
///
/// A prompt whose window is not on screen is held: nobody would see it, so it waits, like one that has no
/// window yet, and is shown once its window is exposed (updateExposure()).
///
/// A prompt whose window has no sink, or whose route is AnyWindow, goes to the sink that was attached first
/// (the BootstrapRoute's last); while there is none the prompt waits, and goes to the first sink attached.
/// The router is shared by the broker and by every sink's owner, so that neither has to outlive the other.
class AskpassPromptRouter
{
  public:
    /// @param onWaiting Told, on the GUI thread, whenever a prompt has to wait because no sink can show it:
    /// the
    ///        application's cue to bring up a window for it. Empty: prompts wait for a window to appear.
    /// @param onSinksChanged Told, on the GUI thread, after a sink was attached or detached or the router
    /// shut
    ///        down. Empty: nobody is told.
    /// @param onHeldChanged Told, on the GUI thread, after the prompts that wait for a window changed: one
    ///        began to wait, or was shown, withdrawn or cancelled. Empty: nobody is told.
    explicit AskpassPromptRouter(std::function<void()> onWaiting = {},
                                 std::function<void()> onSinksChanged = {},
                                 std::function<void()> onHeldChanged = {}):
        _onWaiting { std::move(onWaiting) },
        _onSinksChanged { std::move(onSinksChanged) },
        _onHeldChanged { std::move(onHeldChanged) }
    {
    }

    /// Says which windows are on screen. A window not named, and the stand-in, count as on screen. The
    /// prompts held for a window that is now on screen are shown.
    /// @param windows Every window that has a sink.
    void updateExposure(std::vector<SinkExposure> const& windows);

    /// @param route The window an attach was started from.
    /// @return Whether a prompt of that route waits, for want of a window or because it is not on screen.
    [[nodiscard]] bool isHolding(AskpassRouteId route) const noexcept;

    /// @return The routes that have a sink, oldest first.
    [[nodiscard]] std::span<AskpassRouteId const> attachedRoutes() const noexcept { return _attachOrder; }
    ~AskpassPromptRouter() = default;

    AskpassPromptRouter(AskpassPromptRouter const&) = delete;
    AskpassPromptRouter& operator=(AskpassPromptRouter const&) = delete;
    AskpassPromptRouter(AskpassPromptRouter&&) = delete;
    AskpassPromptRouter& operator=(AskpassPromptRouter&&) = delete;

    /// Makes @p sink the one that shows the prompts of @p route. Prompts that wait go to it.
    /// @param route The window.
    /// @param sink Borrowed until detach(); one per route.
    void attach(AskpassRouteId route, IAskpassPromptSink& sink);

    /// Takes the sink of @p route away. Its open prompts are cancelled: nobody can answer them any more.
    /// @param route The window; one that has no sink is ignored.
    void detach(AskpassRouteId route);

    /// Puts a prompt in front of the user, in the window of @p route.
    /// @copydetails IAskpassPromptSink::ask
    /// @param route Where the prompt belongs.
    void ask(AskpassRouteId route,
             AskpassPromptId promptId,
             AskpassPromptKind kind,
             QString target,
             QString prompt,
             std::function<void(AskpassReply)> reply);

    /// @copydoc IAskpassPromptSink::withdraw
    void withdraw(AskpassPromptId promptId);

    /// Ends the router's service: every prompt is cancelled, no sink is kept, and later prompts are cancelled
    /// as they come.
    void shutdown();

    /// @return Whether a prompt is open or waits: someone is to answer, so a wait that serves the GUI's
    /// events
    ///         meanwhile must let the user's input through.
    [[nodiscard]] bool hasOpenPrompt() const noexcept { return !_shown.empty() || !_waiting.empty(); }

    /// @return How many prompts wait for a sink to be attached.
    [[nodiscard]] std::size_t waitingCount() const noexcept { return _waiting.size(); }

  private:
    /// A prompt asked and not yet ended.
    struct Prompt
    {
        AskpassPromptId id = 0;
        AskpassRouteId route = AnyWindow; ///< The route it asked for.
        AskpassPromptKind kind = AskpassPromptKind::Secret;
        QString target;
        QString text;
        std::function<void(AskpassReply)> reply;
        AskpassRouteId shownBy = AnyWindow; ///< The route of the sink that has it; meaningful once shown.
    };

    /// @return The route whose sink shows prompts asked for @p route; none while no sink is attached.
    [[nodiscard]] std::optional<AskpassRouteId> sinkRouteFor(AskpassRouteId route) const;

    /// Shows the prompts that wait to the windows that can show them now.
    void flush();

    /// Tells whoever wants to know that the prompts that wait changed.
    void noteHeldChanged() const;

    /// Hands @p prompt to the sink of @p sinkRoute.
    void deliver(Prompt prompt, AskpassRouteId sinkRoute);

    /// Cancels @p prompt: its reply is told so, with no answer.
    static void cancel(Prompt& prompt);

    std::map<AskpassRouteId, IAskpassPromptSink*> _sinks;
    std::vector<AskpassRouteId> _attachOrder; ///< The routes of the sinks, oldest first.
    /// Routes whose sink went away: their window is closed, and what they still ask is cancelled, not shown
    /// in another window the user did not start it from.
    std::set<AskpassRouteId> _closed;
    std::vector<Prompt> _waiting; ///< Prompts no sink could take yet, in the order asked.
    std::vector<Prompt> _shown;   ///< Prompts a sink has, in the order asked.
    std::function<void()> _onWaiting;
    std::function<void()> _onSinksChanged;
    std::function<void()> _onHeldChanged;
    std::set<AskpassRouteId> _hidden; ///< The routes whose window is not on screen.
    bool _isShutDown = false;
};

} // namespace contour::askpass
