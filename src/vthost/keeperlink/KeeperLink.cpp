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
        Closing,  ///< The link is being destroyed; nothing is reported any more.
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

    /// Whether a spawn() waits for its answer, so that what arrives meanwhile waits too.
    enum class Dispatch : uint8_t
    {
        Free = 0,
        InsideSpawn,
    };
} // namespace

struct KeeperLink::State: std::enable_shared_from_this<State>
{
    State(core::net::EventLoop& loopRef,
          KeeperChannel channelValue,
          uint32_t version,
          std::function<void()> lostHandler,
          std::chrono::milliseconds replyBound):
        loop { loopRef },
        channel { std::move(channelValue) },
        channelVersion { version },
        onLost { std::move(lostHandler) },
        spawnReplyBound { replyBound }
    {
    }

    core::net::EventLoop& loop;
    KeeperChannel channel;
    uint32_t channelVersion; ///< As the handshake negotiated it.
    std::function<void()> onLost;
    std::chrono::milliseconds spawnReplyBound;
    /// The spawns given up on whose answer has not come: a late Spawned for one is released.
    std::set<uint64_t> abandoned;
    ChildEvents children; ///< @see KeeperLink::attachChildren

    std::mutex queueLock;
    std::deque<Outgoing> queue; ///< Guarded by queueLock; sent from the loop thread only.

    Link link = Link::Open;
    WriteSide writeSide = WriteSide::Open;
    Writing writing = Writing::Ready;
    Dispatch dispatch = Dispatch::Free;
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

    /// Hands every queued message to its handler, unless a spawn() waits for its answer. Loop thread.
    void dispatchInbox()
    {
        while (dispatch == Dispatch::Free && link == Link::Open && !inbox.empty())
        {
            auto received = std::move(inbox.front());
            inbox.pop_front();
            handle(received);
        }
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
                       std::chrono::milliseconds spawnReplyBound):
    _state { std::make_shared<State>(
        loop, std::move(channel), channelVersion, std::move(onLost), spawnReplyBound) }
{
    loop.spawn(State::readLoop(_state));
#ifdef CONTOUR_KEEPER_KQUEUE
    _state->armKeeperWatch();
#endif
}

KeeperLink::~KeeperLink()
{
    auto& state = *_state;
    // A Shutdown posted from the shutdown sequence is what lets the keeper tell a clean end from a crash.
    if (state.link == Link::Open)
        std::ignore = state.flushUntil(state.loop.clock().now() + FinalFlushBound);
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

    state.dispatch = Dispatch::InsideSpawn;
    auto reply =
        std::expected<SpawnReply, keeper::SpawnError> { std::unexpected { keeper::SpawnError::Unknown } };
    while (true)
    {
        auto received = state.channel.receive(state.loop.clock(), deadline);
        if (!received)
        {
            if (received.error() == ChannelError::TimedOut)
                state.abandoned.insert(id);
            else
                state.lose();
            break;
        }
        if (received->message)
        {
            if (auto* const spawned = std::get_if<keeper::Spawned>(&*received->message);
                spawned && spawned->request == id)
            {
                auto handle = keeper::UniqueFd {};
                if (!received->fds.empty())
                    handle = std::move(received->fds.front());
                reply = SpawnReply { .spawned = *spawned, .processHandle = std::move(handle) };
                break;
            }
            if (auto const* failed = std::get_if<keeper::SpawnFailed>(&*received->message);
                failed && failed->request == id)
            {
                reply = std::unexpected { failed->error };
                break;
            }
        }
        state.inbox.push_back(std::move(*received));
    }
    // What came in the same read as the answer is not left on the channel, where nothing would look for it
    // until the keeper writes again.
    for (auto& message: state.channel.takeReady())
        state.inbox.push_back(std::move(message));
    state.dispatch = Dispatch::Free;
    if (!state.inbox.empty())
        state.loop.post([weak = std::weak_ptr { _state }] {
            if (auto const self = weak.lock())
                self->dispatchInbox();
        });
    return reply;
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
