// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/ScriptedBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/ConnectionRegistry.hpp>
#include <vthost/DaemonUpgradeHooks.hpp>

namespace
{

/// An UpgradeObserver that records what it was told, in order, into a shared log.
class RecordingObserver final: public vthost::UpgradeObserver
{
  public:
    RecordingObserver(std::string name, std::vector<std::string>& log): _name(std::move(name)), _log(log) {}

    void announceUpgrade(std::string_view toRelease) override
    {
        _log.push_back(_name + " announced " + std::string { toRelease });
    }

    void closeForUpgrade() override { _log.push_back(_name + " closed"); }

    void abortForUpgrade() override { _log.push_back(_name + " aborted"); }

  private:
    std::string _name;
    std::vector<std::string>& _log;
};

/// Reads until @p socket ends, then records its endpoint into @p ended. A free coroutine: a capturing
/// lambda's closure would not outlive its frame.
core::async::Task<void> recordEnd(std::vector<std::string>* ended,
                                  vthost::ConnectionId id,
                                  std::unique_ptr<core::net::ISocket> socket)
{
    auto buffer = std::array<std::byte, 16> {};
    std::ignore = co_await socket->read(buffer);
    ended->push_back(id.endpoint);
}

} // namespace

TEST_CASE("the requester is answered before any client is told", "[vthost][upgrade][hooks]")
{
    auto log = std::vector<std::string> {};
    auto const registry = vthost::ConnectionRegistry {};
    auto hooks = vthost::DaemonUpgradeHooks { registry, [&log] { log.emplace_back("snapshot"); } };
    auto first = RecordingObserver { "a", log };
    auto second = RecordingObserver { "b", log };
    hooks.subscribe(first);
    hooks.subscribe(second);
    hooks.setRequester([&log] { log.emplace_back("requester answered"); });

    hooks.announceUpgrade("1.2.3");
    hooks.writeWorkspaceSnapshot();
    hooks.closeClients();

    CHECK(log
          == std::vector<std::string> { "requester answered",
                                        "a announced 1.2.3",
                                        "b announced 1.2.3",
                                        "snapshot",
                                        "a closed",
                                        "b closed" });

    // The requester is answered once.
    log.clear();
    hooks.announceUpgrade("1.2.3");
    CHECK(log == std::vector<std::string> { "a announced 1.2.3", "b announced 1.2.3" });
}

TEST_CASE("an unsubscribed observer is told nothing, and a daemon without a workspace writes none",
          "[vthost][upgrade][hooks]")
{
    auto log = std::vector<std::string> {};
    auto const registry = vthost::ConnectionRegistry {};
    auto hooks = vthost::DaemonUpgradeHooks { registry, {} };
    auto observer = RecordingObserver { "a", log };
    hooks.subscribe(observer);
    hooks.unsubscribe(observer);
    hooks.announceUpgrade("1.2.3");
    hooks.writeWorkspaceSnapshot();
    hooks.closeClients();
    CHECK(log.empty());
}

TEST_CASE("closeClients closes every tracked connection of the other protocols", "[vthost][upgrade][hooks]")
{
    auto source = core::net::testing::ScriptedBackend {};
    auto loop = core::net::EventLoop { source };
    auto ended = std::vector<std::string> {};
    auto const registry = vthost::ConnectionRegistry {};
    auto const handler =
        registry.tracked([&ended](vthost::ConnectionId id, std::unique_ptr<core::net::ISocket> socket) {
            return recordEnd(&ended, std::move(id), std::move(socket));
        });
    auto tmux = core::net::testing::InMemorySocketPair::create();
    auto imsg = core::net::testing::InMemorySocketPair::create();
    loop.spawn(
        handler(vthost::ConnectionId { .endpoint = "tmux-compat", .index = 1 }, std::move(tmux.server)));
    loop.spawn(handler(vthost::ConnectionId { .endpoint = "imsg", .index = 1 }, std::move(imsg.server)));
    std::ignore = loop.runUntilIdle();
    REQUIRE(registry.size() == 2);

    auto hooks = vthost::DaemonUpgradeHooks { registry, {} };
    hooks.closeClients();
    std::ignore = loop.runUntilIdle();

    CHECK(ended.size() == 2);
    CHECK(registry.size() == 0);
}

TEST_CASE("aborting the clients closes every observer's connection at once, and every other one",
          "[vthost][upgrade][hooks]")
{
    auto log = std::vector<std::string> {};
    auto const registry = vthost::ConnectionRegistry {};
    auto hooks = vthost::DaemonUpgradeHooks { registry, {} };
    auto first = RecordingObserver { "a", log };
    auto second = RecordingObserver { "b", log };
    hooks.subscribe(first);
    hooks.subscribe(second);

    hooks.abortClients();

    CHECK(log == std::vector<std::string> { "a aborted", "b aborted" });
}
