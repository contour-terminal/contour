// SPDX-License-Identifier: Apache-2.0
#include <contour/window/PromptHost.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include <QtTest/QSignalSpy>

using namespace contour::askpass;
using contour::window::PromptHost;
using contour::window::PromptKindRows;

namespace
{

struct Replied
{
    std::vector<AskpassReply> replies;

    [[nodiscard]] std::function<void(AskpassReply)> reply()
    {
        return [this](AskpassReply given) {
            replies.push_back(std::move(given));
        };
    }
};

} // namespace

TEST_CASE("the prompt kind rows are in the order of the kinds", "[askpass][prompthost]")
{
    for (auto const index: std::views::iota(std::size_t { 0 }, PromptKindRows.size()))
        CHECK(static_cast<std::size_t>(PromptKindRows[index].kind) == index);
    CHECK(PromptKindRows.size() == 8);
}

TEST_CASE("a window's host publishes the prompt it holds", "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = PromptHost { router, 1 };
    auto spy = QSignalSpy { &host, &PromptHost::pendingPromptChanged };
    CHECK(host.pendingPrompt().isEmpty());

    router->ask(1, 5, AskpassPromptKind::Secret, "ssh://me@host", "me@host's password: ", replied.reply());

    CHECK(spy.count() == 1);
    auto const prompt = host.pendingPrompt();
    CHECK(prompt.value("promptId").toInt() == 5);
    CHECK(prompt.value("isSecret").toBool());
    CHECK_FALSE(prompt.value("isVerbatim").toBool());
    CHECK(prompt.value("text").toString() == "me@host's password:");
    CHECK(prompt.value("heading").toString() == "Log in to ssh://me@host");
    CHECK(prompt.value("acceptText").toString() == "OK");
    CHECK(prompt.value("rejectText").toString() == "Cancel");
    // A password asks for nothing to be compared with.
    CHECK(prompt.value("hint").toString().isEmpty());
}

TEST_CASE("a daemon's fingerprint comes with what to compare it with", "[askpass][prompthost]")
{
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = PromptHost { router, 1 };
    router->ask(1, 7, AskpassPromptKind::PinConfirm, "studio:7297", "SHA256:abc", replied.reply());

    auto const prompt = host.pendingPrompt();
    CHECK(prompt.value("text").toString() == "SHA256:abc");
    CHECK(prompt.value("isVerbatim").toBool());
    CHECK(prompt.value("hint").toString() == "Compare with “contour daemon status” on that machine.");
}

TEST_CASE("an answer goes to the broker once, naming the prompt", "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = PromptHost { router, 1 };
    router->ask(1, 5, AskpassPromptKind::Secret, "ssh://me@host", "Password: ", replied.reply());

    // Another prompt's id answers nothing.
    host.answerPrompt(6, static_cast<int>(AskpassOutcome::Answered), "wrong");
    CHECK(replied.replies.empty());

    host.answerPrompt(5, static_cast<int>(AskpassOutcome::Answered), "hunter2");
    host.answerPrompt(5, static_cast<int>(AskpassOutcome::Answered), "doubled");
    REQUIRE(replied.replies.size() == 1);
    CHECK(replied.replies.front().outcome == AskpassOutcome::Answered);
    CHECK(replied.replies.front().answer == "hunter2");
    CHECK(host.pendingPrompt().isEmpty());
}

TEST_CASE("a cancelled prompt carries no answer", "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = PromptHost { router, 1 };
    router->ask(1, 5, AskpassPromptKind::Secret, "ssh://me@host", "Password: ", replied.reply());

    host.answerPrompt(5, static_cast<int>(AskpassOutcome::Cancelled), "typed anyway");

    REQUIRE(replied.replies.size() == 1);
    CHECK(replied.replies.front().outcome == AskpassOutcome::Cancelled);
    CHECK(replied.replies.front().answer.empty());
}

TEST_CASE("a question is shown before a notice, whatever came first", "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = PromptHost { router, 1 };
    router->ask(1, 1, AskpassPromptKind::Notice, "ssh://me@host", "Confirm user presence", replied.reply());
    CHECK(host.pendingPrompt().value("promptId").toInt() == 1);
    CHECK(host.pendingPrompt().value("acceptText").toString().isEmpty());

    router->ask(1, 2, AskpassPromptKind::Secret, "ssh://me@host", "PIN: ", replied.reply());
    CHECK(host.pendingPrompt().value("promptId").toInt() == 2);

    host.answerPrompt(2, static_cast<int>(AskpassOutcome::Answered), "1234");
    CHECK(host.pendingPrompt().value("promptId").toInt() == 1);
}

TEST_CASE("a host key is answered with the words ssh expects", "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = PromptHost { router, 1 };
    router->ask(
        1, 5, AskpassPromptKind::HostKey, "ssh://me@host", "Are you sure (yes/no)? ", replied.reply());

    auto const prompt = host.pendingPrompt();
    CHECK(prompt.value("isVerbatim").toBool());
    CHECK(prompt.value("acceptAnswer").toString() == "yes");
    CHECK(prompt.value("rejectAnswer").toString() == "no");
    CHECK(prompt.value("rejectOutcome").toInt() == static_cast<int>(AskpassOutcome::Answered));
}

TEST_CASE("two windows each get their own prompt", "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto left = PromptHost { router, 1 };
    auto right = PromptHost { router, 2 };

    router->ask(2, 10, AskpassPromptKind::Secret, "ssh://me@host", "Password: ", replied.reply());
    router->ask(1, 11, AskpassPromptKind::Secret, "ssh://me@host", "Password: ", replied.reply());

    CHECK(left.pendingPrompt().value("promptId").toInt() == 11);
    CHECK(right.pendingPrompt().value("promptId").toInt() == 10);
}

TEST_CASE("a window that closes cancels its prompts, and one that comes later gets the waiting ones",
          "[askpass][prompthost]")
{
    // Declared first, so it outlives the host, whose end cancels what is open.
    auto replied = Replied {};
    auto router = std::make_shared<AskpassPromptRouter>();
    router->ask(AnyWindow, 3, AskpassPromptKind::Secret, "ssh://me@host", "Password: ", replied.reply());

    auto host = std::optional<PromptHost> {};
    host.emplace(router, 1);
    CHECK(host->pendingPrompt().value("promptId").toInt() == 3);

    host.reset();
    REQUIRE(replied.replies.size() == 1);
    CHECK(replied.replies.front().outcome == AskpassOutcome::Cancelled);
}
