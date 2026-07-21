// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/TmuxClientModel.h>

#include <charconv>
#include <chrono>
#include <format>
#include <utility>

namespace muxserver::tmux
{

// ---------------------------------------------------------------------------
// PaneView

PaneView::PaneView(int columns, int lines)
{
    auto const pageSize =
        vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) };
    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    _terminal = std::make_unique<vtbackend::Terminal>(_events,
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

PaneView* TmuxClientModel::pane(uint64_t id) noexcept
{
    auto const it = _panes.find(id);
    return it != _panes.end() ? dynamic_cast<PaneView*>(it->second.sink.get()) : nullptr;
}

void TmuxClientModel::outputReceived(uint64_t pane, std::string_view bytes)
{
    auto const it = _panes.find(pane);
    if (it == _panes.end())
        return; // output for a pane whose layout has not arrived yet
    if (!it->second.replayed)
    {
        it->second.pendingOutput.append(bytes);
        return;
    }
    it->second.sink->feed(bytes);
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
        _panes.erase(paneId);
        for (auto* observer: _observers)
            observer->paneRemoved(window, paneId);
    }
    _windows.erase(it);
    for (auto* observer: _observers)
        observer->windowClosed(window);
}

void TmuxClientModel::windowRenamed(uint64_t window, std::string_view name)
{
    _windows[window].name = std::string { name };
    for (auto* observer: _observers)
        observer->windowRenamed(window, _windows[window].name);
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
        view.panes.push_back(*leaf->paneId);
        std::erase(previous, *leaf->paneId);

        auto const it = _panes.find(*leaf->paneId);
        if (it != _panes.end())
        {
            it->second.sink->resize(leaf->width, leaf->height);
            continue;
        }
        auto sink = _sinkFactory ? _sinkFactory(*leaf->paneId, leaf->width, leaf->height)
                                 : std::make_unique<PaneView>(leaf->width, leaf->height);
        auto entry =
            PaneEntry { .sink = std::move(sink), .replayed = _gateway == nullptr, .pendingOutput = {} };
        _panes.emplace(*leaf->paneId, std::move(entry));
        for (auto* observer: _observers)
            observer->paneAdded(window, *leaf->paneId, leaf->width, leaf->height);
        if (_gateway != nullptr)
            replayHistory(*leaf->paneId);
    }

    // Leaves gone from the layout are closed panes.
    for (auto const paneId: previous)
    {
        _panes.erase(paneId);
        for (auto* observer: _observers)
            observer->paneRemoved(window, paneId);
    }

    for (auto* observer: _observers)
        observer->layoutTreeChanged(window);
}

void TmuxClientModel::replayHistory(uint64_t pane)
{
    // capture-pane: -p print, -e with escapes (SGR carries across lines),
    // -q quiet, -J join wrapped lines. Text + SGR only — tmux serializes no
    // images; live %output does carry them (inherited tmux limitation).
    _gateway->sendCommand(std::format("capture-pane -peqJ -t %{}", pane),
                          [this, pane](bool ok, std::vector<std::string> const& body) {
                              auto const it = _panes.find(pane);
                              if (it == _panes.end())
                                  return; // closed while the capture was in flight
                              if (ok)
                              {
                                  // Rows joined BETWEEN lines only: a trailing
                                  // newline after the last page row would
                                  // scroll the first one off the page.
                                  auto first = true;
                                  for (auto const& line: body)
                                  {
                                      if (!first)
                                          it->second.sink->feed("\r\n");
                                      it->second.sink->feed(line);
                                      first = false;
                                  }
                              }
                              // Only now does buffered live output land on top.
                              it->second.replayed = true;
                              it->second.sink->feed(it->second.pendingOutput);
                              it->second.pendingOutput.clear();
                          });
}

} // namespace muxserver::tmux
