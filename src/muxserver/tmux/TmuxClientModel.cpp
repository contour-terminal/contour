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
    return it != _panes.end() ? it->second.view.get() : nullptr;
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
    it->second.view->feed(bytes);
}

void TmuxClientModel::layoutChanged(uint64_t window, std::string_view layout)
{
    ingestLayout(window, layout);
}

void TmuxClientModel::windowAdded(uint64_t window)
{
    _windows.try_emplace(window); // the layout arrives with %layout-change
}

void TmuxClientModel::windowClosed(uint64_t window)
{
    auto const it = _windows.find(window);
    if (it == _windows.end())
        return;
    for (auto const paneId: it->second.panes)
        _panes.erase(paneId);
    _windows.erase(it);
}

void TmuxClientModel::windowRenamed(uint64_t window, std::string_view name)
{
    _windows[window].name = std::string { name };
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
            it->second.view->resize(leaf->width, leaf->height);
            continue;
        }
        auto entry = PaneEntry { .view = std::make_unique<PaneView>(leaf->width, leaf->height),
                                 .replayed = _gateway == nullptr,
                                 .pendingOutput = {} };
        _panes.emplace(*leaf->paneId, std::move(entry));
        if (_gateway != nullptr)
            replayHistory(*leaf->paneId);
    }

    // Leaves gone from the layout are closed panes.
    for (auto const paneId: previous)
        _panes.erase(paneId);
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
                                          it->second.view->feed("\r\n");
                                      it->second.view->feed(line);
                                      first = false;
                                  }
                              }
                              // Only now does buffered live output land on top.
                              it->second.replayed = true;
                              it->second.view->feed(it->second.pendingOutput);
                              it->second.pendingOutput.clear();
                          });
}

} // namespace muxserver::tmux
