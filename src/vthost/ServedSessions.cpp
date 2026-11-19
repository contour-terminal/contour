// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServedSessions.hpp>

#include <vtbackend/grid/GraphemeClusters.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/RepeatedText.hpp>
#include <vthost/RevealOnLoop.hpp>
#include <vthost/SessionHost.hpp>

namespace vthost
{

namespace
{
    /// What a host frame starts with: synchronized output on, scrolling margins, rendition, cursor and the
    /// mouse and focus reports the extension may have turned on reset, and the page erased from its home --
    /// ED 0, not ED 2, which would push a copy of the page into the scrollback with every frame.
    constexpr auto HostFrameOpening = std::string_view {
        "\x1b[?2026h\x1b[r\x1b[0m\x1b[?25l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1004l\x1b[?2004l\x1b[H\x1b[J"
    };

    /// What it ends with: synchronized output off, which presents it.
    constexpr auto HostFrameClosing = std::string_view { "\x1b[?2026l" };

    /// The lines of the host's frames, each a format of the extension's title. The stopped frame's middle
    /// line is its fault, as it is; its last names the palette row
    /// (vthost::proto::RestartExtensionRowFormat).
    constexpr auto StoppedFrameTitle = std::string_view { "{} stopped after repeated errors" };
    constexpr auto StoppedFrameRestart =
        std::string_view { "Restart it from the command palette: \u201C{}\u201D." };
    constexpr auto RestartingFrameTitle = std::string_view { "{} restarting\u2026" };

    /// The columns a box adds around a line: a border and a space on each side.
    constexpr auto HostBoxPadding = vtbackend::ColumnCount(4);

    /// The lines a box adds: its top and bottom border.
    constexpr auto HostBoxBorders = vtbackend::LineCount(2);

    /// How a host frame measures what it writes: as a served session's screen does, whose grapheme
    /// clustering (DEC mode 2027) is on and cannot be turned off from the allowlist -- an emoji with VS16
    /// takes two columns.
    constexpr auto HostFrameWidths = vtbackend::ClusterWidthPolicy::ClusterAware;

    /// @return The cursor moved to @p line and @p column, both counted from zero.
    [[nodiscard]] std::string cursorTo(int line, int column)
    {
        return std::format("\x1b[{};{}H", line + 1, column + 1);
    }

    /// @return The host's frame of @p lines on a page of @p size: a rounded box, centred, each line cut with
    ///         "…" to fit; the lines alone from the top where no box fits.
    [[nodiscard]] std::string hostFrame(vtbackend::PageSize size, std::span<std::string const> lines)
    {
        using vtbackend::ColumnCount;
        using vtbackend::LineCount;

        auto const textLines = LineCount::cast_from(lines.size());
        auto frame = std::string(HostFrameOpening);

        if (size.columns <= HostBoxPadding || size.lines < textLines + HostBoxBorders)
        {
            // No room for a box: the lines alone, from the top.
            auto line = 0;
            for (auto const& text: lines | std::views::take(unbox<std::size_t>(size.lines)))
            {
                frame += cursorTo(line++, 0);
                frame += vtbackend::elideEnd(text, size.columns, HostFrameWidths);
            }
            frame += HostFrameClosing;
            return frame;
        }

        auto widest = ColumnCount(0);
        for (auto const& text: lines)
            widest = std::max(widest, vtbackend::columnsOnTheGrid(text, HostFrameWidths));
        auto const inner = std::min(widest, size.columns - HostBoxPadding);
        auto line = unbox<int>(size.lines - textLines - HostBoxBorders) / 2;
        auto const left = unbox<int>(size.columns - inner - HostBoxPadding) / 2;
        auto const rule = repeatedText("\u2500", unbox<std::size_t>(inner) + 2);

        frame += cursorTo(line++, left) + "\u256D" + rule + "\u256E";
        for (auto const& text: lines)
        {
            auto const shown = vtbackend::elideEnd(text, inner, HostFrameWidths);
            auto const pad = inner - vtbackend::columnsOnTheGrid(shown, HostFrameWidths);
            frame += cursorTo(line++, left) + "\u2502 " + shown + std::string(unbox<std::size_t>(pad), ' ')
                     + " \u2502";
        }
        frame += cursorTo(line, left) + "\u2570" + rule + "\u256F";
        frame += HostFrameClosing;
        return frame;
    }
} // namespace

std::string stoppedFrame(vtbackend::PageSize size, std::string_view title, std::string_view lastFault)
{
    auto const row = std::vformat(proto::RestartExtensionRowFormat, std::make_format_args(title));
    auto const lines = std::array {
        std::vformat(StoppedFrameTitle, std::make_format_args(title)),
        std::string(lastFault),
        std::vformat(StoppedFrameRestart, std::make_format_args(row)),
    };
    return hostFrame(size, lines);
}

std::string restartingFrame(vtbackend::PageSize size, std::string_view title)
{
    auto const lines = std::array { std::vformat(RestartingFrameTitle, std::make_format_args(title)) };
    return hostFrame(size, lines);
}

/// What one device's sinks record. Shared by the sinks, which may outlive the ServedSessions that made them.
struct ServedSessions::DeviceState
{
    /// What the host draws into the tab instead of its extension.
    enum class HostFrame : std::uint8_t
    {
        None = 0,   ///< Nothing: the extension draws it.
        Stopped,    ///< The stopped frame: the extension stopped after repeated faults.
        Restarting, ///< One line saying it restarts, until the restarted extension draws.
    };

    /// Whether the input is dropping what does not fit: one episode is logged once.
    enum class InputOverflow : std::uint8_t
    {
        None = 0, ///< The last chunk fit.
        Dropping, ///< The last chunk was dropped.
    };

    /// @param initial The size the device starts at.
    /// @param changed Tells the extension its tab changed; empty for a device no extension serves.
    DeviceState(vtbackend::PageSize initial, std::function<void()> changed):
        size { initial }, changed { std::move(changed) }
    {
    }

    /// Appends @p chunk to the input, whole, if it fits; drops it whole otherwise. The first chunk into an
    /// empty buffer tells the extension. While the host draws the tab, what is typed into its frame is
    /// dropped: it was meant for no extension, and must not reach the one that restarts.
    /// @param chunk One write of the terminal's.
    void accept(std::string_view chunk)
    {
        if (chunk.size() > MaxServedTransferBytes)
        {
            sessionLog()("a served tab's input of {} bytes can never fit its bound of {} bytes; dropped",
                         chunk.size(),
                         MaxServedTransferBytes);
            return;
        }
        auto wasEmpty = false;
        {
            auto const lock = std::lock_guard { mutex };
            if (frame != HostFrame::None)
                return;
            if (input.size() + chunk.size() > MaxServedTransferBytes)
            {
                if (std::exchange(overflow, InputOverflow::Dropping) == InputOverflow::None)
                    sessionLog()("a served tab's input is full; whole chunks are dropped until it is read");
                return;
            }
            overflow = InputOverflow::None;
            wasEmpty = input.empty();
            input.append(chunk);
        }
        if (wasEmpty && changed)
            changed();
    }

    /// Records the device's new size, draws the host's frame into @p device again at that size while the host
    /// draws the tab, and tells the extension.
    void resized(vtbackend::PageSize cells, vtpty::ChannelPty& device)
    {
        {
            auto const lock = std::lock_guard { mutex };
            size = cells;
            drawFrameInto(device);
        }
        if (changed)
            changed();
    }

    /// Has the host draw the tab: @p shown, with @p lastFault; what was typed and not read yet is dropped.
    /// Call holding `mutex`.
    void showFrame(vtpty::ChannelPty& device, HostFrame shown, std::string_view lastFault)
    {
        frame = shown;
        fault = std::string(lastFault);
        input.clear();
        overflow = InputOverflow::None;
        drawFrameInto(device);
    }

    /// Feeds @p device the host's frame at the current size, if the host draws the tab. Call holding `mutex`:
    /// a frame composed for one size is fed before a resize can compose one for the next.
    void drawFrameInto(vtpty::ChannelPty& device) const
    {
        switch (frame)
        {
            case HostFrame::None: break;
            case HostFrame::Stopped: device.feed(stoppedFrame(size, title, fault)); break;
            case HostFrame::Restarting: device.feed(restartingFrame(size, title)); break;
        }
    }

    mutable std::mutex mutex; ///< Guards everything below but `changed`.
    std::string input;
    vtbackend::PageSize size;
    InputOverflow overflow = InputOverflow::None;
    HostFrame frame = HostFrame::None; ///< What the host draws into the tab, if anything.
    std::string title;                 ///< The extension's title, while the host draws the tab.
    std::string fault;                 ///< Its last fault, while the host draws the stopped frame.
    std::function<void()> changed;     ///< Fixed at construction; called holding no lock.
};

ServedSessions::ServedSessions(std::shared_ptr<vtcontrol::UuidSource> ids,
                               IExtensionActions const& origins,
                               IConnectionWorkspaces const& workspaces,
                               IServedAdmission const& admission,
                               Raise raise,
                               OwnerPost post,
                               ServedTabHost& host,
                               core::platform::IClock const& clock):
    _ids { std::move(ids) },
    _origins { origins },
    _workspaces { workspaces },
    _admission { admission },
    _raise { std::move(raise) },
    _post { std::move(post) },
    _host { host },
    _clock { clock }
{
}

ServedSessions::Tracked ServedSessions::track(vtbackend::PageSize size, std::function<void()> changed)
{
    auto device = std::make_shared<vtpty::ChannelPty>(size);
    auto state = std::make_shared<DeviceState>(size, std::move(changed));
    device->setWriteSink(
        [state](std::string_view chunk, vtpty::InputKind /*kind*/) { state->accept(chunk); });
    // The device calls its own sink: it is alive for the call, and holding it here would keep it alive
    // forever.
    device->setResizeSink(
        [state, pty = device.get()](vtbackend::PageSize cells, std::optional<vtpty::ImageSize> /*pixels*/) {
            state->resized(cells, *pty);
        });
    // The entries of devices nobody holds any more go as a new one comes.
    std::erase_if(_devices, [](Entry const& entry) { return entry.device.expired(); });
    _devices.push_back(Entry { .device = device, .state = state });
    return Tracked { .device = std::move(device), .state = std::move(state) };
}

std::shared_ptr<vtpty::ChannelPty> ServedSessions::makeDevice(vtbackend::PageSize size)
{
    auto const lock = std::lock_guard { _mutex };
    return track(size, {}).device;
}

std::string ServedSessions::inputOf(vtpty::ChannelPty const& device) const
{
    auto state = std::shared_ptr<DeviceState> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find_if(
            _devices, [&](Entry const& entry) { return entry.device.lock().get() == &device; });
        if (found == _devices.end())
            return {};
        state = found->state;
    }
    auto const lock = std::lock_guard { state->mutex };
    return state->input;
}

void ServedSessions::publishPageSize(vtbackend::PageSize size)
{
    auto const lock = std::lock_guard { _mutex };
    _pageSize = size;
}

ServedSessions::Tab* ServedSessions::ownedBy(std::string_view extension, std::string_view id)
{
    auto const found = std::ranges::find_if(_tabs, [&](Tab const& tab) {
        return tab.id == id && tab.extension == extension && tab.phase != TabPhase::Cancelled;
    });
    return found != _tabs.end() ? &*found : nullptr;
}

std::optional<vtworkspace::WorkspaceId> ServedSessions::workspaceOfTab(std::string_view extension,
                                                                       std::string_view tab) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const found = std::ranges::find_if(_tabs, [&](Tab const& candidate) {
        return candidate.id == tab && candidate.extension == extension
               && candidate.phase != TabPhase::Cancelled;
    });
    return found != _tabs.end() ? std::optional { found->workspace } : std::nullopt;
}

std::optional<ConnectionId> ServedSessions::recentInputFrom(std::string_view extension) const
{
    auto const lock = std::lock_guard { _mutex };
    auto const typed = _userInput.find(extension);
    if (typed == _userInput.end() || _clock.now() - typed->second.second > ServedInputGrace)
        return std::nullopt;
    return typed->second.first;
}

void ServedSessions::noteUserInput(vtworkspace::SessionId session, ConnectionId const& from)
{
    auto const lock = std::lock_guard { _mutex };
    auto const tab = std::ranges::find(_tabs, std::optional { session }, &Tab::session);
    if (tab == _tabs.end())
        return;
    _userInput.insert_or_assign(tab->extension, std::pair { from, _clock.now() });
}

void ServedSessions::forget(std::string_view id)
{
    std::erase_if(_tabs, [&](Tab const& tab) { return tab.id == id; });
}

std::expected<std::string, ServedTabError> ServedSessions::open(std::string_view extension,
                                                                std::string_view title,
                                                                std::string_view moduleDigest,
                                                                std::string_view scheme,
                                                                Origin origin)
{
    auto const connection = _origins.connectionOf(extension, origin);
    if (!connection)
        return std::unexpected(ServedTabError::Gone);
    auto id = std::string {};
    auto reveal = std::optional<std::pair<vtworkspace::SessionId, vtworkspace::WorkspaceId>> {};
    auto created = false;
    {
        auto const lock = std::lock_guard { _mutex };
        if (_admission.current() == ServedAdmission::Closed)
            return std::unexpected(ServedTabError::Limit);
        auto const workspace = _workspaces.selectedBy(*connection);
        if (!workspace)
            return std::unexpected(ServedTabError::Gone);
        // One dashboard per extension and workspace: a second open, pending or not, is the first one.
        auto const existing = std::ranges::find_if(_tabs, [&](Tab const& tab) {
            return tab.extension == extension && tab.workspace == *workspace
                   && tab.phase != TabPhase::Cancelled;
        });
        if (existing != _tabs.end())
        {
            // A pending one is activated by its creation.
            if (existing->session)
                reveal.emplace(*existing->session, *workspace);
            id = existing->id;
        }
        else
        {
            id = vtcontrol::toString(_ids->next());
            auto tracked = track(_pageSize, [raise = _raise, name = std::string(extension)] {
                raise(name, ServedHint::Tabs);
            });
            _tabs.push_back(Tab { .id = id,
                                  .extension = std::string(extension),
                                  .scheme = std::string(scheme),
                                  .workspace = *workspace,
                                  .phase = TabPhase::Pending,
                                  .device = std::move(tracked.device),
                                  .state = std::move(tracked.state),
                                  .session = std::nullopt });
            created = true;
        }
    }
    if (created)
        _post([this, id, title = std::string(title), moduleDigest = std::string(moduleDigest)] {
            create(id, title, moduleDigest);
        });
    else if (reveal)
        _post([this, session = reveal->first, connection = *connection, workspace = reveal->second] {
            revealSession(_host, session, connection, workspace);
        });
    return id;
}

void ServedSessions::create(std::string const& id, std::string const& title, std::string const& moduleDigest)
{
    auto spawn = ServedSpawn {};
    auto workspace = vtworkspace::WorkspaceId {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_tabs, id, &Tab::id);
        if (found == _tabs.end() || found->phase == TabPhase::Open)
            return;
        if (found->phase == TabPhase::Cancelled)
        {
            // Closed before it was created: nothing is.
            found->device->close();
            forget(id);
            return;
        }
        spawn = ServedSpawn { .extension = found->extension,
                              .scheme = found->scheme,
                              .moduleDigest = moduleDigest,
                              .device = found->device,
                              .title = title };
        workspace = found->workspace;
    }
    auto const pane = vtcontrol::parseUuid(id);
    auto const extension = spawn.extension;
    auto const device = spawn.device;
    auto const session =
        pane ? _host.createServedTab(
                   workspace,
                   SessionSpawnRequest { .presetIds = PresetObjectIds { .pane = *pane, .tabName = title },
                                         .served = std::move(spawn) })
             : std::nullopt;
    auto cancelled = false;
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_tabs, id, &Tab::id);
        cancelled = found == _tabs.end() || found->phase == TabPhase::Cancelled;
        if (session && !cancelled)
        {
            found->phase = TabPhase::Open;
            found->session = session;
        }
        else
            forget(id);
    }
    if (!session)
    {
        // Refused: its workspace went, or a live upgrade runs. The extension finds it gone from its tabs.
        sessionLog()("the served tab {} of {} could not be created", id, extension);
        device->close();
        _raise(extension, ServedHint::Tabs);
        return;
    }
    if (cancelled)
    {
        // Closed while it was being created.
        _host.closeServed(*session, SessionEnd::Requested);
        return;
    }
    _raise(extension, ServedHint::Tabs);
}

void ServedSessions::sessionMoved(vtworkspace::SessionId session, vtworkspace::WorkspaceId workspace)
{
    auto const lock = std::lock_guard { _mutex };
    if (auto const found = std::ranges::find(_tabs, std::optional { session }, &Tab::session);
        found != _tabs.end())
        found->workspace = workspace;
}

void ServedSessions::sessionEnded(vtworkspace::SessionId session)
{
    auto extension = std::string {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const found = std::ranges::find(_tabs, std::optional { session }, &Tab::session);
        if (found == _tabs.end())
            return;
        extension = found->extension;
        _tabs.erase(found);
    }
    _raise(extension, ServedHint::Tabs);
}

std::expected<void, ServedTabError> ServedSessions::close(std::string_view extension, std::string_view tab)
{
    auto session = std::optional<vtworkspace::SessionId> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        if (!found->session)
        {
            found->phase = TabPhase::Cancelled;
            return {};
        }
        session = found->session;
        forget(tab);
    }
    _post([this, session = *session] { _host.closeServed(session, SessionEnd::Requested); });
    return {};
}

std::expected<void, ServedTabError> ServedSessions::reveal(std::string_view extension,
                                                           std::string_view tab,
                                                           Origin origin)
{
    // The origin's window; else that of the key or click that asked for it, into one of its tabs just now.
    auto const connection =
        _origins.connectionOf(extension, origin).or_else([&] { return recentInputFrom(extension); });
    auto session = vtworkspace::SessionId {};
    auto workspace = vtworkspace::WorkspaceId {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        // A pending tab is activated by its creation.
        if (!found->session)
            return {};
        auto const selected = connection ? _workspaces.selectedBy(*connection) : std::nullopt;
        if (!selected)
            return std::unexpected(ServedTabError::Gone);
        session = *found->session;
        workspace = *selected;
    }
    _post([this, session, connection = *connection, workspace] {
        revealSession(_host, session, connection, workspace);
    });
    return {};
}

std::vector<ServedTabInfo> ServedSessions::list(std::string_view extension) const
{
    auto const lock = std::lock_guard { _mutex };
    auto listed = std::vector<ServedTabInfo> {};
    for (auto const& tab: _tabs)
    {
        if (tab.extension != extension || tab.phase == TabPhase::Cancelled)
            continue;
        auto const size = [&] {
            auto const stateLock = std::lock_guard { tab.state->mutex };
            return tab.state->size;
        }();
        listed.push_back(ServedTabInfo { .tab = tab.id,
                                         .scheme = tab.scheme,
                                         .cols = unbox<uint32_t>(size.columns),
                                         .rows = unbox<uint32_t>(size.lines),
                                         .state = tab.phase == TabPhase::Open ? ServedTabState::Open
                                                                              : ServedTabState::Pending });
    }
    return listed;
}

std::expected<std::string, ServedTabError> ServedSessions::read(std::string_view extension,
                                                                std::string_view tab)
{
    auto state = std::shared_ptr<DeviceState> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        // A pending tab has no terminal to have written anything yet.
        if (!found->session)
            return std::string {};
        state = found->state;
    }
    auto const lock = std::lock_guard { state->mutex };
    state->overflow = DeviceState::InputOverflow::None;
    return std::exchange(state->input, {});
}

std::expected<void, ServedTabError> ServedSessions::write(std::string_view extension,
                                                          std::string_view tab,
                                                          std::string_view bytes)
{
    auto device = std::shared_ptr<vtpty::ChannelPty> {};
    auto state = std::shared_ptr<DeviceState> {};
    {
        auto const lock = std::lock_guard { _mutex };
        auto const* const found = ownedBy(extension, tab);
        if (found == nullptr)
            return std::unexpected(ServedTabError::NotOwned);
        device = found->device;
        state = found->state;
    }
    if (bytes.size() > MaxServedTransferBytes)
        return std::unexpected(ServedTabError::Limit);
    if (device->isClosed())
        return std::unexpected(ServedTabError::Gone);
    // The extension draws: whatever frame the host drew is its to cover, and no resize draws it again. Fed
    // holding the lock, so that a resize's frame never lands after what the extension drew.
    auto const lock = std::lock_guard { state->mutex };
    state->frame = DeviceState::HostFrame::None;
    device->feed(bytes);
    return {};
}

void ServedSessions::closeAll(std::string_view extension)
{
    auto sessions = std::vector<vtworkspace::SessionId> {};
    {
        auto const lock = std::lock_guard { _mutex };
        for (auto& tab: _tabs)
        {
            if (tab.extension != extension)
                continue;
            if (tab.session)
                sessions.push_back(*tab.session);
            else
                tab.phase = TabPhase::Cancelled;
        }
        std::erase_if(_tabs, [&](Tab const& tab) { return tab.extension == extension && tab.session; });
    }
    if (!sessions.empty())
        _post([this, sessions = std::move(sessions)] {
            for (auto const session: sessions)
                _host.closeServed(session, SessionEnd::Requested);
        });
}

void ServedSessions::drawStopped(std::string_view extension,
                                 std::string_view title,
                                 std::string_view lastFault)
{
    auto const lock = std::lock_guard { _mutex };
    for (auto const& tab: _tabs)
    {
        if (tab.extension != extension || tab.phase == TabPhase::Cancelled)
            continue;
        auto const stateLock = std::lock_guard { tab.state->mutex };
        tab.state->title = std::string(title);
        tab.state->showFrame(*tab.device, DeviceState::HostFrame::Stopped, lastFault);
    }
}

void ServedSessions::resumeDrawing(std::string_view extension)
{
    auto const lock = std::lock_guard { _mutex };
    for (auto const& tab: _tabs)
    {
        if (tab.extension != extension)
            continue;
        auto const stateLock = std::lock_guard { tab.state->mutex };
        // Only a tab the host drew stopped says that it restarts: one its extension draws stays as it is.
        if (tab.state->frame == DeviceState::HostFrame::Stopped)
            tab.state->showFrame(*tab.device, DeviceState::HostFrame::Restarting, {});
    }
}

} // namespace vthost
