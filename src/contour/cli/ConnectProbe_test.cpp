// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConnectProbe.hpp>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemoryTransport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <vthost/DaemonSpawn.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>

using contour::cli::checkHostReachable;
using contour::cli::DaemonRunState;
using contour::cli::probeLocalDaemon;
using vthost::client::ControlErrorCode;

namespace
{
class FakeSpawner final: public vthost::DaemonSpawner
{
  public:
    explicit FakeSpawner(vthost::DaemonPresence presence): _presence(presence) {}

    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> /*argv*/,
                                                         vtpty::ProcessPlacement& /*placement*/) override
    {
        return std::unexpected { std::string { "a probe never spawns" } };
    }

    [[nodiscard]] vthost::DaemonPresence probe(std::filesystem::path const& socketPath) override
    {
        probed = socketPath;
        return _presence;
    }

    std::filesystem::path probed;

  private:
    vthost::DaemonPresence _presence;
};

class FakeControl final: public vthost::client::DaemonControlPort
{
  public:
    explicit FakeControl(std::expected<vthost::client::DaemonStatus, vthost::client::ControlError> answer):
        _answer(std::move(answer))
    {
    }

    [[nodiscard]] std::expected<vthost::client::DaemonStatus, vthost::client::ControlError> status() override
    {
        return _answer;
    }

    [[nodiscard]] std::expected<void, vthost::client::ControlError> stop(vthost::proto::StopReason) override
    {
        return {};
    }

    [[nodiscard]] std::expected<vthost::proto::UpgradeDaemonReply, vthost::client::ControlError> upgrade(
        std::string) override
    {
        return vthost::proto::UpgradeDaemonReply {};
    }

  private:
    std::expected<vthost::client::DaemonStatus, vthost::client::ControlError> _answer;
};

[[nodiscard]] vthost::client::ControlError failure(ControlErrorCode code, std::string detail)
{
    return vthost::client::ControlError { .code = code, .detail = std::move(detail) };
}
} // namespace

TEST_CASE("ConnectProbe.a daemon that accepts is running, with its sessions", "[contour][connect]")
{
    auto spawner = FakeSpawner { vthost::DaemonPresence::Serving };
    auto const state =
        probeLocalDaemon(spawner, "/run/contour/default", [](std::filesystem::path const& socket) {
            CHECK(socket == "/run/contour/default");
            return std::optional<std::size_t> { 3 };
        });
    CHECK(state.run == DaemonRunState::Running);
    CHECK(state.sessions == 3);
    CHECK(spawner.probed == "/run/contour/default");
}

TEST_CASE("ConnectProbe.a daemon that does not say its sessions is still running", "[contour][connect]")
{
    auto spawner = FakeSpawner { vthost::DaemonPresence::Serving };
    auto const state = probeLocalDaemon(spawner, "/s", [](auto const&) { return std::nullopt; });
    CHECK(state.run == DaemonRunState::Running);
    CHECK_FALSE(state.sessions.has_value());
}

TEST_CASE("ConnectProbe.an absent daemon is not asked for sessions", "[contour][connect]")
{
    auto spawner = FakeSpawner { vthost::DaemonPresence::Absent };
    auto asked = false;
    auto const state = probeLocalDaemon(spawner, "/s", [&](auto const&) {
        asked = true;
        return std::optional<std::size_t> { 1 };
    });
    CHECK(state.run == DaemonRunState::NotRunning);
    CHECK_FALSE(asked);
}

TEST_CASE("ConnectProbe.a host that answers is reachable, an old daemon too", "[contour][connect]")
{
    auto answers = FakeControl { vthost::client::DaemonStatus {} };
    CHECK(checkHostReachable(answers).has_value());
    auto old = FakeControl { std::unexpected { failure(ControlErrorCode::Unsupported, "") } };
    CHECK(checkHostReachable(old).has_value());
}

TEST_CASE("ConnectProbe.a login that wants an answer the check may not ask for is reachable",
          "[contour][connect]")
{
    // The check never prompts: where ssh would ask (a password, a host key) the host has answered, and the
    // window that attaches asks, so the user answers once.
    auto needsLogin = FakeControl { std::unexpected {
        failure(ControlErrorCode::LoginNeeded, "ssh could not log in to ssh://me@homepc") } };
    CHECK(checkHostReachable(needsLogin).has_value());

    // A changed host key is not a login to answer: the host is not the one on record, and the check says so.
    auto changedKey = FakeControl { std::unexpected {
        failure(ControlErrorCode::Transport, "The host key of ssh://me@homepc has changed.") } };
    auto const changed = checkHostReachable(changedKey);
    REQUIRE_FALSE(changed.has_value());
    CHECK(changed.error().contains("has changed"));

    // Any other failure of the connection is still one.
    auto broken = FakeControl { std::unexpected { failure(ControlErrorCode::Transport, "no route") } };
    CHECK_FALSE(checkHostReachable(broken).has_value());
}

TEST_CASE("ConnectProbe.a host that fails says why in the transport's words", "[contour][connect]")
{
    auto missing = FakeControl { std::unexpected { failure(
        ControlErrorCode::Transport, "Contour isn't installed on starfive (contour: command not found).") } };
    auto const result = checkHostReachable(missing);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains("Contour isn't installed on starfive"));
}

namespace
{
/// Holds the far end of a connection open and says nothing: a daemon wedged in the handshake. Owned by the
/// loop it was spawned on.
core::async::Task<void> saySomethingNever(std::unique_ptr<core::net::ISocket> remote)
{
    auto buffer = std::vector<std::byte>(256);
    while (true)
    {
        auto const read = co_await remote->read(buffer);
        if (!read.has_value() || *read == 0)
            co_return;
    }
}

/// Hands out connections to a daemon that never answers.
class WedgedTransport final: public vthost::ITransportFactory
{
  public:
    [[nodiscard]] core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connect(core::net::EventLoop& loop) override
    {
        return connectOn(&loop);
    }

    [[nodiscard]] std::string describe() const override { return "wedged"; }
    [[nodiscard]] vthost::PeerTrust peerTrust() const noexcept override
    {
        return vthost::PeerTrust::Verified;
    }

  private:
    /// The coroutine behind connect(), which takes the loop by pointer: a coroutine's parameters outlive
    /// the call only by value.
    [[nodiscard]] static core::async::Task<
        std::expected<std::unique_ptr<vthost::Connection>, vthost::TransportError>>
    connectOn(core::net::EventLoop* loop)
    {
        auto pair = core::net::testing::makeSocketPair(*loop);
        loop->spawn(saySomethingNever(std::move(pair->first)));
        co_return std::make_unique<vthost::Connection>(
            vthost::Connection { .lifetime = {}, .socket = std::move(pair->second), .token = {} });
    }
};
} // namespace

TEST_CASE("ConnectProbe.a host control with no connect bound still bounds the handshake",
          "[contour][connect]")
{
    using namespace std::chrono_literals;
    auto const control = contour::cli::makeHostControl(std::make_shared<WedgedTransport>(),
                                                       vthost::AttachBudget::promptsOpen(300ms));
    auto const started = std::chrono::steady_clock::now();
    auto const result = checkHostReachable(*control);
    auto const took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(result.has_value());
    CHECK(took >= 250ms);
    CHECK(took < 5s);
}
