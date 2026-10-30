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

    /// @return What @p objects names @p uuid's object by: its UUID and label; nullopt for one it does not
    /// name.
    [[nodiscard]] std::optional<ObjectIdentityRecord> identityOf(ObjectRegistry const& objects,
                                                                 vtcontrol::ObjectKind kind,
                                                                 std::optional<vtcontrol::Uuid> const& uuid)
    {
        if (!uuid)
            return std::nullopt;
        auto record = ObjectIdentityRecord { .uuid = uuid->bytes };
        if (auto const label = objects.labelOf(vtcontrol::ObjectRef { .kind = kind, .uuid = *uuid });
            !label.empty())
            record.label = std::string { label };
        return record;
    }

    /// Walks a pane tree depth first, first child first, converting it.
    class TreeCapture
    {
      public:
        TreeCapture(SessionHost& host, CaptureOptions const& options, vtworkspace::Pane const* active):
            _host { host }, _options { options }, _active { active }
        {
        }

        [[nodiscard]] PaneNode capture(vtworkspace::Pane const& pane)
        {
            auto node = PaneNode { .split = pane.splitState() };
            if (pane.isLeaf())
            {
                if (&pane == _active)
                    _activeLeaf = _leaves;
                ++_leaves;
                node.pane = capturePane(pane.session());
                return node;
            }
            node.ratio = pane.ratio();
            node.children.push_back(capture(*pane.first()));
            node.children.push_back(capture(*pane.second()));
            return node;
        }

        /// @return The leaf order position of the active pane; 0 when it was not met.
        [[nodiscard]] std::uint32_t activeLeaf() const noexcept { return _activeLeaf; }

      private:
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
    auto snapshot = WorkspaceSnapshot { .takenAt = now };
    auto const& model = host.model();
    for (auto const windowIndex: std::views::iota(0, model.windowCount()))
    {
        auto const* const window = model.windowAt(windowIndex);
        if (window == nullptr || window->empty())
            continue;
        auto captured =
            WindowSnapshot { .identity = identityOf(host.objects(),
                                                    vtcontrol::ObjectKind::Window,
                                                    host.objects().uuidOf(window->id())),
                             .activeTab = static_cast<std::uint32_t>(std::max(window->activeTabIndex(), 0)) };
        for (auto const tabIndex: std::views::iota(0, window->tabCount()))
        {
            auto const* const tab = window->tabAt(tabIndex);
            if (tab == nullptr || tab->rootPane() == nullptr)
                continue;
            auto tree = TreeCapture { host, options, tab->activePane() };
            auto entry = TabSnapshot {};
            entry.identity =
                identityOf(host.objects(), vtcontrol::ObjectKind::Tab, host.objects().uuidOf(tab->id()));
            entry.title = tab->runtimeTitle();
            entry.color = tab->color(vtworkspace::TabColorSource::User);
            entry.root = tree.capture(*tab->rootPane());
            entry.activeLeaf = tree.activeLeaf();
            entry.zoom = tab->isZoomed() ? TabZoom::Zoomed : TabZoom::Tiled;
            captured.tabs.push_back(std::move(entry));
        }
        if (!captured.tabs.empty())
            snapshot.windows.push_back(std::move(captured));
    }
    return snapshot;
}

} // namespace vthost
