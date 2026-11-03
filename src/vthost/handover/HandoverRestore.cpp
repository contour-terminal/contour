// SPDX-License-Identifier: Apache-2.0
#include <vthost/handover/HandoverRestore.hpp>

#include <vtbackend/state/TerminalState.hpp>
#include <vtbackend/state/TerminalStateCodec.hpp>

#include <vtpty/Process.hpp>

#ifndef _WIN32
    #include <vtpty/UnixPty.hpp>

    #include <sys/resource.h>
    #include <sys/socket.h>
    #include <sys/syscall.h>

    #include <unistd.h>
#endif

#include <core/Assert.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <ranges>
#include <utility>

#include <vthost/ListeningSocket.hpp>
#include <vthost/Logging.hpp>
#include <vthost/SessionHost.hpp>
#include <vthost/SessionSettings.hpp>

namespace vthost
{

using handover::HandoverError;
using handover::HandoverImage;
using handover::HandoverSession;
using vtworkspace::SessionId;

namespace
{
    /// @return The session ids @p tree's leaves carry, in tree order.
    [[nodiscard]] std::vector<SessionId> leafSessionsOf(vtworkspace::WorkspaceTreeState const& tree)
    {
        auto sessions = std::vector<SessionId> {};
        auto const visit = [&](this auto const& self, vtworkspace::PaneTreeState const& node) -> void {
            if (node.children.empty())
                sessions.push_back(node.session);
            for (auto const& child: node.children)
                self(child);
        };
        for (auto const& window: tree.windows)
            for (auto const& tab: window.tabs)
                visit(tab.root);
        return sessions;
    }
} // namespace

std::expected<void, HandoverError> SessionHost::restoreHandover(HandoverImage image)
{
    // The registry first: it names every object the model brings back, and every session adopted below.
    _objects.restore(image.objects);

    auto outcome = std::expected<void, HandoverError> {};
    if (auto const restored = _model.restore(image.tree, image.modelIds); !restored)
    {
        // The programs matter more than where they were shown: each still gets a tab
        // (adoptHandoverSession()).
        errorLog()("handover: the workspace tree was refused (error {}); every session gets a tab of its own",
                   std::to_underlying(restored.error()));
        outcome = std::unexpected(HandoverError::Malformed);
    }

    // No session minted from now on is named as one the previous image had: not one the tree names, not one
    // it hosted, not one it minted.
    _nextSessionId = std::ranges::fold_left(
        image.sessions,
        std::max(image.nextSessionId.value, vtworkspace::firstUnusedSessionId(image.tree).value),
        [](uint64_t next, HandoverSession const& session) { return std::max(next, session.id.value + 1); });

    if (_model.windowCount() > 0)
        _window = _model.windowAt(0)->id();
    else
    {
        _window = _model.createWindow()->id();
        if (!_objects.uuidOf(_window))
            _objects.bind(_window, _objects.mint());
    }

    for (auto& session: image.sessions)
        adoptHandoverSession(session);

    // A leaf whose session could not be adopted shows nothing, and nothing would ever close it.
    for (auto const session: leafSessionsOf(image.tree))
        if (!_sessions.contains(session.value))
            if (auto const [window, tab, leaf] = _model.findSessionLeaf(session); leaf != nullptr)
                _model.closePane(window->id(), tab->id(), leaf->id());

    daemonLog()("took over {} of {} session(s) from release {}",
                _sessions.size(),
                image.sessions.size(),
                image.fromRelease.empty() ? std::string_view { "(unknown)" } : image.fromRelease);
    return outcome;
}

void SessionHost::adoptHandoverSession(HandoverSession& session)
{
#ifdef _WIN32
    // No image is ever captured here (HandoverError::NotSupported): there is nothing a session could be.
    errorLog()("handover: session {} cannot be adopted on this platform", session.id.value);
#else
    auto pty = vtpty::UnixPty::adopt(session.pty);
    if (!pty)
    {
        errorLog()(
            "handover: session {}: its PTY could not be adopted (error {}); its program cannot be reached",
            session.id.value,
            std::to_underlying(pty.error()));
        return;
    }

    // An id another session of the image took already, or none at all, is a broken image; the program still
    // gets a session, under a fresh id and in a tab of its own.
    auto id = session.id;
    if (id == SessionId {} || _sessions.contains(id.value))
    {
        id = SessionId { _nextSessionId++ };
        errorLog()("handover: session {} is named twice in the image; adopted as session {}",
                   session.id.value,
                   id.value);
    }

    // A child that is gone already leaves its PTY to whatever still holds the other end: served until it
    // closes, as the pump ends a session at its PTY's end.
    auto device = [&]() -> std::unique_ptr<vtpty::Pty> {
        if (!vtpty::Process::canAdopt(session.child))
        {
            sessionLog()("handover: session {}: process {} is no child of this one any more; serving its PTY "
                         "until it closes",
                         id.value,
                         session.child.pid);
            return std::move(*pty);
        }
        auto process = vtpty::Process::adopt(session.child, std::move(*pty));
        if (!process)
        {
            errorLog()("handover: session {}: process {} could not be adopted", id.value, session.child.pid);
            return nullptr;
        }
        return std::move(*process);
    }();
    if (!device)
        return;

    // Built on settings this image's configuration makes up afresh, the terminal's construction settings
    // taken from the captured state, so that the state restores into a terminal of its own shape. A state
    // that cannot be read restores nothing: the terminal is fresh, at the PTY's size.
    auto base = handoverBaseSettings(session);
    auto const state = vtbackend::decodeTerminalState(session.terminalState);
    auto settings = state.and_then(
        [&](vtbackend::TerminalState const& captured) { return vtbackend::settingsFor(captured, base); });
    if (!settings)
        base.pageSize = session.pty.pageSize;
    auto hosted = makeHostedSession(id,
                                    std::move(device),
                                    settings ? *std::move(settings) : std::move(base),
                                    session.profileName,
                                    session.origin);

    // All or nothing (Terminal::restoreState()): one that is refused leaves the terminal exactly as it was
    // constructed -- the fresh terminal the program runs on from here, on the very same PTY.
    auto const restored = state.and_then([&](vtbackend::TerminalState const& captured) {
        auto& terminal = hosted->terminal();
        auto const _ = std::lock_guard { terminal };
        return terminal.restoreState(captured);
    });
    if (!restored)
    {
        errorLog()(
            "handover: session {}: its screen could not be restored (error {}); the program runs on with "
            "a fresh one",
            id.value,
            std::to_underlying(restored.error()));
        _unrestoredScreens.push_back(id);
    }

    // Not part of a terminal's state, and without it an image a program draws before a client attaches has no
    // cells to be laid out on: the PTY's size in pixels, which a client last reported, gives it back.
    if (session.pty.pixels)
    {
        auto& terminal = hosted->terminal();
        auto const _ = std::lock_guard { terminal };
        terminal.setCellPixelSize(*session.pty.pixels / terminal.totalPageSize());
    }

    // Delivered after the replies the restored terminal queued, before anything a client hands it from now.
    hosted->replayCarried(std::move(session.carried));
    installSession(std::move(hosted));

    if (!_objects.uuidOf(id))
    {
        auto const known = session.uuid != vtcontrol::Uuid {}
                           && _objects.find(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane,
                                                                   .uuid = session.uuid });
        auto const uuid = session.uuid == vtcontrol::Uuid {} || known ? _objects.mint() : session.uuid;
        _objects.bind(id, uuid);
        _objects.setLabel(vtcontrol::ObjectRef { .kind = vtcontrol::ObjectKind::Pane, .uuid = uuid },
                          session.label);
    }

    // A session the tree does not lay out still is a program: it gets a tab of its own.
    if (auto const [window, tab, leaf] = _model.findSessionLeaf(id); leaf == nullptr)
    {
        _pendingSessionId = id;
        if (_model.createTab(_window) == nullptr)
            errorLog()("handover: session {} could not be given a tab", id.value);
        _pendingSessionId.reset();
    }
#endif
}

vtbackend::Settings SessionHost::handoverBaseSettings(HandoverSession const& session) const
{
    // Only a subset of the settings crosses the exec (toWireSessionSettings): the profile, resolved in the
    // configuration this image loaded, makes up the rest -- unless a client stated them, whose own they stay.
    if (session.origin == SettingsOrigin::Client)
        return hostedSessionSettings(session.settings);
    if (!_profiles)
        return _settings;
    return hostedSessionSettings(_profiles->resolve(session.profileName).settings);
}

} // namespace vthost

namespace vthost::handover
{

std::vector<int> descriptorsToKeep(HandoverImage const& image)
{
    auto kept = std::vector<int> { 0, 1, 2 };
    auto const keep = [&](int fd) {
        if (fd >= 0)
            kept.push_back(fd);
    };
    for (auto const& session: image.sessions)
    {
        keep(session.pty.masterFd);
        if (session.pty.stdoutFastPipe)
        {
            keep(session.pty.stdoutFastPipe->first);
            keep(session.pty.stdoutFastPipe->second);
        }
    }
    for (auto const& listener: image.listeners)
        keep(listener.fd);
    keep(image.leaseFd);
    std::ranges::sort(kept);
    auto const repeated = std::ranges::unique(kept);
    kept.erase(repeated.begin(), repeated.end());
    return kept;
}

#ifndef _WIN32
namespace
{
    /// Closes descriptors @p first to @p last, both included; nothing when @p first is past @p last.
    void closeEach(int first, int last) noexcept
    {
        if (first > last)
            return;
        for (auto const fd: std::views::iota(first, last + 1))
            ::close(fd);
    }

    /// @return The highest descriptor this process can hold now (RLIMIT_NOFILE), bounded: a limit raised to
    ///         millions (or unlimited) must not make a loop over them the slowest part of a start.
    [[nodiscard]] int highestDescriptor() noexcept
    {
        constexpr auto MaxSweptDescriptor = 1 << 20;
        auto limit = rlimit {};
        if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == RLIM_INFINITY
            || limit.rlim_cur > static_cast<rlim_t>(MaxSweptDescriptor))
            return MaxSweptDescriptor;
        return static_cast<int>(limit.rlim_cur) - 1;
    }

    /// Closes every descriptor from @p first on.
    void closeFrom(int first) noexcept
    {
    #if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
        ::closefrom(first);
    #else
        #if defined(__linux__) && defined(SYS_close_range)
        // The syscall rather than glibc's wrapper, which older C libraries lack; a kernel that predates it
        // (5.9) answers ENOSYS, and the loop below does it.
        if (::syscall(SYS_close_range, static_cast<unsigned>(first), std::numeric_limits<unsigned>::max(), 0U)
            == 0)
            return;
        #endif
        // No descriptor lies above the limit it was opened under.
        closeEach(first, highestDescriptor());
    #endif
    }
} // namespace
#endif

std::expected<HandoverImage, HandoverError> readHandover(HandoverMemory& memory, int fd)
{
    auto image =
        memory.read(fd).and_then([](std::vector<uint8_t> const& bytes) { return decodeHandover(bytes); });
#ifndef _WIN32
    ::close(fd);
#endif
    return image;
}

std::size_t closeListeningSockets()
{
#ifdef _WIN32
    return 0;
#else
    auto closed = std::size_t { 0 };
    // Above the standard streams, up to the descriptor limit: the old image's listeners are among the first
    // descriptors it opened, but nothing says how far up the rest lie.
    for (auto const fd: std::views::iota(3, highestDescriptor() + 1))
    {
        if (!isListeningSocket(fd))
            continue;
        ::close(fd);
        ++closed;
    }
    return closed;
#endif
}

void closeDescriptorsExcept(std::span<int const> keep)
{
    Require(std::ranges::is_sorted(keep) && std::ranges::adjacent_find(keep) == keep.end()
            && (keep.empty() || keep.front() >= 0));
#ifndef _WIN32
    // The gaps between kept descriptors are a few numbers each; everything above the last goes at once.
    auto next = 0;
    for (auto const fd: keep)
    {
        closeEach(next, fd - 1);
        next = fd + 1;
    }
    closeFrom(next);
#endif
}

} // namespace vthost::handover
