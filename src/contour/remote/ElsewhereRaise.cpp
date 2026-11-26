// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/FinishNotification.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/remote/ElsewhereRaise.hpp>
#include <contour/window/WindowController.hpp>

#include <QtCore/QString>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <string_view>

#include <vthost/NotifyWire.hpp>

namespace contour::remote
{

namespace
{
    /// @return @p title as a notice of the daemon @p daemonName reads it: unchanged for none.
    [[nodiscard]] std::string titledFor(std::string title, std::string_view daemonName)
    {
        if (daemonName.empty())
            return title;
        if (title.empty())
            return std::string { daemonName };
        return std::format("{}: {}", daemonName, title);
    }

    /// @return A command that finished in a pane this client does not mirror, as a finish notice reads one.
    [[nodiscard]] vtbackend::CommandBlockSummary summaryOf(vthost::proto::FinishedCommand const& finished)
    {
        auto summary = vtbackend::CommandBlockSummary {};
        summary.commandLine = vtbackend::CommandLineText { finished.commandLine };
        summary.exitCode = finished.exitCode;
        summary.duration = finished.durationMs.transform([](uint64_t ms) {
            return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds { ms });
        });
        return summary;
    }

    /// What every finish notice of a pane of another workspace is raised under: the daemon, its run and the
    /// pane's daemon session id follow, so a pane's later finish replaces its earlier one, and no other's.
    constexpr auto FinishedElsewhereIdentifier = std::string_view { "contour-daemon-finished-{}-{}-{}" };

    /// @param directory What the window lists.
    /// @param pane A daemon session.
    /// @return The name of the tab holding @p pane; empty for none.
    [[nodiscard]] std::string tabLabelOf(window::WorkspaceDirectory const& directory, uint64_t pane)
    {
        for (auto const& workspace: directory.workspaces)
            for (auto const& tab: workspace.tabs)
                if (std::ranges::contains(tab.panes, window::PaneKey { pane }, &window::DirectoryPane::key))
                    return tab.name;
        return {};
    }

    /// Notifies a command that finished in a pane of a workspace the window does not select, as a hidden
    /// pane's finish is decided.
    /// @param pane The pane's daemon session.
    /// @param finished How the command finished.
    /// @param tabName The name of the pane's tab.
    /// @param notices What the finish is decided and raised with.
    /// @param daemon The daemon the pane is of.
    /// @return Whether the finish asks the window to ring too.
    [[nodiscard]] RingBell noticeFinishedElsewhere(uint64_t pane,
                                                   vthost::proto::FinishedCommand const& finished,
                                                   std::string const& tabName,
                                                   ElsewhereNotices const& notices,
                                                   RaisingDaemon const& daemon)
    {
        auto const summary = summaryOf(finished);
        auto const ending =
            platform::finishNoticeKind(summary, notices.policy, platform::SessionVisibility::Hidden);
        if (!ending)
            return RingBell::No;
        auto const request = platform::finishNotificationText(
            summary, *ending, tabName, notices.homeDirectory, notices.policy.action);
        auto const effects = platform::effectsOf(request.action);
        if (effects.test(platform::FinishEffect::Notification))
        {
            auto notification = vtbackend::DesktopNotification {};
            notification.identifier =
                std::format(FinishedElsewhereIdentifier, daemon.daemon.value, daemon.instance.value, pane);
            notification.title = titledFor(request.title, daemon.name);
            notification.body = request.body;
            notification.sound = platform::finishNoticeSound(
                request.action, platform::bellAudibilityOf(notices.bell.sound, notices.bell.volume));
            notices.notifier.notify(notification);
        }
        return effects.test(platform::FinishEffect::Bell) ? RingBell::Yes : RingBell::No;
    }

    /// Raises a desktop notification of a pane of a workspace the window does not select, as a hidden pane's.
    /// @param notice What the pane notified.
    /// @param notifier Where it is raised.
    /// @param daemonName The daemon its title names; empty for none.
    void notifyElsewhere(vthost::proto::PaneNotification const& notice,
                         platform::Notifier& notifier,
                         std::string_view daemonName)
    {
        // As a mirrored pane raises one (ScreenMirror::applyEvent): one that stated no occasion takes the OSC
        // 9 / OSC 777 rule.
        auto const occasion =
            vthost::displayOccasionOf(notice.occasion).value_or(platform::UnstatedNotificationOccasion);
        if (!platform::isNoticeWanted(platform::notifyWhenOf(occasion), platform::SessionVisibility::Hidden))
            return;
        auto notification = vtbackend::DesktopNotification {};
        notification.title = titledFor(notice.title, daemonName);
        notification.body = notice.body;
        notification.occasion = occasion;
        notification.focusOnActivation = false;
        notifier.notify(notification);
    }

} // namespace

RingBell raiseElsewhere(std::span<vthost::proto::PaneSummary const> raised,
                        window::WorkspaceDirectory const& directory,
                        ElsewhereNotices const& notices,
                        RaisingDaemon const& daemon)
{
    auto ring = RingBell::No;
    // Out of every window's sight by definition: decided as a hidden pane's are.
    for (auto const& summary: raised)
    {
        if (summary.bell == vthost::proto::PaneBell::Rang)
            ring = RingBell::Yes;
        for (auto const& notice: summary.notifications)
            notifyElsewhere(notice, notices.notifier, daemon.name);
        if (summary.finished
            && noticeFinishedElsewhere(summary.session,
                                       *summary.finished,
                                       tabLabelOf(directory, summary.session),
                                       notices,
                                       daemon)
                   == RingBell::Yes)
            ring = RingBell::Yes;
    }
    // Once per turn, however many rang, as a pane coalesces its own bells.
    return ring;
}

void ringWindowBell(window::WindowController& window, config::Bell const& bell)
{
    window.requestBell(QString::fromStdString(platform::bellSourceOf(bell.sound)),
                       static_cast<double>(bell.volume));
    if (bell.alert)
        window.requestAlert();
}

} // namespace contour::remote
