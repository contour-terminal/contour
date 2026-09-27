// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/ImsgServer.hpp>

#include <vthost/Logging.hpp>

#ifndef _WIN32

    #include <core/Utils.hpp>
    #include <core/async/WhenAll.hpp>
    #include <core/net/Sockets.hpp>
    #include <core/net/SplitSocket.hpp>
    #include <core/net/WriteQueue.hpp>

    #include <algorithm>
    #include <array>
    #include <chrono>
    #include <cstddef>
    #include <cstring>
    #include <string>
    #include <string_view>
    #include <tuple>
    #include <vector>

    #include <unistd.h>

    #include <vthost/imsg/CommandArgv.hpp>
    #include <vthost/imsg/Identify.hpp>
    #include <vthost/imsg/ImsgCodec.hpp>
    #include <vthost/tmux/ControlSession.hpp>

namespace vthost::tmux
{

using namespace std::chrono_literals;

namespace
{
    /// The startup commands we serve; anything else is rejected (a documented
    /// deviation: the real server executes arbitrary startup commands).
    constexpr auto AttachVerbs = std::to_array<std::string_view>({
        "attach-session",
        "attach",
        "new-session",
        "new",
    });

    /// The most MSG_* bytes one connection may have waiting behind the frame being written. The
    /// frames sent after attach are a few bytes each; only a client that stopped reading gets near it.
    constexpr auto ImsgBacklogBound = std::size_t { 64 } * 1024;

    [[nodiscard]] std::vector<std::byte> encodeImsg(uint32_t type, std::span<std::byte const> payload)
    {
        return imsg::encodeFrame(type, payload, /*hasFd=*/false, static_cast<uint32_t>(::getpid()));
    }

    /// Writes one frame directly. Only for the handshake, where the connection has one flow and so
    /// one writer; once attached, every frame goes through the connection's WriteQueue.
    [[nodiscard]] core::async::Task<void> sendImsg(core::net::ISocket* socket,
                                                   uint32_t type,
                                                   std::span<std::byte const> payload)
    {
        auto const wire = encodeImsg(type, payload);
        std::ignore = co_await socket->write(wire);
    }

    /// Queues one frame on the connection's single writer. A refusal means the queue has failed or
    /// closed: the connection is ending, and there is nobody left to tell.
    void enqueueImsg(core::net::WriteQueue* writer, uint32_t type, std::span<std::byte const> payload)
    {
        auto const wire = encodeImsg(type, payload);
        std::ignore =
            writer->enqueue(std::string { reinterpret_cast<char const*>(wire.data()), wire.size() });
    }

    /// MSG_EXIT payload: int32 retval, optionally followed by a NUL message.
    [[nodiscard]] std::vector<std::byte> exitPayload(int32_t retval, std::string_view message)
    {
        auto payload = std::vector<std::byte>(sizeof(int32_t));
        std::memcpy(payload.data(), &retval, sizeof(int32_t));
        if (!message.empty())
        {
            auto const* begin = reinterpret_cast<std::byte const*>(message.data());
            payload.insert(payload.end(), begin, begin + message.size());
            payload.push_back(std::byte { 0 });
        }
        return payload;
    }

    [[nodiscard]] core::async::Task<void> sendExit(core::net::ISocket* socket,
                                                   int32_t retval,
                                                   std::string message)
    {
        co_await sendImsg(socket, imsg::msgtype::Exit, exitPayload(retval, message));
    }

    /// The imsg-side lifecycle loop while the control session serves: answers
    /// MSG_EXITING with MSG_EXITED and unwinds the bridge when this arm ends.
    /// It reads @p socket, but writes only through @p writer, which the other arm writes through too.
    [[nodiscard]] core::async::Task<void> imsgLifecycle(core::net::ISocket* socket,
                                                        core::net::WriteQueue* writer,
                                                        imsg::ImsgDecoder* decoder,
                                                        core::net::ISocket* bridge)
    {
        // EVERY exit closes the bridge, not just the EOF one. `serveImsgClient` awaits this arm
        // together with `control->run()`, and run() returns only once the bridge — the passed
        // stdin/stdout pair — goes away; unwinding the control session through its transport is
        // the only way to end it from here. A `co_return` that skipped this (a decoder framing
        // error, or a client-sent MSG_EXITING) left run() parked forever, so the whenAll never
        // resolved: the connection was never closed, the ScopedStreamSubscription never
        // released, and the ControlSession plus every pane it drives stayed resident until daemon
        // shutdown — one leaked session, and one leaked fd, per malformed frame. A scope guard
        // rather than three call sites, so a fourth exit cannot forget.
        auto const unwindControlSession = core::Finally([bridge]() noexcept { bridge->close(); });

        auto buffer = std::array<std::byte, 4096> {};
        while (true)
        {
            // Drain frames queued from the handshake reads first.
            while (true)
            {
                auto frame = decoder->next();
                if (!frame.has_value())
                    co_return; // protocol error: the connection is done
                if (!frame->has_value())
                    break;
                if ((*frame)->type == imsg::msgtype::Exiting)
                {
                    enqueueImsg(writer, imsg::msgtype::Exited, {});
                    co_return;
                }
                // Everything else a control client may send here is ignored.
            }

            auto const r = co_await socket->readWithFd(buffer);
            if (!r || r->bytesRead == 0)
                co_return; // the client vanished
            decoder->feed(std::span { buffer.data(), r->bytesRead }, r->fd);
        }
    }

    /// One binary tmux client's whole lifetime.
    core::async::Task<void> serveImsgClient(core::net::EventLoop* loop,
                                            SessionHost* host,
                                            ConnectionId id,
                                            std::unique_ptr<core::net::ISocket> connection)
    {
        auto decoder = imsg::ImsgDecoder {};
        auto state = imsg::IdentifyState {};
        auto startupOk = false;
        auto buffer = std::array<std::byte, 4096> {};

        // Phase 1+2: identify, then the MSG_COMMAND startup command. The
        // client pipelines everything, so frames may arrive in one chunk.
        while (!startupOk)
        {
            auto const r = co_await connection->readWithFd(buffer);
            if (!r || r->bytesRead == 0)
            {
                tmuxLog()("{}: imsg client closed before identifying", id);
                connection->close();
                co_return;
            }
            decoder.feed(std::span { buffer.data(), r->bytesRead }, r->fd);

            while (!startupOk)
            {
                auto frame = decoder.next();
                if (!frame.has_value())
                {
                    errorLog()("{}: imsg framing violation during handshake", id);
                    connection->close();
                    co_return; // framing violation
                }
                if (!frame->has_value())
                    break; // need more bytes

                // The version rides in peerid's low byte on every message but
                // MSG_VERSION; a mismatch answers MSG_VERSION and drops.
                if ((*frame)->type != imsg::msgtype::Version
                    && ((*frame)->peerid & 0xFF) != imsg::ProtocolVersion)
                {
                    errorLog()("{}: imsg protocol version mismatch (peer {}, ours {})",
                               id,
                               (*frame)->peerid & 0xFF,
                               imsg::ProtocolVersion);
                    co_await sendImsg(connection.get(), imsg::msgtype::Version, {});
                    connection->close();
                    co_return;
                }

                if (!state.done)
                {
                    if (!imsg::applyIdentify(state, std::move(**frame)).has_value())
                    {
                        errorLog()("{}: imsg MSG_IDENTIFY_* rejected", id);
                        connection->close();
                        co_return;
                    }
                    if (!state.done)
                        continue;
                    if (auto accepted = imsg::checkAcceptance(state); !accepted)
                    {
                        auto reason = imsg::rejectMessage(accepted.error());
                        errorLog()("{}: imsg client rejected: {}", id, reason);
                        co_await sendExit(connection.get(), 1, std::move(reason));
                        connection->close();
                        co_return;
                    }
                    continue;
                }

                // Identified and accepted: the next frame must be the startup
                // command.
                if ((*frame)->type != imsg::msgtype::Command)
                    continue; // pre-attach lifecycle noise: ignore
                auto const argv = imsg::unpackArgv((*frame)->payload);
                if (!argv.has_value())
                {
                    errorLog()("{}: malformed MSG_COMMAND argv", id);
                    connection->close();
                    co_return;
                }
                if (!argv->empty()
                    && std::ranges::find(AttachVerbs, std::string_view { argv->front() })
                           == AttachVerbs.end())
                {
                    errorLog()("{}: unsupported startup command '{}'; only attach-session and "
                               "new-session are served",
                               id,
                               argv->front());
                    co_await sendExit(connection.get(),
                                      1,
                                      "contour daemon: unsupported startup command; use attach-session");
                    connection->close();
                    co_return;
                }
                startupOk = true;
                tmuxLog()("{}: imsg client identified and attached", id);
            }
        }

        // Phase 3: the control-mode line protocol over the PASSED descriptors.
        auto stdinSocket = core::net::adoptFd(*loop, state.stdinFd.release());
        auto stdoutSocket = core::net::adoptFd(*loop, state.stdoutFd.release());
        if (!stdinSocket || !stdoutSocket)
        {
            errorLog()("{}: cannot adopt the passed stdin/stdout descriptors", id);
            connection->close();
            co_return;
        }
        auto bridge = core::net::combineHalves(std::move(*stdinSocket), std::move(*stdoutSocket));
        auto* bridgeView = bridge.get();

        auto session = std::make_unique<ControlSession>(
            *loop,
            *host,
            std::move(id),
            std::move(bridge),
            [] {
                return std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            },
            ControlSessionOptions { .emitExitLine = false, .initialGuardFlag = 0 });
        auto const subscription = makeScopedStreamSubscription(*host, *session);

        // Both arms below write to the imsg socket, and neither waits for the other: MSG_EXIT
        // follows run(), MSG_EXITED answers the client's MSG_EXITING whenever it comes. Written
        // directly, the second could be armed while the first is parked on a full socket, which
        // core-cpp ends the process for -- every session with it. One queue is one writer.
        auto writer = core::net::WriteQueue { *loop, connection.get(), ImsgBacklogBound };
        auto flushed = false;
        auto const closeUnflushed = core::Finally([&writer, &flushed]() noexcept {
            if (!flushed)
                writer.close();
        });

        // run() drains its stdout before returning (the control_all_done
        // gating); only then does MSG_EXIT go out on the imsg socket.
        auto serveAndExit = [](core::net::WriteQueue* queue,
                               ControlSession* control) -> core::async::Task<void> {
            co_await control->run();
            enqueueImsg(queue, imsg::msgtype::Exit, exitPayload(0, {}));
        };
        co_await core::async::whenAll(serveAndExit(&writer, session.get()),
                                      imsgLifecycle(connection.get(), &writer, &decoder, bridgeView));
        // Closing the queue closes the connection.
        co_await writer.flushThenClose();
        flushed = true;
    }
} // namespace

ConnectionHandler makeTmuxImsgHandler(core::net::EventLoop& loop, SessionHost& host)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host](ConnectionId id, std::unique_ptr<core::net::ISocket> connection) {
        return serveImsgClient(&loop, &host, std::move(id), std::move(connection));
    };
}

} // namespace vthost::tmux

#else

namespace vthost::tmux
{

ConnectionHandler makeTmuxImsgHandler(core::net::EventLoop&, SessionHost&)
{
    return {};
}

} // namespace vthost::tmux

#endif
