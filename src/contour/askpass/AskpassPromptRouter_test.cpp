// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassPromptRouter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace contour::askpass;

namespace
{

/// A sink that keeps what it is asked, and answers by hand.
class RecordingSink final: public IAskpassPromptSink
{
  public:
    struct Ask
    {
        AskpassPromptId id;
        AskpassPromptKind kind;
        QString prompt;
        std::function<void(AskpassReply)> reply;
    };

    void ask(AskpassPromptId promptId,
             AskpassPromptKind kind,
             QString /*target*/,
             QString prompt,
             std::function<void(AskpassReply)> reply) override
    {
        asks.push_back(Ask { promptId, kind, std::move(prompt), std::move(reply) });
    }

    void withdraw(AskpassPromptId promptId) override { withdrawn.push_back(promptId); }

    std::vector<Ask> asks;
    std::vector<AskpassPromptId> withdrawn;
};

/// What the broker's side is told.
struct Replies
{
    std::vector<std::string> told;

    [[nodiscard]] std::function<void(AskpassReply)> reply(std::string name)
    {
        return [this, name = std::move(name)](AskpassReply given) {
            told.push_back(name
                           + (given.outcome == AskpassOutcome::Answered
                                  ? ":" + std::string { given.answer.view() }
                                  : ":cancelled"));
        };
    }
};

void ask(AskpassPromptRouter& router, AskpassRouteId route, AskpassPromptId id, Replies& replies)
{
    router.ask(route,
               id,
               AskpassPromptKind::Secret,
               "ssh://me@host",
               "Password: ",
               replies.reply(std::to_string(id)));
}

} // namespace

TEST_CASE("a prompt goes to the window that started its attach", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto first = RecordingSink {};
    auto second = RecordingSink {};
    router.attach(1, first);
    router.attach(2, second);
    auto replies = Replies {};

    ask(router, 2, 10, replies);
    ask(router, 1, 11, replies);

    REQUIRE(first.asks.size() == 1);
    REQUIRE(second.asks.size() == 1);
    CHECK(first.asks.front().id == 11);
    CHECK(second.asks.front().id == 10);
}

TEST_CASE("a prompt of no window, or of a window that is gone, goes to the first window", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto first = RecordingSink {};
    auto second = RecordingSink {};
    router.attach(1, first);
    router.attach(2, second);
    auto replies = Replies {};

    ask(router, AnyWindow, 10, replies);
    ask(router, 99, 11, replies);

    CHECK(first.asks.size() == 2);
    CHECK(second.asks.empty());
}

TEST_CASE("a prompt waits until a window can show it", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto replies = Replies {};
    ask(router, AnyWindow, 10, replies);
    ask(router, 7, 11, replies);
    CHECK(router.waitingCount() == 2);

    auto window = RecordingSink {};
    router.attach(3, window);

    CHECK(router.waitingCount() == 0);
    REQUIRE(window.asks.size() == 2);
    CHECK(window.asks[0].id == 10);
    CHECK(window.asks[1].id == 11);
}

TEST_CASE("an answer reaches the broker once, and a late one is dropped", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto window = RecordingSink {};
    router.attach(1, window);
    auto replies = Replies {};
    ask(router, 1, 10, replies);

    window.asks.front().reply(answeredReply("secret"));
    window.asks.front().reply(answeredReply("again"));
    CHECK(replies.told == std::vector<std::string> { "10:secret" });

    ask(router, 1, 11, replies);
    router.withdraw(11);
    CHECK(window.withdrawn == std::vector<AskpassPromptId> { 11 });
    window.asks.back().reply(answeredReply("late"));
    CHECK(replies.told.size() == 1);
}

TEST_CASE("withdrawing a prompt that waits drops it without asking anyone", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto replies = Replies {};
    ask(router, AnyWindow, 10, replies);
    router.withdraw(10);
    CHECK(router.waitingCount() == 0);

    auto window = RecordingSink {};
    router.attach(1, window);
    CHECK(window.asks.empty());
    CHECK(replies.told.empty());
}

TEST_CASE("a window that goes cancels what it was asked", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto kept = RecordingSink {};
    auto closing = RecordingSink {};
    router.attach(1, kept);
    router.attach(2, closing);
    auto replies = Replies {};
    ask(router, 1, 10, replies);
    ask(router, 2, 11, replies);

    router.detach(2);

    CHECK(replies.told == std::vector<std::string> { "11:cancelled" });
    // A reply from the closed window's sink afterwards is dropped.
    closing.asks.front().reply(answeredReply("late"));
    CHECK(replies.told.size() == 1);
}

TEST_CASE("after shutdown nothing is asked, and open prompts are cancelled", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto window = RecordingSink {};
    router.attach(1, window);
    auto replies = Replies {};
    ask(router, 1, 10, replies);

    router.shutdown();
    CHECK(replies.told == std::vector<std::string> { "10:cancelled" });

    ask(router, 1, 11, replies);
    CHECK(replies.told == std::vector<std::string> { "10:cancelled", "11:cancelled" });
    CHECK(window.asks.size() == 1);
    auto later = RecordingSink {};
    router.attach(5, later);
    ask(router, 5, 12, replies);
    CHECK(later.asks.empty());
}

TEST_CASE("the router says when a prompt has no window to wait in, and the stand-in is the last resort",
          "[askpass][router]")
{
    auto waited = 0;
    auto router = AskpassPromptRouter { [&waited] { ++waited; } };
    auto replies = Replies {};

    ask(router, AnyWindow, 10, replies);
    CHECK(waited == 1);

    // The stand-in window takes what waits.
    auto standIn = RecordingSink {};
    router.attach(BootstrapRoute, standIn);
    REQUIRE(standIn.asks.size() == 1);
    ask(router, AnyWindow, 11, replies);
    CHECK(standIn.asks.size() == 2);
    CHECK(waited == 1);

    // A real window comes: what has no window of its own goes there now, the stand-in keeps what it has.
    auto window = RecordingSink {};
    router.attach(4, window);
    ask(router, AnyWindow, 12, replies);
    ask(router, 4, 13, replies);
    ask(router, BootstrapRoute, 14, replies);
    CHECK(standIn.asks.size() == 3); // its own route, asked for by name
    REQUIRE(window.asks.size() == 2);
    CHECK(window.asks[0].id == 12);
    CHECK(window.asks[1].id == 13);
}

TEST_CASE("a prompt is open from when it is asked until it ends, whoever shows it", "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto replies = Replies {};
    CHECK_FALSE(router.hasOpenPrompt());

    // Waiting for a window counts.
    ask(router, AnyWindow, 10, replies);
    CHECK(router.hasOpenPrompt());
    router.withdraw(10);
    CHECK_FALSE(router.hasOpenPrompt());

    // So does being shown, until it is answered.
    auto window = RecordingSink {};
    router.attach(1, window);
    ask(router, 1, 11, replies);
    CHECK(router.hasOpenPrompt());
    window.asks.front().reply(AskpassReply { AskpassOutcome::Cancelled, {} });
    CHECK_FALSE(router.hasOpenPrompt());
}

TEST_CASE("a prompt for the route of a closed window is cancelled, never shown in another",
          "[askpass][router]")
{
    auto router = AskpassPromptRouter {};
    auto kept = RecordingSink {};
    auto closing = RecordingSink {};
    router.attach(1, kept);
    router.attach(2, closing);
    router.detach(2);
    auto replies = Replies {};

    // The window the attach was started from is gone: its later prompts are no's.
    ask(router, 2, 10, replies);
    CHECK(replies.told == std::vector<std::string> { "10:cancelled" });
    CHECK(kept.asks.empty());
    CHECK(router.waitingCount() == 0);

    // A window that never existed, or no window at all, still goes to the first one.
    ask(router, 99, 11, replies);
    ask(router, AnyWindow, 12, replies);
    CHECK(kept.asks.size() == 2);
}

TEST_CASE("a prompt for a window that is not on screen is held until it is", "[askpass][router][exposure]")
{
    auto held = 0;
    auto router = AskpassPromptRouter { {}, {}, [&held] { ++held; } };
    auto first = RecordingSink {};
    auto second = RecordingSink {};
    router.attach(1, first);
    router.attach(2, second);
    auto replies = Replies {};
    using State = SinkExposure::State;
    router.updateExposure(
        { { .route = 1, .state = State::Hidden }, { .route = 2, .state = State::OnScreen } });

    ask(router, 1, 10, replies);
    ask(router, 2, 11, replies);
    // Its own window is on screen: shown at once. The other window's prompt waits, though a window is on
    // screen.
    CHECK(second.asks.size() == 1);
    CHECK(first.asks.empty());
    CHECK(router.isHolding(1));
    CHECK_FALSE(router.isHolding(2));
    CHECK(router.waitingCount() == 1);
    CHECK(held >= 1);

    // No window's own: the first window shows it, and that one is hidden.
    ask(router, AnyWindow, 12, replies);
    CHECK(router.isHolding(AnyWindow));

    // Shown once the window is on screen.
    router.updateExposure(
        { { .route = 1, .state = State::OnScreen }, { .route = 2, .state = State::OnScreen } });
    CHECK(first.asks.size() == 2);
    CHECK(router.waitingCount() == 0);
    CHECK_FALSE(router.isHolding(1));
}

TEST_CASE("a window that is closed while its prompt is held cancels it", "[askpass][router][exposure]")
{
    auto router = AskpassPromptRouter {};
    auto window = RecordingSink {};
    router.attach(1, window);
    auto replies = Replies {};
    router.updateExposure({ { .route = 1, .state = SinkExposure::State::Hidden } });
    ask(router, 1, 10, replies);
    REQUIRE(router.isHolding(1));

    router.detach(1);
    CHECK(replies.told == std::vector<std::string> { "10:cancelled" });
    CHECK_FALSE(router.isHolding(1));
    CHECK(window.asks.empty());
}

TEST_CASE("only a prompt with no window at all brings up the stand-in", "[askpass][router][exposure]")
{
    auto waited = 0;
    auto router = AskpassPromptRouter { [&waited] { ++waited; } };
    auto window = RecordingSink {};
    router.attach(1, window);
    router.updateExposure({ { .route = 1, .state = SinkExposure::State::Hidden } });
    auto replies = Replies {};
    ask(router, 1, 10, replies);
    CHECK(waited == 0);
    router.detach(1);
    ask(router, AnyWindow, 11, replies);
    CHECK(waited == 1);
}
