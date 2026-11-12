// SPDX-License-Identifier: Apache-2.0
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

#include <vthost/AttachBudget.hpp>
#include <vthost/PromptPause.hpp>

using namespace std::chrono_literals;

namespace
{
/// A session whose prompt has been open for as long as the test says.
class FakeSession final: public vthost::IAskpassSession
{
  public:
    explicit FakeSession(std::atomic<std::chrono::milliseconds::rep> const* openFor): _openFor(openFor) {}
    [[nodiscard]] vthost::SpawnEnvironment environment() const override { return {}; }
    [[nodiscard]] bool wasCancelled() const noexcept override { return false; }
    [[nodiscard]] std::chrono::milliseconds promptTime() const noexcept override
    {
        return std::chrono::milliseconds { _openFor->load() };
    }
    void onCancelled(std::function<void()> /*wake*/) override {}

  private:
    std::atomic<std::chrono::milliseconds::rep> const* _openFor;
};

class FakeBroker final: public vthost::IAskpassBroker
{
  public:
    explicit FakeBroker(std::atomic<std::chrono::milliseconds::rep> const* openFor): _openFor(openFor) {}
    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        vthost::SshTarget const& /*target*/) override
    {
        return std::make_unique<FakeSession>(_openFor);
    }

  private:
    std::atomic<std::chrono::milliseconds::rep> const* _openFor;
};

core::async::Task<int> afterwards(core::net::EventLoop* loop, std::chrono::milliseconds wait)
{
    co_await loop->delay(wait);
    co_return 1;
}
} // namespace

TEST_CASE("a PromptPause adds up the prompts that ended and the one that is open", "[vthost][attach]")
{
    auto openFor = std::atomic<std::chrono::milliseconds::rep> { 0 };
    auto const pause = std::make_shared<vthost::PromptPause>();
    auto broker = vthost::PausingAskpassBroker { std::make_shared<FakeBroker>(&openFor), pause };
    CHECK(pause->total() == 0ms);
    {
        auto session = broker.open(vthost::SshTarget {});
        REQUIRE(session);
        openFor = 700;
        CHECK(pause->total() == 700ms); // live, while the prompt is open
        openFor = 900;
    }
    CHECK(pause->total() == 900ms); // kept once the session ended
    openFor = 100;
    auto second = broker.open(vthost::SshTarget {});
    CHECK(pause->total() == 1000ms);
}

TEST_CASE("an attach budget is extended by the prompt time its transport reports", "[vthost][attach]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto openFor = std::atomic<std::chrono::milliseconds::rep> { 0 };
    auto const pause = std::make_shared<vthost::PromptPause>();
    auto broker = vthost::PausingAskpassBroker { std::make_shared<FakeBroker>(&openFor), pause };
    auto session = broker.open(vthost::SshTarget {});
    REQUIRE(session);
    auto const budget = vthost::AttachBudget { 100ms };

    // 250 ms of work under a 100 ms budget is too long, as a plain budget says ...
    auto const plain =
        loop.blockOn(vthost::withAttachBudget(&loop, afterwards(&loop, 250ms), budget, nullptr));
    CHECK_FALSE(plain.has_value());

    // ... but not once the user spent 300 ms of it answering a prompt, from 20 ms into the attempt on.
    auto const paused =
        loop.blockOn([](core::net::EventLoop* loop,
                        std::atomic<std::chrono::milliseconds::rep>* openFor,
                        vthost::AttachBudget budget) -> core::async::Task<std::optional<int>> {
            auto prompting =
                [](core::net::EventLoop* loop,
                   std::atomic<std::chrono::milliseconds::rep>* openFor) -> core::async::Task<void> {
                co_await loop->delay(20ms);
                *openFor = 300;
            }(loop, openFor);
            auto result = std::optional<int> {};
            auto attaching = [](core::net::EventLoop* loop,
                                vthost::AttachBudget budget,
                                std::optional<int>* out) -> core::async::Task<void> {
                *out = co_await vthost::withAttachBudget(loop, afterwards(loop, 250ms), budget, nullptr);
            }(loop, budget, &result);
            co_await core::async::whenAll(std::move(prompting), std::move(attaching));
            co_return result;
        }(&loop, &openFor, budget.pausedBy(pause)));
    REQUIRE(paused.has_value());
    CHECK(*paused == 1);
}

TEST_CASE("prompts of overlapping sessions are each counted", "[vthost][attach]")
{
    auto firstFor = std::atomic<std::chrono::milliseconds::rep> { 0 };
    auto secondFor = std::atomic<std::chrono::milliseconds::rep> { 0 };
    auto const pause = std::make_shared<vthost::PromptPause>();
    auto first = vthost::PausingAskpassBroker { std::make_shared<FakeBroker>(&firstFor), pause };
    auto second = vthost::PausingAskpassBroker { std::make_shared<FakeBroker>(&secondFor), pause };

    auto one = first.open(vthost::SshTarget {});
    auto two = second.open(vthost::SshTarget {});
    REQUIRE(one);
    REQUIRE(two);
    CHECK(pause->openSessions() == 2);
    firstFor = 400;
    secondFor = 200;
    CHECK(pause->total() == 600ms); // the second did not hide the first
    one->reset();
    CHECK(pause->openSessions() == 1);
    secondFor = 250;
    CHECK(pause->total() == 650ms); // the first kept, the second still live
    two->reset();
    CHECK(pause->openSessions() == 0);
    CHECK(pause->total() == 650ms);
}

TEST_CASE("an attempt is extended only by the prompts asked since it began", "[vthost][attach]")
{
    auto openFor = std::atomic<std::chrono::milliseconds::rep> { 0 };
    auto const pause = std::make_shared<vthost::PromptPause>();
    auto broker = vthost::PausingAskpassBroker { std::make_shared<FakeBroker>(&openFor), pause };
    auto const budget = vthost::AttachBudget { 100ms }.pausedBy(pause);
    {
        // An earlier attempt on the same transport (a listing before the attach) asked for 5 s.
        auto session = broker.open(vthost::SshTarget {});
        REQUIRE(session);
        openFor = 5000;
    }
    CHECK(budget.paused() == 5000ms);
    auto const attempt = budget.startingNow();
    CHECK(attempt.paused() == 0ms);
    openFor = 300;
    auto session = broker.open(vthost::SshTarget {});
    REQUIRE(session);
    CHECK(attempt.paused() == 300ms);
}
