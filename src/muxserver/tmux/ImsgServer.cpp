// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/ImsgServer.h>

#ifndef _WIN32

    #include <algorithm>
    #include <array>
    #include <chrono>
    #include <cstring>
    #include <string>
    #include <vector>

    #include <unistd.h>

    #include <coro/WhenAll.hpp>
    #include <muxserver/imsg/CommandArgv.h>
    #include <muxserver/imsg/Identify.h>
    #include <muxserver/imsg/ImsgCodec.h>
    #include <muxserver/tmux/ControlSession.h>
    #include <net/Sockets.h>
    #include <net/SplitSocket.h>

namespace muxserver::tmux
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

    [[nodiscard]] coro::Task<void> sendImsg(net::ISocket* socket,
                                            uint32_t type,
                                            std::span<std::byte const> payload)
    {
        auto const wire =
            imsg::encodeFrame(type, payload, /*hasFd=*/false, static_cast<uint32_t>(::getpid()));
        std::ignore = co_await socket->write(wire);
    }

    /// MSG_EXIT payload: int32 retval, optionally followed by a NUL message.
    [[nodiscard]] coro::Task<void> sendExit(net::ISocket* socket, int32_t retval, std::string message)
    {
        auto payload = std::vector<std::byte>(sizeof(int32_t));
        std::memcpy(payload.data(), &retval, sizeof(int32_t));
        if (!message.empty())
        {
            auto const* begin = reinterpret_cast<std::byte const*>(message.data());
            payload.insert(payload.end(), begin, begin + message.size());
            payload.push_back(std::byte { 0 });
        }
        co_await sendImsg(socket, imsg::msgtype::Exit, payload);
    }

    /// The imsg-side lifecycle loop while the control session serves: answers
    /// MSG_EXITING with MSG_EXITED and unwinds the bridge on socket EOF.
    [[nodiscard]] coro::Task<void> imsgLifecycle(net::ISocket* socket,
                                                 imsg::ImsgDecoder* decoder,
                                                 net::ISocket* bridge)
    {
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
                    co_await sendImsg(socket, imsg::msgtype::Exited, {});
                    co_return;
                }
                // Everything else a control client may send here is ignored.
            }

            auto const r = co_await socket->readWithFd(buffer);
            if (!r || r->bytesRead == 0)
            {
                // The client vanished: unwind the control session via its
                // transport so run() completes.
                bridge->close();
                co_return;
            }
            decoder->feed(std::span { buffer.data(), r->bytesRead }, r->fd);
        }
    }

    /// One binary tmux client's whole lifetime.
    coro::Task<void> serveImsgClient(net::EventLoop* loop,
                                     SessionHost* host,
                                     std::unique_ptr<net::ISocket> connection)
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
                connection->close();
                co_return;
            }
            decoder.feed(std::span { buffer.data(), r->bytesRead }, r->fd);

            while (!startupOk)
            {
                auto frame = decoder.next();
                if (!frame.has_value())
                {
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
                    co_await sendImsg(connection.get(), imsg::msgtype::Version, {});
                    connection->close();
                    co_return;
                }

                if (!state.done)
                {
                    if (!imsg::applyIdentify(state, std::move(**frame)).has_value())
                    {
                        connection->close();
                        co_return;
                    }
                    if (!state.done)
                        continue;
                    if (auto accepted = imsg::checkAcceptance(state); !accepted)
                    {
                        co_await sendExit(connection.get(), 1, imsg::rejectMessage(accepted.error()));
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
                    connection->close();
                    co_return;
                }
                if (!argv->empty()
                    && std::ranges::find(AttachVerbs, std::string_view { argv->front() })
                           == AttachVerbs.end())
                {
                    co_await sendExit(connection.get(),
                                      1,
                                      "contour daemon: unsupported startup command; use attach-session");
                    connection->close();
                    co_return;
                }
                startupOk = true;
            }
        }

        // Phase 3: the control-mode line protocol over the PASSED descriptors.
        auto stdinSocket = net::adoptFd(*loop, state.stdinFd.release());
        auto stdoutSocket = net::adoptFd(*loop, state.stdoutFd.release());
        if (!stdinSocket || !stdoutSocket)
        {
            connection->close();
            co_return;
        }
        auto bridge = net::combineHalves(std::move(*stdinSocket), std::move(*stdoutSocket));
        auto* bridgeView = bridge.get();

        auto session = std::make_unique<ControlSession>(
            *loop,
            *host,
            std::move(bridge),
            [] {
                return std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                    .count();
            },
            ControlSessionOptions { .emitExitLine = false, .initialGuardFlag = 0 });
        auto const subscription = makeScopedStreamSubscription(*host, *session);

        // run() drains its stdout before returning (the control_all_done
        // gating); only then does MSG_EXIT go out on the imsg socket.
        auto serveAndExit = [](net::ISocket* socket, ControlSession* control) -> coro::Task<void> {
            co_await control->run();
            co_await sendExit(socket, 0, {});
        };
        co_await coro::whenAll(serveAndExit(connection.get(), session.get()),
                               imsgLifecycle(connection.get(), &decoder, bridgeView));
        connection->close();
    }
} // namespace

std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeTmuxImsgHandler(net::EventLoop& loop,
                                                                                   SessionHost& host)
{
    // NOT a coroutine itself: it merely constructs the free coroutine's task,
    // so the captures never outlive an activation frame.
    return [&loop, &host](std::unique_ptr<net::ISocket> connection) {
        return serveImsgClient(&loop, &host, std::move(connection));
    };
}

} // namespace muxserver::tmux

#else

namespace muxserver::tmux
{

std::function<coro::Task<void>(std::unique_ptr<net::ISocket>)> makeTmuxImsgHandler(net::EventLoop&,
                                                                                   SessionHost&)
{
    return {};
}

} // namespace muxserver::tmux

#endif
