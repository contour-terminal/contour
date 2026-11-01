// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <vtworkspace/ModelEvents.hpp>
#include <vtworkspace/SessionModel.hpp>

namespace vtworkspace::test
{

/// Records every model event so tests can assert what fired.
struct RecordingEvents: ModelEvents
{
    std::vector<std::string> log;

    void tabAboutToBeAdded(WindowId, int index) override
    {
        log.push_back(std::format("tabAboutToBeAdded:{}", index));
    }
    void tabAdded(WindowId, TabId t, int index) override
    {
        log.push_back(std::format("tabAdded:{}:{}", t.value, index));
    }
    void tabAboutToBeRemoved(WindowId, int index) override
    {
        log.push_back(std::format("tabAboutToBeRemoved:{}", index));
    }
    void tabClosed(WindowId, TabId t, int index) override
    {
        log.push_back(std::format("tabClosed:{}:{}", t.value, index));
    }
    void tabAboutToBeMoved(WindowId, int from, int to) override
    {
        log.push_back(std::format("tabAboutToBeMoved:{}>{}", from, to));
    }
    void tabMoved(WindowId, TabId t, int from, int to) override
    {
        log.push_back(std::format("tabMoved:{}:{}>{}", t.value, from, to));
    }
    void activeTabChanged(WindowId, TabId t, int index) override
    {
        log.push_back(std::format("activeTab:{}:{}", t.value, index));
    }
    void paneSplit(TabId t, PaneId splitNode, PaneId newLeaf) override
    {
        log.push_back(std::format("paneSplit:{}:{}", t.value, newLeaf.value));
        lastSplitNode = splitNode;
        lastNewLeaf = newLeaf;
    }
    void paneClosed(TabId t, PaneId closed, PaneId survivor) override
    {
        // Log the survivor too, so the collapse/fold contract (which leaf absorbs) can be asserted.
        log.push_back(std::format("paneClosed:{}:{}:{}", t.value, closed.value, survivor.value));
        lastClosedSurvivor = survivor;
    }
    void activePaneChanged(TabId t, PaneId leaf) override
    {
        log.push_back(std::format("activePane:{}:{}", t.value, leaf.value));
        lastActivePane = leaf;
    }
    void paneRatioChanged(TabId, PaneId node, double ratio) override
    {
        log.push_back(std::format("ratio:{}:{}", node.value, ratio));
        lastRatio = ratio;
    }
    void paneOrientationChanged(TabId t, PaneId splitNode, SplitState newState) override
    {
        log.push_back(
            std::format("orientation:{}:{}:{}", t.value, splitNode.value, static_cast<int>(newState)));
        lastOrientation = newState;
    }
    void paneSwapped(TabId t, PaneId a, PaneId b) override
    {
        log.push_back(std::format("swapped:{}:{}:{}", t.value, a.value, b.value));
    }
    void paneSessionReplaced(TabId t, PaneId leaf) override
    {
        log.push_back(std::format("replaced:{}:{}", t.value, leaf.value));
    }
    void paneZoomChanged(TabId t, std::optional<PaneId> zoomedLeaf) override
    {
        // Log the leaf, not just "on/off": zoom follows focus, so it can move between leaves while
        // staying on, and a host has to re-read WHICH pane is on screen.
        log.push_back(std::format(
            "zoom:{}:{}", t.value, zoomedLeaf.has_value() ? std::to_string(zoomedLeaf->value) : "-"));
        lastZoomedLeaf = zoomedLeaf;
    }
    void paneTreeRestructured(TabId t) override { log.push_back(std::format("restructured:{}", t.value)); }
    void tabAboutToBeMovedToWindow(WindowId from, int fromIdx, WindowId to, int toIdx) override
    {
        log.push_back(std::format("tabAboutToMoveWin:{}:{}>{}:{}", from.value, fromIdx, to.value, toIdx));
    }
    void tabMovedToWindow(WindowId from, TabId t, int fromIdx, WindowId to, int toIdx) override
    {
        log.push_back(
            std::format("tabMovedWin:{}:{}:{}>{}:{}", from.value, t.value, fromIdx, to.value, toIdx));
    }

    // Last-seen values for direct assertions (the log strings are for ordering/count).
    std::optional<PaneId> lastSplitNode;
    std::optional<PaneId> lastNewLeaf;
    std::optional<PaneId> lastClosedSurvivor;
    std::optional<PaneId> lastActivePane;
    std::optional<double> lastRatio;
    std::optional<SplitState> lastOrientation;
    std::optional<PaneId> lastZoomedLeaf;

    /// Count how many log entries start with @p prefix (for exact-once assertions).
    [[nodiscard]] long countPrefix(std::string const& prefix) const
    {
        return std::count_if(log.begin(), log.end(), [&](auto const& e) { return e.starts_with(prefix); });
    }
    void tabTitleChanged(TabId t) override { log.push_back(std::format("title:{}", t.value)); }
    void tabColorChanged(TabId t) override { log.push_back(std::format("color:{}", t.value)); }

    [[nodiscard]] bool sawPrefix(std::string const& prefix) const
    {
        for (auto const& e: log)
            if (e.starts_with(prefix))
                return true;
        return false;
    }
};

struct Fixture
{
    RecordingEvents events;
    uint64_t nextSession = 1000;
    SessionModel model { events, [this] { return SessionId { nextSession++ }; } };
};

} // namespace vtworkspace::test
