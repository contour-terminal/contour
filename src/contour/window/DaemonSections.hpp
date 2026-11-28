// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a window reaches the daemons its sidebar lists, its own and every other: their sections, and the verbs
/// a sidebar row may ask of them. Qt-free and free of `remote/`: the presence service implements it, and the
/// sidebar and its tests speak only this.

#include <contour/window/DaemonSection.hpp>
#include <contour/window/PaneKey.hpp>
#include <contour/window/PaneSeen.hpp>
#include <contour/window/SidebarTree.hpp>

#include <vtbackend/core/Color.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::window
{

/// Holds one subscription to change notices, and ends it when destroyed: the subscriber cannot be called
/// after its subscription is gone. Move-only; a moved-from subscription holds nothing.
class SectionsSubscription
{
  public:
    /// A subscription to nothing.
    SectionsSubscription() = default;

    /// @param unsubscribe Ends the subscription; called once, when this one is destroyed or assigned over.
    explicit SectionsSubscription(std::function<void()> unsubscribe);

    ~SectionsSubscription();

    /// Takes @p other's subscription, leaving it holding nothing.
    SectionsSubscription(SectionsSubscription&& other) noexcept;

    /// Ends the subscription held so far, then takes @p other's, leaving it holding nothing.
    SectionsSubscription& operator=(SectionsSubscription&& other) noexcept;

    SectionsSubscription(SectionsSubscription const&) = delete;
    SectionsSubscription& operator=(SectionsSubscription const&) = delete;

    /// @return Whether a subscription is held.
    [[nodiscard]] bool holds() const noexcept { return static_cast<bool>(_unsubscribe); }

  private:
    /// Ends the subscription held, if any.
    void reset() noexcept;

    std::function<void()> _unsubscribe; ///< Empty when nothing is held.
};

/// The subscribers to change notices of an IDaemonSections, and the telling of them.
class SectionsSubscribers
{
  public:
    /// @param changed Told by every notify() until the subscription returned is destroyed.
    /// @return The subscription. It ends nothing once these subscribers are gone.
    [[nodiscard]] SectionsSubscription subscribe(std::function<void()> changed);

    /// Tells every subscriber, in the order they subscribed. One that a subscriber's call unsubscribed is not
    /// told any more; one that a call subscribed is told from the next notify() on.
    void notify() const;

    /// @return How many subscriptions are held.
    [[nodiscard]] std::size_t size() const noexcept { return _subscribers->size(); }

  private:
    using Subscribers = std::map<uint64_t, std::function<void()>>;

    std::shared_ptr<Subscribers> _subscribers = std::make_shared<Subscribers>();
    uint64_t _next = 1;
};

/// The daemons a window's sidebar lists, and the verbs its rows ask of them.
///
/// Every verb on a workspace, tab or pane names the daemon's instance it was read from: a verb whose
/// instance no longer matches the daemon's (it restarted) names nothing and does nothing. A verb on a daemon
/// that is not listed does nothing either.
class IDaemonSections
{
  public:
    virtual ~IDaemonSections() = default;

    /// @return Every listed daemon, in tree order.
    [[nodiscard]] virtual std::vector<DaemonSection> sections() const = 0;

    /// Connects to @p daemon, prompting (for a password, a host key) in @p askingWindow. Does nothing for a
    /// `link:` daemon, which no switch can name.
    /// @param daemon       The daemon.
    /// @param askingWindow The window whose row asked.
    virtual void connect(DaemonRef const& daemon, vtworkspace::WorkspaceId askingWindow) = 0;

    /// Ends a connect() still connecting: the daemon reads Not connected, and nothing dials it again until
    /// asked. Does nothing for a daemon not connecting.
    /// @param daemon The daemon.
    virtual void disconnect(DaemonRef const& daemon) = 0;

    /// Starts a local daemon that is not running.
    /// @param daemon The daemon.
    virtual void start(DaemonRef const& daemon) = 0;

    /// Unlists a daemon for the rest of the run (not kept in the GUI state).
    /// @param daemon The daemon.
    virtual void remove(DaemonRef const& daemon) = 0;

    /// Keeps the user's choice to list a daemon, or not, in the GUI state.
    /// @param daemon The daemon.
    /// @param choice Whether to list it.
    virtual void setListed(DaemonRef const& daemon, SidebarListingChoice choice) = 0;

    /// @return The daemons the user chose not to list that the Hidden row offers back, in tree order.
    [[nodiscard]] virtual std::vector<HiddenDaemon> hiddenDaemons() const = 0;

    /// Renames a workspace.
    /// @param daemon    The daemon serving it.
    /// @param instance  The daemon's run the workspace was read from.
    /// @param workspace The workspace's daemon id.
    /// @param name      Its new name.
    virtual void renameWorkspace(DaemonRef const& daemon,
                                 DaemonInstance instance,
                                 uint64_t workspace,
                                 std::string name) = 0;

    /// Renames a tab.
    /// @param daemon   The daemon serving it.
    /// @param instance The daemon's run the tab was read from.
    /// @param tab      The tab.
    /// @param name     Its new name.
    virtual void renameTab(DaemonRef const& daemon,
                           DaemonInstance instance,
                           TabRef tab,
                           std::string name) = 0;

    /// Colours a workspace, or clears its colour.
    /// @param daemon    The daemon serving it.
    /// @param instance  The daemon's run the workspace was read from.
    /// @param workspace The workspace's daemon id.
    /// @param color     Its colour; none to clear it.
    virtual void setWorkspaceColor(DaemonRef const& daemon,
                                   DaemonInstance instance,
                                   uint64_t workspace,
                                   std::optional<vtbackend::RGBColor> color) = 0;

    /// Closes a tab: every pane of it.
    /// @param daemon   The daemon serving it.
    /// @param instance The daemon's run the tab was read from.
    /// @param tab      The tab.
    virtual void closeTab(DaemonRef const& daemon, DaemonInstance instance, TabRef tab) = 0;

    /// Colours a tab, or clears its colour.
    /// @param daemon   The daemon serving it.
    /// @param instance The daemon's run the tab was read from.
    /// @param tab      The tab.
    /// @param color    Its colour; none to clear it.
    virtual void setTabColor(DaemonRef const& daemon,
                             DaemonInstance instance,
                             TabRef tab,
                             std::optional<vtbackend::RGBColor> color) = 0;

    /// Closes a pane.
    /// @param daemon   The daemon serving it.
    /// @param instance The daemon's run the pane was read from.
    /// @param pane     The pane.
    virtual void closePane(DaemonRef const& daemon, DaemonInstance instance, PaneKey pane) = 0;

    /// Closes a workspace.
    /// @param daemon    The daemon serving it.
    /// @param instance  The daemon's run the workspace was read from.
    /// @param workspace The workspace's daemon id.
    virtual void closeWorkspace(DaemonRef const& daemon, DaemonInstance instance, uint64_t workspace) = 0;

    /// Tells the daemon what the user saw of a pane it keeps the flags of.
    /// @param daemon   The daemon serving it.
    /// @param instance The daemon's run the pane was read from.
    /// @param pane     The pane.
    /// @param seen     What was seen of it.
    virtual void markSeen(DaemonRef const& daemon, DaemonInstance instance, PaneKey pane, PaneSeen seen) = 0;

    /// Subscribes to changes of sections(). GUI thread: @p changed is called on it.
    /// @param changed Called after the sections changed.
    /// @return The subscription; @p changed is not called once it is destroyed.
    [[nodiscard]] virtual SectionsSubscription onChanged(std::function<void()> changed) = 0;
};

/// @param sections The daemon sections.
/// @param daemon A daemon.
/// @return The section @p daemon is listed by now; none for one not listed.
[[nodiscard]] inline std::optional<DaemonSection> listedSection(IDaemonSections const& sections,
                                                                DaemonRef const& daemon)
{
    auto listed = sections.sections();
    auto const found = std::ranges::find(listed, daemon, &DaemonSection::daemon);
    return found != listed.end() ? std::optional { std::move(*found) } : std::nullopt;
}

/// @param daemon  A daemon.
/// @param section The section @p daemon is listed by, if any (listedSection()).
/// @return The name the window calls @p daemon by: its section's, else the one its reference carries, else
///         the reference itself.
[[nodiscard]] inline std::string daemonDisplayName(DaemonRef const& daemon,
                                                   std::optional<DaemonSection> const& section)
{
    if (section && !section->name.empty())
        return section->name;
    auto const parsed = parseDaemonRef(daemon);
    return parsed ? std::string { parsed->second } : daemon.value;
}

} // namespace contour::window
