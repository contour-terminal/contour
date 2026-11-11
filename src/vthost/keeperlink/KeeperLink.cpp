// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/KeeperLink.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

#include <algorithm>
#include <deque>
#include <mutex>
#include <ranges>
#include <set>
#include <span>
#include <utility>
#include <variant>

#include <fcntl.h>

#include <vthost/Logging.hpp>
#include <vthost/keeper/Kqueue.hpp>

#ifdef CONTOUR_KEEPER_KQUEUE
    #include <unistd.h>
#endif

namespace vthost::keeperlink
{

namespace
{
    /// The most frames one gathered send takes.
    constexpr auto MaxBatch = std::size_t { 32 };

    /// One frame on its way to the keeper.
    struct Outgoing
    {
        std::vector<uint8_t> bytes;
        std::vector<keeper::UniqueFd> fds; ///< Duplicates, the queue's own; closed once the frame is sent.
        std::size_t sent = 0;              ///< How many of the bytes went.
    };

    /// Whether the link still reaches the keeper.
    enum class Link : uint8_t
    {
        Open = 0, ///< Reading and writing.
        Lost,     ///< The keeper's end is gone, or the channel broke; reported once.
        Closing,  ///< The link is sealed or being destroyed: nothing is sent or reported any more.
    };

    /// Whether frames still go out.
    enum class WriteSide : uint8_t
    {
        Open = 0,
        Closed, ///< A send found the keeper gone: what it sent is still read, until the end.
    };

    /// Whether a write waits for the socket to drain.
    enum class Writing : uint8_t
    {
        Ready = 0,
        AwaitingWritable, ///< A flow waits for the socket to take more.
    };

    /// Whether a frame was queued.
    enum class QueueOutcome : uint8_t
    {
        Dropped = 0, ///< Its descriptors could not be duplicated; it was dropped.
        Queued,
    };

    /// Where a flush stopped.
    enum class Flushed : uint8_t
    {
        All = 0, ///< Nothing is left queued, or nothing more can go.
        Blocked, ///< The socket takes no more now.
    };

    /// Whether a spawn() or announceShutdown() waits for its answer, so that what arrives meanwhile waits
    /// too.
    enum class Dispatch : uint8_t
    {
        Free = 0,
        Held,
    };

    /// Whether the daemon told the keeper it shuts down (announceShutdown()).
    enum class Farewell : uint8_t
    {
        Unannounced = 0, ///< Any end now is a crash to the keeper.
        Announced,       ///< What is queued as the link goes is the clean end's, and goes.
    };
} // namespace

struct KeeperLink::State: std::enable_shared_from_this<State>
{
    State(core::net::EventLoop& loopRef,
          KeeperChannel channelValue,
          uint32_t version,
          std::function<void()> lostHandler,
          std::chrono::milliseconds replyBound,
          std::chrono::milliseconds beatInterval):
        loop { loopRef },
        channel { std::move(channelValue) },
        channelVersion { version },
        onLost { std::move(lostHandler) },
        spawnReplyBound { replyBound },
        heartbeatInterval { beatInterval }
    {
    }

    core::net::EventLoop& loop;
    KeeperChannel channel;
    uint32_t channelVersion; ///< As the handshake negotiated it.
    std::function<void()> onLost;
    std::chrono::milliseconds spawnReplyBound;
    std::chrono::milliseconds heartbeatInterval; ///< How often a wait for an answer beats.
    /// The spawns given up on whose answer has not come: a late Spawned for one is released.
    std::set<uint64_t> abandoned;
    ChildEvents children; ///< @see KeeperLink::attachChildren

    std::mutex queueLock;
    std::deque<Outgoing> queue; ///< Guarded by queueLock; sent from the loop thread only.

    Link link = Link::Open;
    WriteSide writeSide = WriteSide::Open;
    Writing writing = Writing::Ready;
    Dispatch dispatch = Dispatch::Free;
    Farewell farewell = Farewell::Unannounced;
    std::deque<ReceivedMessage> inbox;
    uint64_t lastRequest = 0;
#ifdef CONTOUR_KEEPER_KQUEUE
    keeper::UniqueFd parentWatch; ///< A kqueue that reports the keeper's exit (EVFILT_PROC, NOTE_EXIT).
#endif

    // {{{ writing

    /// Queues @p message with duplicates of @p fds.
    /// @return Whether it was queued: a descriptor that cannot be duplicated (the process is out of them)
    ///         drops that one frame, logged, rather than the link, which a frame without its descriptor would
    ///         break.
    [[nodiscard]] QueueOutcome enqueue(keeper::Message const& message, std::span<int const> fds)
    {
        auto outgoing = Outgoing { .bytes = frameBytesOf(message, fds.size()), .fds = {}, .sent = 0 };
        for (auto const fd: fds)
        {
            auto copy = keeper::UniqueFd { ::fcntl(fd, F_DUPFD_CLOEXEC, 0) };
            if (!copy.isOpen())
            {
                errorLog()("cannot duplicate a descriptor for the keeper (errno {}); message {} dropped",
                           errno,
                           message.index() + 1);
                return QueueOutcome::Dropped;
            }
            outgoing.fds.push_back(std::move(copy));
        }
        auto const guard = std::scoped_lock { queueLock };
        queue.push_back(std::move(outgoing));
        return QueueOutcome::Queued;
    }

    /// Sends what the socket takes now, in order, and has the loop send the rest once it can. Loop thread.
    void flush()
    {
        if (flushNow() == Flushed::Blocked)
            awaitWritable();
    }

    /// Sends what the socket takes now, in order. Loop thread.
    /// @return Whether the socket took everything that can go.
    [[nodiscard]] Flushed flushNow()
    {
        auto const guard = std::scoped_lock { queueLock };
        while (!queue.empty())
        {
            if (writeSide == WriteSide::Closed || link != Link::Open)
            {
                queue.clear();
                return Flushed::All;
            }
            // A frame with descriptors goes in a send of its own, its descriptors on its first byte; the
            // frames up to the next such frame are gathered into one.
            auto runs = std::vector<std::span<uint8_t const>> {};
            auto fds = std::vector<int> {};
            auto const& front = queue.front();
            if (!front.fds.empty() && front.sent == 0)
            {
                runs.emplace_back(front.bytes);
                fds = std::ranges::to<std::vector>(front.fds | std::views::transform(&keeper::UniqueFd::get));
            }
            else
            {
                runs.push_back(std::span<uint8_t const> { front.bytes }.subspan(front.sent));
                for (auto const& next: queue | std::views::drop(1) | std::views::take(MaxBatch - 1))
                {
                    if (!next.fds.empty())
                        break;
                    runs.emplace_back(next.bytes);
                }
            }
            auto const sent = channel.sendSome(runs, fds);
            if (!sent)
            {
                // A keeper that is gone is reported by the read side, once what it wrote is read.
                if (sent.error() == ChannelError::Closed)
                    writeSide = WriteSide::Closed;
                else
                    lose();
                queue.clear();
                return Flushed::All;
            }
            if (*sent == 0)
                return Flushed::Blocked;
            consume(*sent);
        }
        return Flushed::All;
    }

    /// Drops @p count sent bytes off the queue's front. queueLock held.
    void consume(std::size_t count)
    {
        while (count > 0 && !queue.empty())
        {
            auto& front = queue.front();
            auto const rest = front.bytes.size() - front.sent;
            if (count < rest)
            {
                front.sent += count;
                front.fds.clear(); // they went with the first byte
                return;
            }
            count -= rest;
            queue.pop_front();
        }
    }

    void awaitWritable()
    {
        if (writing == Writing::AwaitingWritable)
            return;
        writing = Writing::AwaitingWritable;
        loop.spawn(writeWhenWritable(shared_from_this()));
    }

    static core::async::Task<void> writeWhenWritable(std::shared_ptr<State> self)
    {
        try
        {
            co_await self->loop.waitWritable(self->channel.handle());
        }
        catch (core::async::OperationCancelled const&)
        {
            self->writing = Writing::Ready;
            co_return;
        }
        catch (core::net::FdRegistrationFailed const&)
        {
            self->writing = Writing::Ready;
            self->lose();
            co_return;
        }
        self->writing = Writing::Ready;
        self->flush();
    }

    /// Sends everything queued, waiting here while the socket is full, until @p deadline. Loop thread.
    /// @return Nothing once all of it went, or nothing more can; ChannelError::TimedOut at the deadline.
    [[nodiscard]] std::expected<void, ChannelError> flushUntil(core::platform::SteadyTimePoint deadline)
    {
        while (flushNow() == Flushed::Blocked)
            if (auto const writable = channel.awaitWritable(loop.clock(), deadline); !writable)
                return writable;
        return {};
    }

    /// Drops what is queued, unsent: the link ends without the daemon's announcement.
    void dropQueued()
    {
        auto const guard = std::scoped_lock { queueLock };
        if (queue.empty())
            return;
        errorLog()(
            "the daemon ends without announcing it to the keeper; {} queued message(s) dropped, so that "
            "the keeper keeps every session for the daemon it restarts",
            queue.size());
        queue.clear();
    }

    /// Queues and, on the loop thread, sends at once; elsewhere the loop sends it next.
    void post(keeper::Message const& message, std::span<int const> fds)
    {
        if (enqueue(message, fds) == QueueOutcome::Dropped)
            return;
        if (loop.isOnWorkerThread())
        {
            flush();
            return;
        }
        loop.post([weak = weak_from_this()] {
            if (auto const self = weak.lock())
                self->flush();
        });
    }

    // }}}
    // {{{ reading

    /// Reports the loss once, from a turn of its own.
    void lose()
    {
        if (link != Link::Open)
            return;
        link = Link::Lost;
        // No answer comes any more: nothing is left to release.
        abandoned.clear();
        loop.post([weak = weak_from_this()] {
            auto const self = weak.lock();
            if (!self || self->link != Link::Lost)
                return;
            if (self->children.orphaned)
                self->children.orphaned();
            if (self->onLost)
                self->onLost();
        });
    }

    static core::async::Task<void> readLoop(std::shared_ptr<State> self)
    {
        try
        {
            while (self->link == Link::Open)
            {
                // What the handshake left on the channel is dispatched before anything is waited for.
                auto read = self->channel.readAvailable();
                if (!read)
                {
                    if (self->link == Link::Open)
                        errorLog()("the keeper's channel ended: {}",
                                   read.error() == ChannelError::Closed ? "the keeper is gone" : "it broke");
                    self->lose();
                    co_return;
                }
                for (auto& message: *read)
                    self->inbox.push_back(std::move(message));
                self->dispatchInbox();
                if (self->link != Link::Open)
                    co_return;
                co_await self->loop.waitReadable(self->channel.handle());
            }
        }
        catch (core::async::OperationCancelled const&)
        {
            co_return;
        }
        catch (core::net::FdRegistrationFailed const&)
        {
            self->lose();
        }
    }

#ifdef CONTOUR_KEEPER_KQUEUE
    /// The backstop on macOS and the BSDs, which have no PR_SET_PDEATHSIG: the keeper's exit, by kqueue
    /// NOTE_EXIT.
    static core::async::Task<void> watchKeeper(std::shared_ptr<State> self)
    {
        try
        {
            // A link destroyed before the loop first turned woke no one: this would park on the kqueue for
            // good, and keep the channel open with the state it holds.
            if (self->link != Link::Open)
                co_return;
            co_await self->loop.waitReadable(self->parentWatch.get());
            self->lose();
        }
        catch (core::async::OperationCancelled const&)
        {
            co_return;
        }
        catch (core::net::FdRegistrationFailed const&)
        {
            co_return;
        }
    }

    void armKeeperWatch()
    {
        parentWatch = keeper::UniqueFd { ::kqueue() };
        if (!parentWatch.isOpen())
            return;
        struct kevent change {};
        EV_SET(&change, ::getppid(), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
        if (::kevent(parentWatch.get(), &change, 1, nullptr, 0, nullptr) != 0)
        {
            lose(); // ESRCH: the keeper is gone already
            return;
        }
        loop.spawn(watchKeeper(shared_from_this()));
    }
#endif

    /// Hands every queued message to its handler, unless an answer is waited for. Loop thread.
    void dispatchInbox()
    {
        while (dispatch == Dispatch::Free && link == Link::Open && !inbox.empty())
        {
            auto received = std::move(inbox.front());
            inbox.pop_front();
            handle(received);
        }
    }

    /// Waits until @p deadline for the first message @p matches says is the answer. Loop thread. What arrives
    /// meanwhile and is not it is queued, and dispatched after this returns, never inside it. Beats every
    /// heartbeatInterval meanwhile: the loop, held here, runs no flow that would.
    /// @return The answer; or ChannelError::TimedOut at the deadline, or why the channel broke.
    template <typename Matches>
    [[nodiscard]] std::expected<ReceivedMessage, ChannelError> awaitAnswer(
        core::platform::SteadyTimePoint deadline, Matches const& matches)
    {
        dispatch = Dispatch::Held;
        auto answer =
            std::expected<ReceivedMessage, ChannelError> { std::unexpected { ChannelError::TimedOut } };
        auto nextBeat = loop.clock().now() + heartbeatInterval;
        while (true)
        {
            auto received = channel.receive(loop.clock(), std::min(deadline, nextBeat));
            if (!received && received.error() == ChannelError::TimedOut && loop.clock().now() < deadline)
            {
                beat();
                nextBeat = loop.clock().now() + heartbeatInterval;
                continue;
            }
            if (!received)
            {
                answer = std::unexpected { received.error() };
                break;
            }
            if (received->message && matches(*received->message))
            {
                answer = std::move(*received);
                break;
            }
            inbox.push_back(std::move(*received));
        }
        // What came in the same read as the answer is not left on the channel, where nothing would look for
        // it until the keeper writes again.
        for (auto& message: channel.takeReady())
            inbox.push_back(std::move(message));
        dispatch = Dispatch::Free;
        if (!inbox.empty())
            loop.post([weak = weak_from_this()] {
                if (auto const self = weak.lock())
                    self->dispatchInbox();
            });
        return answer;
    }

    /// Tells the keeper the daemon is alive, from a wait that holds the loop. What the socket does not take
    /// now goes once the loop runs again, as anything queued does.
    void beat()
    {
        if (enqueue(keeper::Heartbeat {}, {}) == QueueOutcome::Queued)
            flush();
    }

    void handle(ReceivedMessage& received)
    {
        if (!received.message)
        {
            post(keeper::Unsupported { .tag = received.message.error().tag }, {});
            return;
        }
        std::visit([&](auto const& message) { handle(message); }, *received.message);
    }

    void handle(keeper::Spawned const& message)
    {
        // The answer to a spawn given up on: nothing of this daemon holds the child, so the keeper lets it
        // go.
        if (abandoned.erase(message.request) == 0)
            return;
        errorLog()("the keeper answered spawn {} after it was given up on; session {} released",
                   message.request,
                   message.session);
        post(keeper::Release { .session = message.session, .graceMs = 0 }, {});
    }

    void handle(keeper::SpawnFailed const& message) { abandoned.erase(message.request); }

    void handle(keeper::Exited const& message) const
    {
        if (children.exited)
            children.exited(keeper::KeeperSessionId { message.session }, message.status.toExitStatus());
    }

    static void handle(keeper::Unsupported const& message)
    {
        errorLog()("the keeper does not know the daemon's message {}; ignored", message.tag);
    }

    template <typename Other>
    static void handle(Other const& /*message*/)
    {
        // What only a handshake reads, or what the keeper never sends a daemon: nothing to act on. Its
        // descriptors close with it.
    }

    // }}}
};

KeeperLink::KeeperLink(core::net::EventLoop& loop,
                       KeeperChannel channel,
                       uint32_t channelVersion,
                       std::function<void()> onLost,
                       std::chrono::milliseconds spawnReplyBound,
                       std::chrono::milliseconds heartbeatInterval):
    _state { std::make_shared<State>(
        loop, std::move(channel), channelVersion, std::move(onLost), spawnReplyBound, heartbeatInterval) }
{
    loop.spawn(State::readLoop(_state));
#ifdef CONTOUR_KEEPER_KQUEUE
    _state->armKeeperWatch();
#endif
}

KeeperLink::~KeeperLink()
{
    auto& state = *_state;
    // Only an end the daemon announced sends what is still queued: the Shutdown, if it is not gone yet, and
    // the Releases of the sessions the host let go as it was destroyed. Any other end is a crash to the
    // keeper, and what is queued is dropped. That is only the last line: the host goes before the link, and
    // may turn the loop as it goes, so runDaemon() seals the link before the host (seal()), and no Release of
    // an unannounced end ever leaves.
    if (state.link == Link::Open && state.farewell == Farewell::Announced)
        std::ignore = state.flushUntil(state.loop.clock().now() + FinalFlushBound);
    else
        state.dropQueued();
    state.link = Link::Closing;
    state.onLost = {};
    state.children = {};
    // The flows still parked on the socket are woken to unwind; the socket closes with the last of them.
    state.loop.notifyHandleClosing(state.channel.handle(), core::net::FdWakePolicy::Cancel);
#ifdef CONTOUR_KEEPER_KQUEUE
    if (state.parentWatch.isOpen())
        state.loop.notifyHandleClosing(state.parentWatch.get(), core::net::FdWakePolicy::Cancel);
#endif
}

std::expected<SpawnReply, keeper::SpawnError> KeeperLink::spawn(keeper::Spawn request, std::vector<int> fds)
{
    auto& state = *_state;
    if (state.link != Link::Open || state.writeSide == WriteSide::Closed)
        return std::unexpected { keeper::SpawnError::Unknown };
    request.request = ++state.lastRequest;
    auto const id = request.request;
    auto const deadline = state.loop.clock().now() + state.spawnReplyBound;
    if (state.enqueue(request, fds) == QueueOutcome::Dropped)
        return std::unexpected { keeper::SpawnError::TooManyDescriptors };
    if (!state.flushUntil(deadline))
    {
        // Still queued, it may yet go out and be answered.
        state.abandoned.insert(id);
        return std::unexpected { keeper::SpawnError::Unknown };
    }

    auto answer = state.awaitAnswer(deadline, [id](keeper::Message const& message) {
        if (auto const* spawned = std::get_if<keeper::Spawned>(&message))
            return spawned->request == id;
        if (auto const* failed = std::get_if<keeper::SpawnFailed>(&message))
            return failed->request == id;
        return false;
    });
    if (!answer)
    {
        if (answer.error() == ChannelError::TimedOut)
            state.abandoned.insert(id);
        else
            state.lose();
        return std::unexpected { keeper::SpawnError::Unknown };
    }
    if (auto const* failed = std::get_if<keeper::SpawnFailed>(&*answer->message))
        return std::unexpected { failed->error };
    auto handle = keeper::UniqueFd {};
    if (!answer->fds.empty())
        handle = std::move(answer->fds.front());
    return SpawnReply { .spawned = std::get<keeper::Spawned>(*answer->message),
                        .processHandle = std::move(handle) };
}

void KeeperLink::announceShutdown(uint8_t cause, std::chrono::milliseconds bound)
{
    auto& state = *_state;
    if (state.link != Link::Open || state.writeSide == WriteSide::Closed)
        return;
    auto const deadline = state.loop.clock().now() + bound;
    if (state.enqueue(keeper::Shutdown { .cause = cause }, {}) == QueueOutcome::Dropped)
        return;
    state.farewell = Farewell::Announced;
    if (!state.flushUntil(deadline))
        return; // still queued: the link's last flush tries once more as it goes
    auto const noted = state.awaitAnswer(deadline, [](keeper::Message const& message) {
        return std::holds_alternative<keeper::ShutdownNoted>(message);
    });
    if (noted)
        return;
    if (noted.error() != ChannelError::TimedOut)
        state.lose();
    else
        errorLog()("the keeper did not take note of the shutdown within {} ms; a new instance may find the "
                   "label still held for a moment",
                   bound.count());
}

std::expected<void, ChannelError> KeeperLink::announceExec(std::filesystem::path const& executable,
                                                           std::chrono::milliseconds bound)
{
    auto& state = *_state;
    // What the queue holds goes nowhere once the keeper is gone: flushNow() would drop it and call it sent.
    auto const reachable = [&]() -> std::expected<void, ChannelError> {
        if (state.link != Link::Open)
            return std::unexpected { ChannelError::Broken };
        if (state.writeSide == WriteSide::Closed)
            return std::unexpected { ChannelError::Closed };
        return {};
    };
    if (auto const open = reachable(); !open)
        return open;
    auto const deadline = state.loop.clock().now() + bound;
    if (state.enqueue(keeper::Exec { .executablePath = executable.string() }, {}) == QueueOutcome::Dropped)
        return std::unexpected { ChannelError::Broken };
    // Called off: what is still queued, the Exec included, goes on as the daemon serves on, and ExecAborted
    // after it tells the keeper to send what it held back.
    auto const callOff = [&](ChannelError error) -> std::expected<void, ChannelError> {
        abortExec();
        return std::unexpected { error };
    };
    if (auto const flushed = state.flushUntil(deadline); !flushed)
        return callOff(flushed.error());
    if (auto const open = reachable(); !open)
        return open;
    auto const noted = state.awaitAnswer(deadline, [](keeper::Message const& message) {
        return std::holds_alternative<keeper::ExecNoted>(message);
    });
    if (!noted)
    {
        if (noted.error() != ChannelError::TimedOut)
            state.lose();
        return callOff(noted.error());
    }
    // What came before the note is this image's to handle, now: the exec would drop it.
    state.dispatchInbox();
    // What a handler posted (a Release, an Unsupported) goes whole too, or the exec is called off. Once this
    // drained, nothing is half-sent: only this thread sends, and it sends no more.
    auto const drained = state.flushUntil(deadline);
    if (auto const open = reachable(); !open)
        return open;
    if (!drained)
    {
        errorLog()(
            "the keeper did not take what the daemon sent before the exec in time; the exec is called off");
        return callOff(drained.error());
    }
    // The keeper sends nothing after the note, so nothing may be left half-read.
    if (!state.inbox.empty() || !state.channel.isAtFrameBoundary())
    {
        errorLog()("the keeper's channel holds a message begun but not read whole at the exec; the exec is "
                   "called off");
        return callOff(ChannelError::Broken);
    }
    return {};
}

void KeeperLink::abortExec()
{
    _state->post(keeper::ExecAborted {}, {});
}

void KeeperLink::seal()
{
    auto& state = *_state;
    if (state.farewell == Farewell::Announced || state.link != Link::Open)
        return;
    state.link = Link::Closing;
    state.dropQueued();
}

void KeeperLink::beat()
{
    auto& state = *_state;
    if (state.link == Link::Open && state.enqueue(keeper::Heartbeat {}, {}) == QueueOutcome::Queued)
        state.flush();
}

void KeeperLink::post(keeper::Message const& message, std::vector<int> const& fds)
{
    _state->post(message, fds);
}

void KeeperLink::attachChildren(ChildEvents events)
{
    _state->children = std::move(events);
}

int KeeperLink::channelHandle() const noexcept
{
    return _state->channel.handle();
}

uint32_t KeeperLink::channelVersion() const noexcept
{
    return _state->channelVersion;
}

bool KeeperLink::lost() const noexcept
{
    return _state->link != Link::Open;
}

} // namespace vthost::keeperlink
