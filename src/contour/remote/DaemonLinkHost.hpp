// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a remote::DaemonLink needs from the application; light, so that the composition root can implement
/// it without including the link.

#include <core/platform/Clock.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include <boxed-cpp/boxed.hpp>

class QScreen;

namespace contour::remote
{

class DaemonLink;
struct DaemonLinkConfig;
class SwitchGrants;

namespace detail::tags
{
    struct DaemonLinkId
    {
    };
} // namespace detail::tags

/// Names a link among the application's.
using DaemonLinkId = boxed::boxed<uint32_t, detail::tags::DaemonLinkId>;

/// A daemon workspace a link hands to a link of its own: one OS window, one link (@see
/// DaemonLinkConfig::handOverFrom).
struct HandOver
{
    DaemonLinkId from; ///< The link that followed the workspace, and lets it go once the new one shows it.
    uint64_t workspace = 0; ///< The daemon workspace handed over.
};

/// Runs @p fire once, on the GUI thread, @p delay from now: the time source of a switch's deadline and of a
/// switch question's expiry, injected so that a test fires it by hand. Nothing cancels a fire: one for a
/// switch that ended meanwhile finds nothing to give up.
using SwitchTimer = std::function<void(std::chrono::milliseconds delay, std::function<void()> fire)>;

/// How long a switch waits for its target workspace's first layout before it gives up.
inline constexpr auto DefaultSwitchTimeout = std::chrono::milliseconds { std::chrono::seconds { 10 } };

/// The production switch timer: a single-shot QTimer on the GUI thread.
/// @return The timer.
[[nodiscard]] SwitchTimer qtSwitchTimer();

/// What a switch that never lands is bounded by: one for every switch of the process, so that a switch
/// question (switchQuestionExpiry()) leaves the switch it allows the time this gives it.
struct SwitchDeadline
{
    /// How long after its follow is sent, or its link started, a switch waits for the first layout.
    std::chrono::milliseconds timeout = DefaultSwitchTimeout;
    SwitchTimer timer;                   ///< What tells it the time is up.
    core::platform::IClock const& clock; ///< What tells how much time a switch with a landing time has left.
};

/// How a hand-over ended.
enum class HandOverOutcome : uint8_t
{
    Settled = 0, ///< The new link's connection follows the workspace and has its layout: the old one lets go.
    Failed,      ///< The new link did not connect, or the daemon has no such workspace: the old one keeps it.
};

/// Whether the application started the upgrade a link asked for.
enum class UpgradeStart : uint8_t
{
    AlreadyUnderWay = 0, ///< A restart or an upgrade of the application's daemon runs; nothing was started.
    Started,             ///< Under way, or answered at once: the outcome reaches DaemonLink::drift().
};

/// What a link needs from the application: OS windows opened, and the local daemon restarted or upgraded by
/// the process resources the application owns. What the link does with its windows once they are open it
/// does through the session manager.
class IDaemonLinkHost
{
  public:
    virtual ~IDaemonLinkHost() = default;

    IDaemonLinkHost() = default;
    IDaemonLinkHost(IDaemonLinkHost const&) = delete;
    IDaemonLinkHost& operator=(IDaemonLinkHost const&) = delete;
    IDaemonLinkHost(IDaemonLinkHost&&) = delete;
    IDaemonLinkHost& operator=(IDaemonLinkHost&&) = delete;

    /// Opens an OS window for the daemon workspace the link has staged. The window claims it as it loads,
    /// through the attach-window binder the link installed
    /// (session::TerminalSessionManager::consumeAttachWindow
    /// -> DaemonLink::bindPendingWindow()), before this returns (the QML load is synchronous).
    /// @param screen Where the window opens; null for the default placement.
    virtual void openWindowFor(QScreen* screen) = 0;

    /// The user asked, through @p link's drift badge or the UpgradeDaemon action, to upgrade its daemon in
    /// place. Asked only of a link whose daemon is this client's (window::isThisClientsDaemon). The
    /// application has one daemon and so one restart or upgrade at a time, whichever window asks; the
    /// outcome of a started one is reported to DaemonLink::drift().
    /// @param link The link whose daemon to upgrade.
    /// @return AlreadyUnderWay when another restart or upgrade runs, and nothing was started.
    [[nodiscard]] virtual UpgradeStart upgradeDaemon(DaemonLink& link) = 0;

    /// The user asked, through @p link's drift badge, to restart its daemon on this release. Asked only of a
    /// link whose daemon is window::DaemonLocality::Local; the outcome is reported to DaemonLink::drift().
    /// Refused, with the failure reported to the link's drift model, while another restart or upgrade runs.
    /// @param link The link whose daemon to restart.
    virtual void restartDaemon(DaemonLink& link) = 0;

    /// A link stopped because the last window it served went, and asks to be destroyed. Not at once: the
    /// application destroys it from its event loop, once its controller binds no pane any more, for the
    /// ptys of the closed windows' sessions unregister from it as they go (DaemonLink::isFinished). Told
    /// again as each of those panes goes.
    virtual void linkEnded() = 0;

    /// Starts a link of its own for a further daemon workspace a link was pushed -- a launch brings back
    /// several, a new window asks for one -- for each OS window has a connection of its own. The new link
    /// opens its window once its connection shows the workspace, and reports to handOverSettled().
    /// @param config What the new link is built with: its intent names the workspace, and its
    ///        DaemonLinkConfig::handOverFrom the link it takes it from.
    virtual void startHandOverLink(DaemonLinkConfig config) = 0;

    /// A link started by startHandOverLink() reports how the hand-over went, for the application to tell the
    /// link it took the workspace from (DaemonLink::completeHandOver), if that one still exists.
    /// @param handOver What was handed over, and by which link.
    /// @param outcome How it went.
    /// @param reason Why it failed; empty when it settled.
    virtual void handOverSettled(HandOver handOver, HandOverOutcome outcome, std::string const& reason) = 0;

    /// @return The answers that stand for the switches programs ask for, one memory for every link of the
    ///         process and for its lifetime (spec §Safety): a link asks it before prompting.
    [[nodiscard]] virtual SwitchGrants& switchGrants() = 0;

    /// @return What bounds the switches of the process: what times out the questions a link asks about the
    ///         switches programs ask for (switchQuestionExpiry()), and the landing time those leave the
    ///         switch. The one the application's switcher is built with.
    [[nodiscard]] virtual SwitchDeadline const& switchDeadline() = 0;
};

} // namespace contour::remote
