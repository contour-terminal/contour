// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The daemons a sidebar lists, faked: sections the test sets, every verb recorded, and subscribers told when
/// the test says the sections changed. Qt-free, so the window/ tests in contour_test use it as the sidebar's
/// tests in contour_gui_test do (SidebarFakes.hpp includes it).

#include <contour/window/DaemonSections.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace contour::test
{

/// One verb a @c FakeDaemonSections was asked, with its arguments.
struct DaemonSectionsCall
{
    /// A call of IDaemonSections::connect().
    struct Connect
    {
        window::DaemonRef daemon;
        vtworkspace::WorkspaceId askingWindow;
        bool operator==(Connect const&) const = default;
    };
    /// A call of IDaemonSections::disconnect().
    struct Disconnect
    {
        window::DaemonRef daemon;

        bool operator==(Disconnect const&) const = default;
    };

    /// A call of IDaemonSections::start().
    struct Start
    {
        window::DaemonRef daemon;
        bool operator==(Start const&) const = default;
    };
    /// A call of IDaemonSections::remove().
    struct Remove
    {
        window::DaemonRef daemon;
        bool operator==(Remove const&) const = default;
    };
    /// A call of IDaemonSections::setListed().
    struct SetListed
    {
        window::DaemonRef daemon;
        window::SidebarListingChoice choice;
        bool operator==(SetListed const&) const = default;
    };
    /// A call of IDaemonSections::renameWorkspace().
    struct RenameWorkspace
    {
        window::DaemonRef daemon;
        window::DaemonInstance instance;
        uint64_t workspace;
        std::string name;
        bool operator==(RenameWorkspace const&) const = default;
    };
    /// A call of IDaemonSections::renameTab().
    struct RenameTab
    {
        window::DaemonRef daemon;
        window::DaemonInstance instance;
        window::TabRef tab;
        std::string name;
        bool operator==(RenameTab const&) const = default;
    };
    /// A call of IDaemonSections::setWorkspaceColor().
    struct SetWorkspaceColor
    {
        window::DaemonRef daemon;
        window::DaemonInstance instance;
        uint64_t workspace;
        std::optional<vtbackend::RGBColor> color;
        bool operator==(SetWorkspaceColor const&) const = default;
    };
    /// A call of IDaemonSections::closeTab().
    struct CloseTab
    {
        window::DaemonRef daemon;
        window::DaemonInstance instance;
        window::TabRef tab;
        bool operator==(CloseTab const&) const = default;
    };
    /// A call of IDaemonSections::closeWorkspace().
    struct CloseWorkspace
    {
        window::DaemonRef daemon;
        window::DaemonInstance instance;
        uint64_t workspace;
        bool operator==(CloseWorkspace const&) const = default;
    };
    /// A call of IDaemonSections::markSeen().
    struct MarkSeen
    {
        window::DaemonRef daemon;
        window::DaemonInstance instance;
        window::PaneKey pane;
        window::PaneSeen seen;
        bool operator==(MarkSeen const&) const = default;
    };

    /// Which verb, with its arguments.
    std::variant<Connect,
                 Disconnect,
                 Start,
                 Remove,
                 SetListed,
                 RenameWorkspace,
                 RenameTab,
                 SetWorkspaceColor,
                 CloseTab,
                 CloseWorkspace,
                 MarkSeen>
        verb;

    bool operator==(DaemonSectionsCall const&) const = default;
};

/// Lists what the test sets, records every verb, and tells its subscribers from changed().
class FakeDaemonSections final: public window::IDaemonSections
{
  public:
    std::vector<window::DaemonSection> listed; ///< What sections() returns.
    std::vector<DaemonSectionsCall> calls;     ///< Every verb asked, in order.
    mutable int reads = 0;                     ///< How often sections() was asked.

    /// Tells every subscriber that the sections changed.
    void changed() const { _subscribers.notify(); }

    /// @return How many subscriptions are held.
    [[nodiscard]] size_t subscriberCount() const noexcept { return _subscribers.size(); }

    [[nodiscard]] std::vector<window::DaemonSection> sections() const override
    {
        ++reads;
        return listed;
    }

    void connect(window::DaemonRef const& daemon, vtworkspace::WorkspaceId askingWindow) override
    {
        record(DaemonSectionsCall::Connect { .daemon = daemon, .askingWindow = askingWindow });
    }
    void disconnect(window::DaemonRef const& daemon) override
    {
        record(DaemonSectionsCall::Disconnect { .daemon = daemon });
    }

    /// As the real presence: only a daemon of this machine is started; a host's is refused unrecorded.
    void start(window::DaemonRef const& daemon) override
    {
        if (auto const parsed = window::parseDaemonRef(daemon);
            !parsed || parsed->first != window::DaemonRefKind::Local)
            return;
        record(DaemonSectionsCall::Start { .daemon = daemon });
    }
    void remove(window::DaemonRef const& daemon) override
    {
        record(DaemonSectionsCall::Remove { .daemon = daemon });
    }
    void setListed(window::DaemonRef const& daemon, window::SidebarListingChoice choice) override
    {
        record(DaemonSectionsCall::SetListed { .daemon = daemon, .choice = choice });
    }
    void renameWorkspace(window::DaemonRef const& daemon,
                         window::DaemonInstance instance,
                         uint64_t workspace,
                         std::string name) override
    {
        record(DaemonSectionsCall::RenameWorkspace {
            .daemon = daemon, .instance = instance, .workspace = workspace, .name = std::move(name) });
    }
    void renameTab(window::DaemonRef const& daemon,
                   window::DaemonInstance instance,
                   window::TabRef tab,
                   std::string name) override
    {
        record(DaemonSectionsCall::RenameTab {
            .daemon = daemon, .instance = instance, .tab = tab, .name = std::move(name) });
    }
    void setWorkspaceColor(window::DaemonRef const& daemon,
                           window::DaemonInstance instance,
                           uint64_t workspace,
                           std::optional<vtbackend::RGBColor> color) override
    {
        record(DaemonSectionsCall::SetWorkspaceColor {
            .daemon = daemon, .instance = instance, .workspace = workspace, .color = color });
    }
    void closeTab(window::DaemonRef const& daemon,
                  window::DaemonInstance instance,
                  window::TabRef tab) override
    {
        record(DaemonSectionsCall::CloseTab { .daemon = daemon, .instance = instance, .tab = tab });
    }
    void closeWorkspace(window::DaemonRef const& daemon,
                        window::DaemonInstance instance,
                        uint64_t workspace) override
    {
        record(DaemonSectionsCall::CloseWorkspace {
            .daemon = daemon, .instance = instance, .workspace = workspace });
    }
    void markSeen(window::DaemonRef const& daemon,
                  window::DaemonInstance instance,
                  window::PaneKey pane,
                  window::PaneSeen seen) override
    {
        record(DaemonSectionsCall::MarkSeen {
            .daemon = daemon, .instance = instance, .pane = pane, .seen = seen });
    }

    [[nodiscard]] window::SectionsSubscription onChanged(std::function<void()> changed) override
    {
        return _subscribers.subscribe(std::move(changed));
    }

  private:
    template <typename Verb>
    void record(Verb verb)
    {
        calls.push_back(DaemonSectionsCall { .verb = std::move(verb) });
    }

    window::SectionsSubscribers _subscribers;
};

} // namespace contour::test
