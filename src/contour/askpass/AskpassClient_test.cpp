// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassClient.hpp>
#include <contour/askpass/AskpassProtocol.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32

    #include <array>
    #include <chrono>
    #include <format>
    #include <functional>
    #include <future>
    #include <map>
    #include <optional>
    #include <sstream>
    #include <string>
    #include <thread>
    #include <tuple>

using namespace contour::askpass;

namespace
{
/// What the fake GUI sends back for a request: the bytes of a frame, or nothing to just hang up.
using Responder = std::function<std::optional<std::string>(AskpassRequest const&)>;

[[nodiscard]] core::async::Task<std::optional<std::string>> readFrame(core::net::ISocket* socket)
{
    auto received = std::string {};
    auto chunk = std::array<std::byte, 1024> {};
    while (true)
    {
        auto const size = askpassFrameSize(received);
        if (!size)
            co_return std::nullopt;
        if (size->has_value() && received.size() >= **size)
            co_return received.substr(0, **size);
        auto const count = co_await socket->read(chunk);
        if (!count || *count == 0)
            co_return std::nullopt;
        received.append(reinterpret_cast<char const*>(chunk.data()), *count);
    }
}

[[nodiscard]] core::async::Task<void> serveOne(core::net::IListener* listener,
                                               Responder const* respond,
                                               std::optional<AskpassRequest>* seen)
{
    auto accepted = co_await listener->accept();
    if (!accepted)
        co_return;
    auto const socket = *std::move(accepted);
    auto const frame = co_await readFrame(socket.get());
    if (!frame)
        co_return;
    auto const request = decodeAskpassRequest(*frame);
    if (!request)
        co_return;
    *seen = *request;
    if (auto const bytes = (*respond)(*request))
        std::ignore = co_await socket->write(std::as_bytes(std::span { *bytes }));
}

/// A one-connection stand-in for the GUI's QLocalServer, on a thread of its own.
class FakeGui
{
  public:
    explicit FakeGui(Responder respond):
        _respond { std::move(respond) },
        _directory { "contour-askpass" },
        _path { (_directory.path() / "sockets" / "gui").string() }
    {
        auto ready = std::promise<void> {};
        auto up = ready.get_future();
        _thread = std::thread { [this, &ready] {
            auto const source = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *source };
            auto listener = core::net::listenUnix(loop, _path);
            REQUIRE(listener.has_value());
            ready.set_value();
            loop.blockOn(serveOne(listener->get(), &_respond, &_seen));
        } };
        up.wait();
    }

    /// Every test connects once, which is what lets the server's thread end.
    ~FakeGui() { _thread.join(); }

    FakeGui(FakeGui const&) = delete;
    FakeGui& operator=(FakeGui const&) = delete;
    FakeGui(FakeGui&&) = delete;
    FakeGui& operator=(FakeGui&&) = delete;

    [[nodiscard]] std::string const& path() const { return _path; }
    [[nodiscard]] std::optional<AskpassRequest> const& seen() const { return _seen; }

  private:
    Responder _respond;
    core::testing::ScopedTempDir _directory;
    std::string _path;
    std::optional<AskpassRequest> _seen;
    std::thread _thread;
};

struct Run
{
    std::optional<int> exitCode;
    std::string out;
    std::string err;
};

[[nodiscard]] Run run(std::map<std::string, std::string, std::less<>> variables,
                      char const* prompt = "Password: ")
{
    auto const environment = core::testing::FakeEnvironment { std::move(variables) };
    auto const argv = std::array<char const*, 2> { "contour", prompt };
    auto out = std::ostringstream {};
    auto err = std::ostringstream {};
    auto const code = runAskpassIfRequested(environment, argv, out, err, std::chrono::milliseconds { 100 });
    return Run { .exitCode = code, .out = out.str(), .err = err.str() };
}
} // namespace

TEST_CASE("askpass helper: not requested without an endpoint", "[contour][askpass]")
{
    auto const result = run({});
    CHECK_FALSE(result.exitCode.has_value());
    CHECK(result.out.empty());
}

TEST_CASE("askpass helper: a missing nonce or an unreachable endpoint exits 1 with nothing on stdout",
          "[contour][askpass]")
{
    auto const noNonce = run({ { "CONTOUR_ASKPASS_ENDPOINT", "/nonexistent" } });
    CHECK(noNonce.exitCode == 1);
    CHECK(noNonce.out.empty());

    auto const unreachable =
        run({ { "CONTOUR_ASKPASS_ENDPOINT", "/nonexistent" }, { "CONTOUR_ASKPASS_NONCE", "n" } });
    CHECK(unreachable.exitCode == 1);
    CHECK(unreachable.out.empty());
}

TEST_CASE("askpass helper: an answer is printed with a newline", "[contour][askpass]")
{
    auto gui = FakeGui { [](AskpassRequest const&) {
        return std::string { encode(answeredReply("hunter2")).view() };
    } };
    auto const result = run({ { "CONTOUR_ASKPASS_ENDPOINT", gui.path() },
                              { "CONTOUR_ASKPASS_NONCE", "feed" },
                              { "SSH_ASKPASS_PROMPT", "confirm" } },
                            "Allow? ");
    CHECK(result.exitCode == 0);
    CHECK(result.out == "hunter2\n");
    CHECK(result.err.empty());
    REQUIRE(gui.seen().has_value());
    CHECK(gui.seen()->nonce == "feed");
    CHECK(gui.seen()->prompt == "Allow? ");
    CHECK(gui.seen()->sshAskpassPrompt == "confirm");
}

TEST_CASE("askpass helper: a cancelled prompt exits 1 with nothing on stdout", "[contour][askpass]")
{
    auto gui = FakeGui { [](AskpassRequest const&) {
        return std::string { encode(AskpassReply { .outcome = AskpassOutcome::Cancelled,
                                                   .answer = Secret { "ignored" } })
                                 .view() };
    } };
    auto const result =
        run({ { "CONTOUR_ASKPASS_ENDPOINT", gui.path() }, { "CONTOUR_ASKPASS_NONCE", "feed" } });
    CHECK(result.exitCode == 1);
    CHECK(result.out.empty());
}

TEST_CASE("askpass helper: a hang-up or a garbled reply exits 1 with nothing on stdout", "[contour][askpass]")
{
    SECTION("hang up")
    {
        auto gui = FakeGui { [](AskpassRequest const&) { return std::nullopt; } };
        auto const result =
            run({ { "CONTOUR_ASKPASS_ENDPOINT", gui.path() }, { "CONTOUR_ASKPASS_NONCE", "feed" } });
        CHECK(result.exitCode == 1);
        CHECK(result.out.empty());
    }
    SECTION("garbled")
    {
        auto gui = FakeGui { [](AskpassRequest const&) { return std::string { "\x03\0\0\0xyz", 7 }; } };
        auto const result =
            run({ { "CONTOUR_ASKPASS_ENDPOINT", gui.path() }, { "CONTOUR_ASKPASS_NONCE", "feed" } });
        CHECK(result.exitCode == 1);
        CHECK(result.out.empty());
    }
}

#endif
