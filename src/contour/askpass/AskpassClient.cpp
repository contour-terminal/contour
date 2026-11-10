// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassClient.hpp>
#include <contour/askpass/AskpassProtocol.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <utility>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <cstdio>

    #include <Windows.h>
    #include <fcntl.h>
    #include <io.h>
#endif

namespace contour::askpass
{

namespace
{
    using Bytes = std::expected<std::string, std::string>;

#ifdef _WIN32
    /// The exchange over a named pipe, which is what QLocalServer listens on under Windows. Blocking, and as
    /// small as it can be: one write of the frame, then reads until the reply's frame is whole.
    [[nodiscard]] Bytes exchange(std::string const& endpoint,
                                 std::string const& frame,
                                 std::chrono::milliseconds connectBudget)
    {
        auto const wide = core::toUtf16(endpoint);
        if (!wide)
            return std::unexpected { std::string { "the prompt endpoint is not a valid path" } };
        auto const deadline = std::chrono::steady_clock::now() + connectBudget;
        auto pipe = INVALID_HANDLE_VALUE;
        while (true)
        {
            pipe = CreateFileW(
                wide->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
                break;
            auto const error = GetLastError();
            auto const left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero() || error != ERROR_PIPE_BUSY)
                return std::unexpected { std::format("cannot reach the prompt (error {})", error) };
            // Busy: every instance is in use, so wait for one to free up.
            WaitNamedPipeW(wide->c_str(), static_cast<DWORD>(std::min<long long>(left.count(), 200)));
        }
        auto result = Bytes {};
        auto written = DWORD { 0 };
        if (!WriteFile(pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr))
            result = std::unexpected { std::string { "cannot send the prompt" } };
        else
        {
            auto received = std::string {};
            auto chunk = std::array<char, 4096> {};
            result = std::string {};
            while (true)
            {
                auto const size = askpassFrameSize(received);
                if (!size)
                {
                    result = std::unexpected { size.error() };
                    break;
                }
                if (size->has_value() && received.size() >= **size)
                {
                    result = received.substr(0, **size);
                    break;
                }
                auto count = DWORD { 0 };
                if (!ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr)
                    || count == 0)
                {
                    result = std::unexpected { std::string { "the prompt was closed without an answer" } };
                    break;
                }
                received.append(chunk.data(), count);
            }
            wipe(received);
        }
        CloseHandle(pipe);
        return result;
    }
#else
    /// Sends @p frame and reads the one frame that comes back.
    [[nodiscard]] core::async::Task<Bytes> exchangeOn(core::net::EventLoop* loop,
                                                      std::string endpoint,
                                                      std::string frame,
                                                      std::chrono::milliseconds connectBudget)
    {
        // Retried for the budget, should the server be busy accepting.
        auto const deadline = std::chrono::steady_clock::now() + connectBudget;
        auto connected = co_await core::net::connectUnix(loop, endpoint);
        while (!connected && std::chrono::steady_clock::now() < deadline)
        {
            co_await loop->delay(std::chrono::milliseconds { 50 });
            connected = co_await core::net::connectUnix(loop, endpoint);
        }
        if (!connected)
            co_return std::unexpected { std::format("cannot reach the prompt: {}",
                                                    connected.error().toString()) };
        auto const socket = *std::move(connected);

        auto const bytes = std::as_bytes(std::span { frame });
        if (auto const sent = co_await socket->write(bytes); !sent)
            co_return std::unexpected { std::format("cannot send the prompt: {}", sent.error().toString()) };

        auto received = std::string {};
        auto chunk = std::array<std::byte, 4096> {};
        while (true)
        {
            auto const size = askpassFrameSize(received);
            if (!size)
                co_return std::unexpected { size.error() };
            if (size->has_value() && received.size() >= **size)
            {
                auto whole = received.substr(0, **size);
                wipe(received);
                co_return whole;
            }
            auto const count = co_await socket->read(chunk);
            if (!count)
                co_return std::unexpected { std::format("cannot read the answer: {}",
                                                        count.error().toString()) };
            if (*count == 0)
                co_return std::unexpected { std::string { "the prompt was closed without an answer" } };
            received.append(reinterpret_cast<char const*>(chunk.data()), *count);
        }
    }

    [[nodiscard]] Bytes exchange(std::string const& endpoint,
                                 std::string const& frame,
                                 std::chrono::milliseconds connectBudget)
    {
        auto const source = core::net::makeDefaultBackend();
        auto loop = core::net::EventLoop { *source };
        return loop.blockOn(exchangeOn(&loop, endpoint, frame, connectBudget));
    }
#endif

    [[nodiscard]] int fail(std::ostream& err, std::string_view reason)
    {
        err << "contour askpass: " << reason << '\n';
        return 1;
    }
} // namespace

std::optional<int> runAskpassIfRequested(core::Environment const& environment,
                                         std::span<char const* const> args,
                                         std::ostream& out,
                                         std::ostream& err,
                                         std::chrono::milliseconds connectBudget)
{
    auto const endpoint = environment.get(AskpassEndpointVariable);
    if (!endpoint || endpoint->empty())
        return std::nullopt;

    auto const nonce = environment.get(AskpassNonceVariable);
    if (!nonce || nonce->empty())
        return fail(err, "no session nonce");

    auto const request =
        AskpassRequest { .nonce = *nonce,
                         .prompt = args.size() > 1 ? std::string { args[1] } : std::string {},
                         .sshAskpassPrompt = environment.get(SshAskpassPromptVariable).value_or("") };
    auto const frame = encode(request);
    if (frame.size() > AskpassFramePrefixSize + MaxAskpassPayload)
        return fail(err, "the prompt is too long");

    auto received = exchange(*endpoint, frame, connectBudget);
    if (!received)
        return fail(err, received.error());
    auto reply = decodeAskpassReply(*received);
    wipe(*received);
    if (!reply)
        return fail(err, reply.error());
    if (reply->outcome != AskpassOutcome::Answered)
        return 1;

    out << reply->answer << '\n';
    out.flush();
    wipe(reply->answer);
    return 0;
}

std::optional<int> runAskpassIfRequested(core::Environment const& environment,
                                         std::span<char const* const> args)
{
#ifdef _WIN32
    // The answer ends in a bare newline, as ssh reads it; text mode would write CR LF.
    std::ignore = _setmode(_fileno(stdout), _O_BINARY);
#endif
    return runAskpassIfRequested(environment, args, std::cout, std::cerr);
}

} // namespace contour::askpass
