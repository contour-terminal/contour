// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/TmuxClientModel.hpp>

#include <charconv>
#include <chrono>
#include <format>
#include <utility>

namespace vthost::tmux
{

namespace
{
    /// The scrollback a replay terminal keeps when the consumer injected no sink of its own.
    ///
    /// Its own number, deliberately not `vthost::DefaultSessionHistoryLineCount`: that constant is
    /// justified entirely by `Grid::stableRangeFloor()` and the native protocol's delta
    /// addressability, none of which applies to a terminal fed raw bytes. The reason here is
    /// simply that `capture-pane -S -` fetches the remote pane's whole history, and a replay
    /// terminal with no scrollback would drop all but the visible page of it. It matches tmux's
    /// own `history-limit` default order of magnitude, which is what the replay actually carries.
    ///
    /// A consumer that cares — the GUI, which backs each pane with its profile's terminal —
    /// supplies a PaneSinkFactory and never constructs a PaneView at all.
    constexpr auto DefaultReplayHistoryLines = vtbackend::LineCount(2000);
} // namespace

// ---------------------------------------------------------------------------
// PaneView

PaneView::PaneView(int columns, int lines, vtbackend::LineCount history)
{
    auto const pageSize =
        vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) };
    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    settings.historyLimits = vtbackend::HistoryLimits::plain(history);
    // The process's own environment: this terminal only mirrors what a remote pane already
    // rendered, so nothing it reads from there describes anything but the client it runs in.
    _terminal = std::make_unique<vtbackend::Terminal>(_events,
                                                      crispy::defaultEnvironment(),
                                                      std::make_unique<vtpty::MockPty>(pageSize),
                                                      std::move(settings),
                                                      std::chrono::steady_clock::now());
}

void PaneView::feed(std::string_view bytes)
{
    _terminal->writeToScreen(bytes);
}

void PaneView::resize(int columns, int lines)
{
    _terminal->resizeScreen(
        vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) });
}

std::string PaneView::pageText() const
{
    return _terminal->primaryScreen().renderMainPageText();
}

// ---------------------------------------------------------------------------
// TmuxClientModel

namespace
{
    /// Collects a parsed layout's leaves in layout order.
    void collectLeaves(ParsedLayout const& node, std::vector<ParsedLayout const*>& out)
    {
        if (node.kind == ParsedLayout::Kind::Leaf)
        {
            out.push_back(&node);
            return;
        }
        for (auto const& child: node.children)
            collectLeaves(child, out);
    }
} // namespace

TmuxClientModel::PaneEntry* TmuxClientModel::entryFor(uint64_t pane) noexcept
{
    if (auto const it = _panes.find(pane); it != _panes.end())
        return &it->second;
    // A pane parked in `_detached` is LIVE, not gone: its sink and its replay bookkeeping are
    // sitting there waiting for the destination window's %layout-change to reclaim them (@see
    // detachPane). Resolving a pane through `_panes` alone therefore discarded everything that
    // arrived during a break-pane/join-pane — which is precisely the window a move opens, and
    // precisely what the parking exists to survive.
    if (auto const it = _detached.find(pane); it != _detached.end())
        return &it->second;
    return nullptr;
}

PaneView* TmuxClientModel::pane(uint64_t id) noexcept
{
    auto* entry = entryFor(id);
    return entry != nullptr ? dynamic_cast<PaneView*>(entry->sink.get()) : nullptr;
}

void TmuxClientModel::outputReceived(uint64_t pane, std::string_view bytes)
{
    auto* entry = entryFor(pane);
    if (entry == nullptr)
        return; // output for a pane whose layout has not arrived yet
    if (!entry->replayed)
    {
        entry->pendingOutput.append(bytes);
        return;
    }
    entry->sink->feed(bytes);
}

void TmuxClientModel::layoutChanged(uint64_t window, std::string_view layout)
{
    ingestLayout(window, layout);
}

void TmuxClientModel::windowAdded(uint64_t window)
{
    if (_windows.try_emplace(window).second) // the layout arrives with %layout-change
        for (auto* observer: _observers)
            observer->windowAdded(window);
}

void TmuxClientModel::windowClosed(uint64_t window)
{
    auto const it = _windows.find(window);
    if (it == _windows.end())
        return;
    for (auto const paneId: it->second.panes)
    {
        auto const pane = _panes.find(paneId);
        if (pane == _panes.end() || pane->second.window != window)
            continue; // moved to another window before this close arrived
        _panes.erase(pane);
        for (auto* observer: _observers)
            observer->paneRemoved(window, paneId);
    }
    _windows.erase(it);
    for (auto* observer: _observers)
        observer->windowClosed(window);
}

void TmuxClientModel::windowRenamed(uint64_t window, std::string_view name)
{
    // find, not operator[]: a rename for a window we have not ingested yet (its
    // %window-add / %layout-change has not arrived) must NOT default-insert a
    // treeless phantom window and notify the frontend to rename a tab that does
    // not exist. The name lands once the window is genuinely added.
    auto const it = _windows.find(window);
    if (it == _windows.end())
        return;
    it->second.name = std::string { name };
    for (auto* observer: _observers)
        observer->windowRenamed(window, it->second.name);
}

void TmuxClientModel::panePaused(uint64_t pane, bool paused)
{
    for (auto* observer: _observers)
        observer->panePaused(pane, paused);
}

void TmuxClientModel::exited(std::string_view reason)
{
    auto const copy = std::string { reason };
    for (auto* observer: _observers)
        observer->exited(copy);
}

void TmuxClientModel::notificationsDrained()
{
    // The burst that arrived together is fully applied: any pane still parked was
    // not reclaimed by a destination %layout-change, so it is a genuine close.
    reconcileDetached();
}

void TmuxClientModel::sessionChanged(uint64_t /*session*/, std::string_view /*name*/)
{
    if (_gateway == nullptr)
        return;
    // Enumerate the session's windows and ingest their layouts; new panes
    // trigger their history replay from ingestLayout.
    _gateway->sendCommand("list-windows -F \"#{window_id} #{window_layout}\"",
                          [this](bool ok, std::vector<std::string> const& body) {
                              if (!ok)
                                  return;
                              for (auto const& line: body)
                              {
                                  auto const space = line.find(' ');
                                  if (space == std::string::npos || !line.starts_with('@'))
                                      continue;
                                  auto window = uint64_t {};
                                  auto const idText = std::string_view { line }.substr(1, space - 1);
                                  auto const [ptr, ec] =
                                      std::from_chars(idText.data(), idText.data() + idText.size(), window);
                                  if (ec != std::errc {} || ptr != idText.data() + idText.size())
                                      continue;
                                  ingestLayout(window, std::string_view { line }.substr(space + 1));
                              }
                          });
}

void TmuxClientModel::ingestLayout(uint64_t window, std::string_view layout)
{
    auto parsed = parseLayout(layout);
    if (!parsed)
        return; // a malformed layout leaves the previous state standing

    auto leaves = std::vector<ParsedLayout const*> {};
    collectLeaves(*parsed, leaves);

    auto& view = _windows[window];
    view.layout = std::string { layout };
    view.tree = std::make_unique<BinaryLayout>(collapseToBinary(*parsed));

    auto previous = std::exchange(view.panes, {});
    for (auto const* leaf: leaves)
    {
        if (!leaf->paneId)
            continue;
        auto const paneId = *leaf->paneId;
        view.panes.push_back(paneId);
        std::erase(previous, paneId);

        if (auto const it = _panes.find(paneId); it != _panes.end())
        {
            if (it->second.window != window)
            {
                // Already adopted here from another window whose stale
                // layout-change has not yet arrived (destination-first order).
                auto const from = std::exchange(it->second.window, window);
                if (auto const src = _windows.find(from); src != _windows.end())
                    std::erase(src->second.panes, paneId);
                for (auto* observer: _observers)
                    observer->paneMoved(from, window, paneId);
            }
            it->second.sink->resize(leaf->width, leaf->height);
            continue;
        }
        if (auto const det = _detached.find(paneId); det != _detached.end())
        {
            // The source window dropped this pane first (source-first order);
            // reclaim its live terminal and buffered output for its new home.
            auto const from = det->second.window;
            auto node = _detached.extract(det);
            node.mapped().window = window;
            node.mapped().sink->resize(leaf->width, leaf->height);
            _panes.insert(std::move(node));
            for (auto* observer: _observers)
                observer->paneMoved(from, window, paneId);
            continue;
        }
        auto sink = _sinkFactory
                        ? _sinkFactory(paneId, leaf->width, leaf->height)
                        : std::make_unique<PaneView>(leaf->width, leaf->height, DefaultReplayHistoryLines);
        auto entry = PaneEntry {
            .sink = std::move(sink), .window = window, .replayed = _gateway == nullptr, .pendingOutput = {}
        };
        _panes.emplace(paneId, std::move(entry));
        for (auto* observer: _observers)
            observer->paneAdded(window, paneId, leaf->width, leaf->height);
        if (_gateway != nullptr)
            replayHistory(paneId);
    }

    // Panes parked by an earlier layout-change that this one did not reclaim
    // are confirmed closed (a move would have re-listed them above).
    reconcileDetached();

    // Panes gone from this window may have closed OR moved to a sibling whose
    // layout-change has not arrived yet: park them for the reclaim above.
    for (auto const paneId: previous)
        detachPane(paneId, window);

    for (auto* observer: _observers)
        observer->layoutTreeChanged(window);
}

void TmuxClientModel::detachPane(uint64_t pane, uint64_t window)
{
    auto const it = _panes.find(pane);
    if (it == _panes.end() || it->second.window != window)
        return; // never tracked here, or already adopted elsewhere
    _detached.insert(_panes.extract(it));
}

void TmuxClientModel::reconcileDetached()
{
    if (_detached.empty())
        return;
    auto const stale = std::exchange(_detached, {});
    for (auto const& [paneId, entry]: stale)
        for (auto* observer: _observers)
            observer->paneRemoved(entry.window, paneId);
}

std::string joinReplayRows(std::vector<std::string> const& rows)
{
    auto replay = std::string {};
    auto needed = std::size_t { 0 };
    for (auto const& row: rows)
        needed += row.size() + 2;
    replay.reserve(needed);

    auto first = true;
    for (auto const& row: rows)
    {
        if (!std::exchange(first, false))
            replay += "\r\n";
        replay += row;
    }
    return replay;
}

void TmuxClientModel::replayHistory(uint64_t pane)
{
    // capture-pane: -p print, -e with escapes (SGR carries across lines),
    // -q quiet, -J join wrapped lines. Text + SGR only — tmux serializes no
    // images; live %output does carry them (inherited tmux limitation).
    //
    // -J is kept knowing it also preserves each row's trailing spaces (tmux documents the two
    // together): joining is what lets the local terminal re-derive wrap state by re-wrapping the
    // joined logical line itself, which no other flag buys. The padding it brings along is what
    // every tmux client that passes -J already renders.
    //
    // -S - starts at the beginning of the pane's history rather than at the top of the visible
    // page, which is the default. Without it a mirrored pane opened with nothing but the current
    // screen, and everything the session had scrolled away before we attached was unreachable —
    // while our OWN control-mode server answers `capture-pane -S -` for tmux clients (@see
    // ControlSession::commandCapturePane), so the two directions disagreed about what an attach
    // replays.
    // The rows below the page cost one capture, and the feed loop scrolls them through the local
    // page into real scrollback exactly as the page rows go.
    _gateway->sendCommand(std::format("capture-pane -peqJ -S - -t %{}", pane),
                          [this, pane](bool ok, std::vector<std::string> const& body) {
                              // Through entryFor: a pane re-parented while its capture was in
                              // flight is parked, not closed, and its replay must still complete —
                              // otherwise it stays flagged un-replayed and buffers live output for
                              // ever.
                              auto* entry = entryFor(pane);
                              if (entry == nullptr)
                                  return; // closed while the capture was in flight
                              // Assembled and fed as ONE buffer rather than two calls per row. A
                              // sink call is not cheap on either implementation — PaneView takes the
                              // terminal lock and publishes a screen update; the GUI's PaneFeed
                              // takes two mutexes and wakes the parser thread — and `-S -` turned a
                              // page's worth of rows into a history's, which would be thousands of
                              // locks and wakeups per attach. @see joinReplayRows for the join.
                              if (ok)
                                  entry->sink->feed(joinReplayRows(body));
                              // Only now does buffered live output land on top. Exchanged rather
                              // than cleared: the buffer's peak is whatever arrived during the
                              // capture round trip, which `-S -` made a good deal longer, and
                              // clear() would keep that capacity for the pane's whole life.
                              entry->replayed = true;
                              entry->sink->feed(std::exchange(entry->pendingOutput, {}));
                          });
}

} // namespace vthost::tmux
