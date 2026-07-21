// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/ControlSession.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <ranges>
#include <utility>

#include <muxserver/tmux/LayoutString.h>
#include <vtmux/Pane.h>
#include <vtmux/Tab.h>

namespace muxserver::tmux
{

using vtmux::Pane;
using vtmux::PaneId;
using vtmux::SessionId;
using vtmux::SplitState;
using vtmux::Tab;
using vtmux::TabId;
using vtmux::WindowId;

std::vector<std::string> splitCommandLine(std::string_view line)
{
    auto arguments = std::vector<std::string> {};
    auto current = std::string {};
    auto inWord = false;
    auto quote = '\0';

    for (std::size_t i = 0; i < line.size(); ++i)
    {
        auto const ch = line[i];
        if (quote != '\0')
        {
            if (ch == quote)
                quote = '\0';
            else if (quote == '"' && ch == '\\' && i + 1 < line.size()
                     && (line[i + 1] == '"' || line[i + 1] == '\\'))
                current += line[++i];
            else
                current += ch;
            continue;
        }
        if (ch == '\'' || ch == '"')
        {
            quote = ch;
            inWord = true;
            continue;
        }
        if (ch == ' ' || ch == '\t')
        {
            if (inWord)
            {
                arguments.push_back(std::move(current));
                current.clear();
                inWord = false;
            }
            continue;
        }
        current += ch;
        inWord = true;
    }
    if (inWord)
        arguments.push_back(std::move(current));
    return arguments;
}

namespace
{
    /// Extracts the value of a `-t <target>` option, if present.
    [[nodiscard]] std::optional<std::string_view> targetOf(std::vector<std::string> const& arguments)
    {
        for (std::size_t i = 1; i + 1 < arguments.size(); ++i)
            if (arguments[i] == "-t")
                return arguments[i + 1];
        return std::nullopt;
    }

    [[nodiscard]] bool hasFlag(std::vector<std::string> const& arguments, std::string_view flag)
    {
        return std::ranges::find(arguments, flag) != arguments.end();
    }

    [[nodiscard]] std::optional<std::uint64_t> parseIdSuffix(std::string_view target, char sigil)
    {
        if (target.size() < 2 || target[0] != sigil)
            return std::nullopt;
        auto value = std::uint64_t {};
        auto const* begin = target.data() + 1;
        auto const* end = target.data() + target.size();
        auto const [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc {} || ptr != end)
            return std::nullopt;
        return value;
    }

    /// The tiny send-keys key-name table; everything else is sent literally.
    struct NamedKey
    {
        std::string_view name;
        std::string_view bytes;
    };

    constexpr auto NamedKeys = std::array {
        NamedKey { "Enter", "\r" },    NamedKey { "Escape", "\x1b" },  NamedKey { "Tab", "\t" },
        NamedKey { "Space", " " },     NamedKey { "BSpace", "\x7f" },  NamedKey { "Up", "\x1b[A" },
        NamedKey { "Down", "\x1b[B" }, NamedKey { "Right", "\x1b[C" }, NamedKey { "Left", "\x1b[D" },
    };

    [[nodiscard]] std::string translateKey(std::string_view key)
    {
        for (auto const& entry: NamedKeys)
            if (entry.name == key)
                return { std::string_view { entry.bytes }.begin(), std::string_view { entry.bytes }.end() };
        // C-x control chords.
        if (key.size() == 3 && key.starts_with("C-"))
        {
            auto const base = static_cast<char>(std::tolower(static_cast<unsigned char>(key[2])));
            if (base >= 'a' && base <= 'z')
                return { 1, static_cast<char>(base - 'a' + 1) };
        }
        return { key.begin(), key.end() };
    }
} // namespace

ControlSession::ControlSession(net::EventLoop& loop,
                               SessionHost& host,
                               std::unique_ptr<net::ISocket> connection,
                               std::function<std::int64_t()> wallClock):
    _loop(loop),
    _host(host),
    _connection(std::move(connection)),
    _wallClock(std::move(wallClock)),
    _writer(loop, _connection.get(), std::size_t { 256 } * 1024),
    _output([this](std::string line) { std::ignore = _writer.enqueue(std::move(line)); })
{
    _host.subscribe(this);
}

ControlSession::~ControlSession()
{
    _host.unsubscribe(this);
}

vtpty::PageSize ControlSession::pageSize() const noexcept
{
    // All layout projection uses the daemon's page size; per-client resize
    // arrives with refresh-client -C in a later slice.
    auto const* tab = _host.model().window(_host.windowId())->activeTab();
    if (tab != nullptr)
        if (auto const* terminal = _host.terminal(tab->rootPane()->session()))
            return terminal->pageSize();
    return vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };
}

coro::Task<void> ControlSession::run()
{
    // The implicit initial command's empty guard pair, then the session state —
    // the preamble iTerm2-style clients gate their notification handling on.
    emitGuarded(HandlerResult { std::vector<std::string> {} });
    _output.enqueueNotification("%session-changed $0 0");

    auto reader = net::AsyncBufferedReader { _connection.get() };
    while (true)
    {
        auto line = co_await reader.readLine();
        if (!line.has_value())
            break; // disconnect or poisoned stream
        if (line->empty())
            break; // an empty line detaches (control.c:547)
        dispatch(*line);
    }

    _output.enqueueNotification("%exit");
    // Let the write queue flush %exit (and any trailing replies) before tearing
    // the connection down: close() drops the backlog, so a bare close here would
    // race the spawned drain and lose the final lines.
    while (_writer.queuedBytes() > 0 || _writer.draining())
        co_await _loop.delay(std::chrono::milliseconds { 1 });
    _writer.close();
    _connection->close();
}

void ControlSession::dispatch(std::string_view line)
{
    auto const arguments = splitCommandLine(line);
    if (arguments.empty())
    {
        emitGuarded(std::unexpected("empty command"));
        return;
    }

    auto const& catalog = commandCatalog();
    auto const entry = std::ranges::find_if(
        catalog, [&](CommandEntry const& command) { return command.name == arguments.front(); });
    if (entry == catalog.end())
    {
        emitGuarded(std::unexpected(std::format("unknown command: {}", arguments.front())));
        return;
    }

    emitGuarded((this->*(entry->handler))(arguments));
}

void ControlSession::emitGuarded(HandlerResult const& result)
{
    auto const time = _wallClock();
    auto const number = _commandNumber++;
    // flags bit 0 (client-originated) is SET on guard blocks answering client
    // commands (cmd-queue.c:591); spontaneous output never uses guards.
    _output.enqueueNotification(std::format("%begin {} {} 1", time, number));
    if (result.has_value())
        for (auto const& line: *result)
            _output.enqueueNotification(line);
    else
        _output.enqueueNotification(result.error());
    _output.enqueueNotification(
        std::format("{} {} {} 1", result.has_value() ? "%end" : "%error", time, number));
}

void ControlSession::onSessionOutput(SessionId session, std::string const& bytes)
{
    // Map the session to its hosting leaf pane (the %N the client knows).
    auto* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto* tab = window->tabAt(tabIndex);
        auto paneId = std::optional<std::uint64_t> {};
        tab->rootPane()->walkTree([&](Pane& pane) {
            if (pane.isLeaf() && pane.session() == session)
                paneId = pane.id().value;
        });
        if (paneId)
        {
            _output.enqueueOutput(*paneId, bytes, std::chrono::steady_clock::now());
            _output.pump(_writer.queuedBytes(), std::chrono::steady_clock::now());
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Notifications

void ControlSession::tabAdded(WindowId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%window-add @{}", tab.value));
    notifyLayoutChanged(tab);
}

void ControlSession::tabClosed(WindowId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%window-close @{}", tab.value));
}

void ControlSession::tabMoved(WindowId /*window*/, TabId /*tab*/, int /*fromIndex*/, int /*toIndex*/)
{
    _output.enqueueNotification("%sessions-changed");
}

void ControlSession::activeTabChanged(WindowId /*window*/, TabId tab, int /*index*/)
{
    _output.enqueueNotification(std::format("%session-window-changed $0 @{}", tab.value));
}

void ControlSession::paneSplit(TabId tab, PaneId /*splitNode*/, PaneId /*newLeaf*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::paneClosed(TabId tab, PaneId /*closed*/, PaneId /*survivor*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::activePaneChanged(TabId /*tab*/, PaneId /*leaf*/)
{
    // Focus is client-local in control mode; nothing to notify.
}

void ControlSession::paneRatioChanged(TabId tab, PaneId /*splitNode*/, double /*ratio*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::tabTitleChanged(TabId tab)
{
    auto const* tabPtr = _host.model().findTab(tab);
    auto const title = tabPtr != nullptr ? tabPtr->runtimeTitle().value_or("") : std::string {};
    _output.enqueueNotification(std::format("%window-renamed @{} {}", tab.value, title));
}

void ControlSession::tabColorChanged(TabId /*tab*/)
{
    // No control-mode notification exists for window colors.
}

void ControlSession::paneTreeRestructured(TabId tab)
{
    notifyLayoutChanged(tab);
}

void ControlSession::paneZoomChanged(TabId tab, std::optional<PaneId> /*zoomedLeaf*/)
{
    notifyLayoutChanged(tab);
}

void ControlSession::notifyLayoutChanged(TabId tab)
{
    auto const* tabPtr = _host.model().findTab(tab);
    if (tabPtr == nullptr)
        return;
    auto const layout = encodeLayout(*tabPtr->rootPane(), pageSize());
    auto const visible = encodeLayout(*tabPtr->layoutRoot(), pageSize());
    // The trailing #{window_raw_flags} expands empty here — the trailing space
    // is part of the wire format (control-notify.c:77-78).
    _output.enqueueNotification(std::format("%layout-change @{} {} {} ", tab.value, layout, visible));
}

// ---------------------------------------------------------------------------
// Commands

std::vector<ControlSession::CommandEntry> const& ControlSession::commandCatalog()
{
    static auto const catalog = std::vector<CommandEntry> {
        { "capture-pane", &ControlSession::commandCapturePane },
        { "display-message", &ControlSession::commandDisplayMessage },
        { "kill-pane", &ControlSession::commandKillPane },
        { "list-panes", &ControlSession::commandListPanes },
        { "list-sessions", &ControlSession::commandListSessions },
        { "list-windows", &ControlSession::commandListWindows },
        { "new-window", &ControlSession::commandNewWindow },
        { "rename-window", &ControlSession::commandRenameWindow },
        { "resize-pane", &ControlSession::commandResizePane },
        { "select-pane", &ControlSession::commandSelectPane },
        { "select-window", &ControlSession::commandSelectWindow },
        { "send-keys", &ControlSession::commandSendKeys },
        { "split-window", &ControlSession::commandSplitWindow },
    };
    return catalog;
}

std::expected<Tab*, std::string> ControlSession::resolveTab(std::vector<std::string> const& arguments) const
{
    auto* window = _host.model().window(_host.windowId());
    if (auto const target = targetOf(arguments))
    {
        auto const id = parseIdSuffix(*target, '@');
        if (!id)
            return std::unexpected(std::format("bad window target: {}", *target));
        auto* tab = _host.model().findTab(TabId { *id });
        if (tab == nullptr)
            return std::unexpected(std::format("window not found: {}", *target));
        return tab;
    }
    auto* active = window->activeTab();
    if (active == nullptr)
        return std::unexpected("no current window");
    return active;
}

std::expected<Pane*, std::string> ControlSession::resolvePane(std::vector<std::string> const& arguments) const
{
    if (auto const target = targetOf(arguments))
    {
        auto const id = parseIdSuffix(*target, '%');
        if (!id)
            return std::unexpected(std::format("bad pane target: {}", *target));
        auto* window = _host.model().window(_host.windowId());
        for (auto const tabIndex: std::views::iota(0, window->tabCount()))
        {
            Pane* found = nullptr;
            window->tabAt(tabIndex)->rootPane()->walkTree([&](Pane& pane) {
                if (pane.isLeaf() && pane.id().value == *id)
                    found = &pane;
            });
            if (found != nullptr)
                return found;
        }
        return std::unexpected(std::format("pane not found: {}", *target));
    }
    auto* active = _host.model().window(_host.windowId())->activeTab();
    if (active == nullptr || active->activePane() == nullptr)
        return std::unexpected("no current pane");
    return active->activePane();
}

ControlSession::HandlerResult ControlSession::commandListSessions(std::vector<std::string> const& /*args*/)
{
    auto const* window = _host.model().window(_host.windowId());
    return std::vector<std::string> { std::format("0: 0 [{} windows] (attached)", window->tabCount()) };
}

ControlSession::HandlerResult ControlSession::commandListWindows(std::vector<std::string> const& /*args*/)
{
    auto lines = std::vector<std::string> {};
    auto const* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto const* tab = window->tabAt(tabIndex);
        lines.push_back(std::format("{}: @{} {} [{}]",
                                    tabIndex,
                                    tab->id().value,
                                    tab->runtimeTitle().value_or(""),
                                    encodeLayout(*tab->rootPane(), pageSize())));
    }
    return lines;
}

ControlSession::HandlerResult ControlSession::commandListPanes(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    auto lines = std::vector<std::string> {};
    auto index = 0;
    (*tab)->rootPane()->walkTree([&](Pane& pane) {
        if (pane.isLeaf())
            lines.push_back(std::format("{}: %{}", index++, pane.id().value));
    });
    return lines;
}

ControlSession::HandlerResult ControlSession::commandNewWindow(std::vector<std::string> const& /*args*/)
{
    auto* tab = _host.createTab();
    if (tab == nullptr)
        return std::unexpected("create window failed");
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSplitWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    // tmux: -h splits left|right (our Vertical), -v (default) stacks (Horizontal).
    auto const orientation = hasFlag(arguments, "-h") ? SplitState::Vertical : SplitState::Horizontal;
    auto const panesBefore = (*tab)->paneCount();
    _host.splitActivePane((*tab)->id(), orientation, 0.5);
    if ((*tab)->paneCount() == panesBefore)
        return std::unexpected("split failed");
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandKillPane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    _host.handleSessionExit((*pane)->session());
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSelectPane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* window = _host.model().window(_host.windowId());
    for (auto const tabIndex: std::views::iota(0, window->tabCount()))
    {
        auto* tab = window->tabAt(tabIndex);
        auto contains = false;
        tab->rootPane()->walkTree([&](Pane& node) {
            if (&node == *pane)
                contains = true;
        });
        if (contains)
        {
            _host.model().setActivePane(tab->id(), (*pane)->id());
            return std::vector<std::string> {};
        }
    }
    return std::unexpected("pane not found");
}

ControlSession::HandlerResult ControlSession::commandSelectWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    _host.model().activateTab(_host.windowId(), (*tab)->id());
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandSendKeys(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* terminal = _host.terminal((*pane)->session());
    if (terminal == nullptr)
        return std::unexpected("pane has no live session");

    auto const literal = hasFlag(arguments, "-l");
    auto bytes = std::string {};
    auto skipNext = false;
    for (auto const& argument: arguments | std::views::drop(1))
    {
        if (skipNext)
        {
            skipNext = false;
            continue;
        }
        if (argument == "-t")
        {
            skipNext = true;
            continue;
        }
        if (argument == "-l")
            continue;
        bytes += literal ? argument : translateKey(argument);
    }

    if (!bytes.empty())
        std::ignore = terminal->device().write(bytes);
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandCapturePane(std::vector<std::string> const& arguments)
{
    auto pane = resolvePane(arguments);
    if (!pane)
        return std::unexpected(pane.error());
    auto* terminal = _host.terminal((*pane)->session());
    if (terminal == nullptr)
        return std::unexpected("pane has no live session");

    auto lines = std::vector<std::string> {};
    auto const text = terminal->primaryScreen().renderMainPageText();
    for (auto const line: std::views::split(text, '\n'))
        lines.emplace_back(std::string_view { line });
    // renderMainPageText ends each row with LF; drop the trailing empty piece.
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

ControlSession::HandlerResult ControlSession::commandRenameWindow(std::vector<std::string> const& arguments)
{
    auto tab = resolveTab(arguments);
    if (!tab)
        return std::unexpected(tab.error());
    auto name = std::string {};
    auto skipNext = false;
    for (auto const& argument: arguments | std::views::drop(1))
    {
        if (skipNext)
        {
            skipNext = false;
            continue;
        }
        if (argument == "-t")
        {
            skipNext = true;
            continue;
        }
        name = argument;
    }
    if (name.empty())
        return std::unexpected("usage: rename-window [-t @N] name");
    _host.model().setTabTitle((*tab)->id(), name);
    return std::vector<std::string> {};
}

ControlSession::HandlerResult ControlSession::commandResizePane(std::vector<std::string> const& arguments)
{
    if (hasFlag(arguments, "-Z"))
    {
        auto tab = resolveTab(arguments);
        if (!tab)
            return std::unexpected(tab.error());
        _host.model().toggleActivePaneZoom((*tab)->id());
        return std::vector<std::string> {};
    }
    return std::unexpected("only resize-pane -Z is supported");
}

ControlSession::HandlerResult ControlSession::commandDisplayMessage(std::vector<std::string> const& arguments)
{
    auto message = std::string {};
    for (auto const& argument: arguments | std::views::drop(1))
    {
        if (argument == "-p")
            continue;
        if (!message.empty())
            message += ' ';
        message += argument;
    }
    return std::vector<std::string> { message };
}

// ---------------------------------------------------------------------------

namespace
{
    /// One control client's whole lifetime, as a free coroutine (a capturing
    /// lambda coroutine would dangle its closure; pointers live in the frame).
    coro::Task<void> serveControlClient(net::EventLoop* loop,
                                        SessionHost* host,
                                        std::unique_ptr<net::ISocket> connection)
    {
        auto session = std::make_unique<ControlSession>(*loop, *host, std::move(connection), [] {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        });

        // Wire the byte tap for the lifetime of this connection. A single
        // control client is the common case; multi-client fan-out arrives with
        // the native protocol phase.
        host->setOutputHandler([&session = *session](SessionId id, std::string const& bytes) {
            session.onSessionOutput(id, bytes);
        });

        co_await session->run();
        host->setOutputHandler(nullptr);
    }
} // namespace

std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeControlModeHandler(net::EventLoop& loop,
                                                                                      SessionHost& host)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host](std::unique_ptr<net::ISocket> connection) {
        return serveControlClient(&loop, &host, std::move(connection));
    };
}

} // namespace muxserver::tmux
