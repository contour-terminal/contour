// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConnectProbe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include <vthost/Daemon.hpp>
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

TEST_CASE("ConnectProbe.a host that fails says why in the transport's words", "[contour][connect]")
{
    auto missing = FakeControl { std::unexpected { failure(
        ControlErrorCode::Transport, "Contour isn't installed on starfive (contour: command not found).") } };
    auto const result = checkHostReachable(missing);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().contains("Contour isn't installed on starfive"));
}
