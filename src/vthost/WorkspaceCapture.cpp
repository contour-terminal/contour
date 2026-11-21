// SPDX-License-Identifier: Apache-2.0
#include <vthost/WorkspaceCapture.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <algorithm>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <vthost/CommandBlockWire.hpp>
#include <vthost/GridWire.hpp>
#include <vthost/ObjectRegistry.hpp>
#include <vthost/SessionHost.hpp>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

namespace
{
    /// The record to keep of @p record, or nullopt for one that is not worth restoring.
    [[nodiscard]] std::optional<vtbackend::CommandBlockRecord> restorableRecord(
        vtbackend::CommandBlockRecord record)
    {
        switch (record.state)
        {
            case vtbackend::CommandBlockState::Prompting: return std::nullopt;
            case vtbackend::CommandBlockState::Running:
                // The program did not finish, and after a restore it cannot still be running.
                record.state = vtbackend::CommandBlockState::Finished;
                record.end = vtbackend::CommandBlockEnd::Abandoned;
                return record;
            case vtbackend::CommandBlockState::Finished: return record;
        }
        return std::nullopt;
    }

    /// What a pane's newest unfinished command runs, as the shell reported it.
    [[nodiscard]] std::optional<std::string> runningCommandLineOf(vtbackend::Terminal const& terminal)
    {
        auto const* const current = terminal.commandBlocks().current();
        if (current == nullptr || current->state != vtbackend::CommandBlockState::Running)
            return std::nullopt;
        auto line = current->commandLine.raw();
        if (line.empty())
            return std::nullopt;
        return std::string { line };
    }

    /// @return What @p objects names @p uuid's object by: its UUID and name; nullopt for one it does not
    /// name.
    [[nodiscard]] std::optional<ObjectIdentityRecord> identityOf(ObjectRegistry const& objects,
                                                                 vtcontrol::ObjectKind kind,
                                                                 std::optional<vtcontrol::Uuid> const& uuid)
    {
        if (!uuid)
            return std::nullopt;
        auto record = ObjectIdentityRecord { .uuid = uuid->bytes };
        if (auto const name = objects.nameOf(vtcontrol::ObjectRef { .kind = kind, .uuid = *uuid });
            !name.empty())
            record.name = std::string { name };
        return record;
    }

    /// @return Whether some leaf of the tree under @p pane, at any depth, is a served session of @p host.
    [[nodiscard]] bool holdsServedLeaf(SessionHost const& host, vtworkspace::Pane const& pane)
    {
        if (pane.isLeaf())
            return host.servedBy(pane.session()) != nullptr;
        return holdsServedLeaf(host, *pane.first()) || holdsServedLeaf(host, *pane.second());
    }

    /// The active tab among those a capture kept: one left out (served, or agent panes alone) maps to the tab
    /// that would become active were it closed -- the one after it, else the one before.
    /// @param capturedBeforeActive How many captured tabs come before the workspace's active one.
    /// @param captured How many tabs were captured; at least one.
    /// @return The captured active tab's index.
    [[nodiscard]] std::uint32_t activeTabAmongCaptured(std::size_t capturedBeforeActive, std::size_t captured)
    {
        return static_cast<std::uint32_t>(std::min(capturedBeforeActive, captured - 1));
    }

    /// Walks a pane tree depth first, first child first, converting it; an agent's pane is pruned.
    class TreeCapture
    {
      public:
        TreeCapture(SessionHost& host, CaptureOptions const& options, vtworkspace::Pane const* active):
            _host { host }, _options { options }, _active { active }
        {
        }

        /// @return The tree under @p pane, an extension's agent pane pruned from it -- its sibling takes the
        ///         split's place; nullopt where no leaf is left.
        [[nodiscard]] std::optional<PaneNode> capture(vtworkspace::Pane const& pane)
        {
            if (pane.isLeaf())
            {
                // An agent's pane is left out: a restore would bring it back as a shell, or re-run it.
                if (isOwned(pane.session()))
                    return std::nullopt;
                if (&pane == _active)
                    _activeLeaf = _leaves;
                ++_leaves;
                return PaneNode { .split = pane.splitState(), .pane = capturePane(pane.session()) };
            }
            auto first = capture(*pane.first());
            auto second = capture(*pane.second());
            if (!first || !second)
                return first ? std::move(first) : std::move(second);
            auto node = PaneNode { .split = pane.splitState(), .ratio = pane.ratio() };
            node.children.push_back(std::move(*first));
            node.children.push_back(std::move(*second));
            return node;
        }

        /// @return The leaf order position of the active pane; 0 when it was not met.
        [[nodiscard]] std::uint32_t activeLeaf() const noexcept { return _activeLeaf; }

      private:
        /// @return Whether @p session runs a daemon-scoped extension's agent pane.
        [[nodiscard]] bool isOwned(vtworkspace::SessionId session) const
        {
            auto const pane = _host.objects().uuidOf(session);
            return pane && _host.ownedPanes().ownsPane(*pane);
        }

        [[nodiscard]] PaneSnapshot capturePane(vtworkspace::SessionId session)
        {
            auto pane = PaneSnapshot {};
            pane.identity =
                identityOf(_host.objects(), vtcontrol::ObjectKind::Pane, _host.objects().uuidOf(session));
            auto runtime = _host.paneRuntime(session);
            pane.workingDirectory = std::move(runtime.workingDirectory);
            pane.foregroundProcess = std::move(runtime.foregroundProcess);
            pane.profile = std::move(runtime.profile);
            if (auto* const terminal = _host.terminal(session))
            {
                auto const guard = std::lock_guard { *terminal };
                pane.runningCommandLine = runningCommandLineOf(*terminal);
                if (_options.scrollback == ScrollbackPersistence::Tail)
                    if (auto history = capturePaneHistory(*terminal, _options.scrollbackRows);
                        !history.lines.empty())
                        pane.history = std::move(history);
            }
            return pane;
        }

        SessionHost& _host;
        CaptureOptions const& _options;
        vtworkspace::Pane const* _active;
        std::uint32_t _leaves = 0;
        std::uint32_t _activeLeaf = 0;
    };
} // namespace

proto::Delta capturePaneHistory(vtbackend::Terminal const& terminal, std::size_t maxRows)
{
    auto const& screen = terminal.primaryScreen();
    auto const& grid = screen.grid();
    auto const& store = terminal.commandBlocks();

    auto const top = -unbox<int>(grid.historyLineCount());
    auto const end = unbox<int>(screen.cursor().position.line);
    auto const isTrailingBlank = [&](int row) {
        auto const& line = grid.lineAt(vtbackend::LineOffset(row));
        return line.empty() && line.blockId().value == 0;
    };
    // Newest first: the blank rows under the last printed one are dropped, then the newest maxRows are kept.
    auto newestFirst = std::ranges::to<std::vector<int>>(std::views::iota(top, end) | std::views::reverse
                                                         | std::views::drop_while(isTrailingBlank)
                                                         | std::views::take(maxRows));
    std::ranges::reverse(newestFirst);

    auto history = proto::Delta {};
    history.snapshot = 1;
    history.commandBlockIdEpoch = store.idEpoch();

    auto linkIds = std::set<uint16_t> {};
    auto kept = std::set<uint32_t> {};
    auto dropped = std::set<uint32_t> {};
    for (auto const row: newestFirst)
    {
        auto const offset = vtbackend::LineOffset(row);
        auto wire = toWireLine(grid, offset, grid.lineAt(offset));
        for (auto const& cell: wire.cells)
            if (cell.hyperlink != 0)
                linkIds.insert(cell.hyperlink);
        if (wire.blockId != 0 && !kept.contains(wire.blockId) && !dropped.contains(wire.blockId))
        {
            auto const* const record = store.find(vtbackend::CommandBlockId { wire.blockId });
            auto restorable = record != nullptr ? restorableRecord(*record) : std::nullopt;
            if (restorable)
            {
                kept.insert(wire.blockId);
                history.commandBlocks.push_back(toWireCommandBlock(*restorable, hostHeadOf(grid, *record)));
            }
            else
                dropped.insert(wire.blockId);
        }
        // A row never names a record that is not kept: a restored row would point at nothing.
        if (dropped.contains(wire.blockId))
            wire.blockId = 0;
        history.lines.push_back(std::move(wire));
    }

    for (auto const id: linkIds)
        if (auto const info = terminal.hyperlinks().hyperlinkById(vtbackend::HyperlinkId { id }))
            history.hyperlinks.push_back(proto::HyperlinkEntry { .id = id, .uri = info->uri });
    return history;
}

WorkspaceSnapshot captureWorkspace(SessionHost& host,
                                   CaptureOptions const& options,
                                   std::chrono::sys_seconds now)
{
    auto snapshot = WorkspaceSnapshot { .takenAt = now, .cellPixelSize = host.learnedCellPixelSize() };
    auto const& model = host.model();
    for (auto const workspaceIndex: std::views::iota(0, model.workspaceCount()))
    {
        auto const* const workspace = model.workspaceAt(workspaceIndex);
        if (workspace == nullptr || workspace->empty())
            continue;
        auto captured = WorkspaceLayoutSnapshot {
            .identity = identityOf(
                host.objects(), vtcontrol::ObjectKind::Workspace, host.objects().uuidOf(workspace->id())),
            .color = workspace->color(),
        };
        auto const active = std::max(workspace->activeTabIndex(), 0);
        auto capturedBeforeActive = std::size_t { 0 };
        for (auto const tabIndex: std::views::iota(0, workspace->tabCount()))
        {
            auto const* const tab = workspace->tabAt(tabIndex);
            // A served tab is left out: its extension opens it again, and a restore would bring it back as a
            // shell.
            if (tab == nullptr || tab->rootPane() == nullptr || holdsServedLeaf(host, *tab->rootPane()))
                continue;
            auto tree = TreeCapture { host, options, tab->activePane() };
            auto root = tree.capture(*tab->rootPane());
            // A tab of agent panes alone is left out as a served one is.
            if (!root)
                continue;
            if (tabIndex < active)
                ++capturedBeforeActive;
            auto entry = TabSnapshot {};
            entry.identity =
                identityOf(host.objects(), vtcontrol::ObjectKind::Tab, host.objects().uuidOf(tab->id()));
            entry.color = tab->color(vtworkspace::TabColorSource::User);
            entry.root = std::move(*root);
            entry.activeLeaf = tree.activeLeaf();
            entry.zoom = tab->isZoomed() ? TabZoom::Zoomed : TabZoom::Tiled;
            captured.tabs.push_back(std::move(entry));
        }
        if (captured.tabs.empty())
            continue;
        captured.activeTab = activeTabAmongCaptured(capturedBeforeActive, captured.tabs.size());
        snapshot.workspaces.push_back(std::move(captured));
    }
    return snapshot;
}

} // namespace vthost
