// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/config/Actions.hpp>
#include <contour/remote/StartupAttach.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/PromptHost.hpp>

#include <core/Utils.hpp>
#include <core/log/LogSink.hpp>

#include <QtCore/QCoreApplication>
#include <QtGui/QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QtTest/QTest>

using namespace std::chrono_literals;
using contour::remote::DuringStartup;
using contour::remote::StartupAttach;
using contour::remote::StartupAttachSteps;

namespace
{

/// A scripted attach and what happens after it.
struct Script
{
    std::optional<std::expected<void, std::string>> outcome; ///< What the next look sees; none: going on.
    std::optional<std::chrono::milliseconds> deadline;
    int looks = 0;
    int firstWindows = 0;
    std::expected<void, std::string> openResult {};
    std::vector<std::string> ended;
    std::vector<int> exitCodes;

    [[nodiscard]] StartupAttachSteps steps()
    {
        return StartupAttachSteps {
            .look =
                [this] {
                    ++looks;
                    return outcome;
                },
            .untilDeadline = [this] { return deadline; },
            .openFirstWindow =
                [this](std::expected<void, std::string> attached) {
                    if (!attached)
                        return std::expected<void, std::string> { std::unexpected(attached.error()) };
                    ++firstWindows;
                    return openResult;
                },
            .end =
                [this](std::string const& reason, int code) {
                    ended.push_back(reason);
                    exitCodes.push_back(code);
                },
        };
    }
};

} // namespace

TEST_CASE("the launch's first window opens once its attach settled", "[contour][startup]")
{
    auto script = Script {};
    auto wait = StartupAttach { script.steps() };
    wait.start();
    CHECK(wait.isWaiting());

    // Going on: looked at, and nothing opens.
    QTest::qWait(20);
    CHECK(script.looks == 1);
    CHECK(script.firstWindows == 0);
    CHECK(wait.isWaiting());

    script.outcome = std::expected<void, std::string> {};
    wait.progressed();
    CHECK(script.firstWindows == 1);
    CHECK_FALSE(wait.isWaiting());
    CHECK(script.ended.empty());

    // Told of progress after it ended: nothing happens twice.
    wait.progressed();
    CHECK(script.firstWindows == 1);
}

TEST_CASE("a launch whose attach failed ends with the reason and status 1", "[contour][startup]")
{
    auto script = Script {};
    script.outcome = std::unexpected(std::string { "The ssh login to ssh://me@host was cancelled." });
    auto wait = StartupAttach { script.steps() };
    wait.start();
    REQUIRE(QTest::qWaitFor([&] { return !script.ended.empty(); }, 2000));

    CHECK(script.ended == std::vector<std::string> { "The ssh login to ssh://me@host was cancelled." });
    CHECK(script.exitCodes == std::vector<int> { 1 });
    CHECK(script.firstWindows == 0);
}

TEST_CASE("a launch that cannot open its first window ends the same way", "[contour][startup]")
{
    auto script = Script {};
    script.outcome = std::expected<void, std::string> {};
    script.openResult = std::unexpected(std::string { "no window" });
    auto wait = StartupAttach { script.steps() };
    wait.start();
    REQUIRE(QTest::qWaitFor([&] { return !script.ended.empty(); }, 2000));
    CHECK(script.exitCodes == std::vector<int> { 1 });
}

TEST_CASE("the attach is looked at again when its time is up, without anyone saying so", "[contour][startup]")
{
    auto script = Script {};
    script.deadline = 30ms;
    auto wait = StartupAttach { script.steps() };
    wait.start();
    REQUIRE(QTest::qWaitFor([&] { return script.looks >= 3; }, 2000));

    script.outcome = std::unexpected(std::string { "timed out" });
    REQUIRE(QTest::qWaitFor([&] { return !script.ended.empty(); }, 2000));
    CHECK(script.ended == std::vector<std::string> { "timed out" });
}

TEST_CASE("an attach that must be prepared is looked at only once it has started", "[contour][startup]")
{
    auto script = Script {};
    auto ready = contour::remote::StartupAttachReady {};
    auto steps = script.steps();
    steps.prepare = [&ready](contour::remote::StartupAttachReady told) {
        ready = std::move(told);
    };
    auto wait = StartupAttach { std::move(steps) };
    wait.start();
    REQUIRE(ready);
    CHECK(wait.isWaiting());

    // News of an attach that does not exist yet is nothing to look at.
    wait.progressed();
    QTest::qWait(20);
    CHECK(script.looks == 0);
    CHECK(wait.isWaiting());

    script.outcome = std::expected<void, std::string> {};
    ready({});
    CHECK(script.looks == 1);
    CHECK(script.firstWindows == 1);
    CHECK_FALSE(wait.isWaiting());
}

TEST_CASE("an attach that cannot be prepared ends the launch with the reason and status 1",
          "[contour][startup]")
{
    auto script = Script {};
    auto steps = script.steps();
    steps.prepare = [](contour::remote::StartupAttachReady const& ready) {
        ready(std::unexpected(std::string { "contour client: --workspace 'w': no such workspace" }));
    };
    auto wait = StartupAttach { std::move(steps) };
    wait.start();
    CHECK(script.ended == std::vector<std::string> { "contour client: --workspace 'w': no such workspace" });
    CHECK(script.exitCodes == std::vector<int> { 1 });
    CHECK(script.looks == 0);
    CHECK(script.firstWindows == 0);
    CHECK_FALSE(wait.isWaiting());
}

TEST_CASE("only Quit runs while the launch waits for its attach", "[contour][startup]")
{
    namespace actions = contour::actions;
    CHECK(contour::remote::duringStartup(actions::Action { actions::Quit {} }) == DuringStartup::Allowed);
    CHECK(contour::remote::duringStartup(actions::Action { actions::NewTerminal {} })
          == DuringStartup::Refused);
    CHECK(contour::remote::duringStartup(actions::Action { actions::NewWindowTo {} })
          == DuringStartup::Refused);
    CHECK(contour::remote::duringStartup(actions::Action { actions::OpenConfiguration {} })
          == DuringStartup::Refused);
}

TEST_CASE("ContourGuiApp: a check's transport never prompts, a launch's does", "[contour][app][askpass]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto const host =
        contour::config::HostEntry { .url = contour::config::HostUrl { .scheme =
                                                                           contour::config::HostScheme::Ssh,
                                                                       .user = "me",
                                                                       .host = "homepc",
                                                                       .port = std::nullopt },
                                     .tokenFile = {},
                                     .tlsCa = {},
                                     .remoteCommand = {},
                                     .profile = {} };
    using Policy = contour::cli::SshPrompts;
    auto const check = app.app().transportOf("home", host, contour::askpass::AnyWindow, Policy::Batch);
    auto const attach = app.app().transportOf("home", host, contour::askpass::AnyWindow, Policy::Askpass);
    REQUIRE(check.has_value());
    REQUIRE(attach.has_value());
    // The check runs ssh in batch mode, so it opens no askpass session and the user answers once, at the
    // attach. The transport's budget follows from that choice (the factory is built with a broker or not),
    // and says it: a batch ssh has an outer clock for the connect, one that answers prompts has none, as
    // its own deadline pauses while a prompt is open. The spawn environments themselves are pinned in
    // SshTransport_test ("ssh spawns in batch mode exactly when it has no broker").
    CHECK(check->transport.attach.connect.has_value());
    CHECK_FALSE(attach->transport.attach.connect.has_value());
}

TEST_CASE("ContourGuiApp: while the launch waits for its attach a new window is refused, and Quit ends it "
          "with status 0 and cancels the prompt",
          "[contour][app][startup]")
{
    namespace actions = contour::actions;
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto script = Script {};
    // waitForAttach() turns it off until the first window; the rest of the suite expects the default, even
    // when a REQUIRE below fails.
    auto const restoreQuit = core::Finally { [] { QGuiApplication::setQuitOnLastWindowClosed(true); } };
    app.app().waitForAttach(script.steps());
    REQUIRE(app.app().isStartingUp());

    // A prompt waits in a window that has a sink.
    auto replies = std::vector<contour::askpass::AskpassReply> {};
    struct Sink final: contour::askpass::IAskpassPromptSink
    {
        void ask(contour::askpass::AskpassPromptId,
                 contour::askpass::AskpassPromptKind,
                 QString,
                 QString,
                 std::function<void(contour::askpass::AskpassReply)>) override
        {
        }
        void withdraw(contour::askpass::AskpassPromptId) override {}
    } sink;
    app.app().askpassRouter()->attach(1, sink);
    app.app().askpassRouter()->ask(
        1,
        1,
        contour::askpass::AskpassPromptKind::Secret,
        QStringLiteral("ssh://me@host"),
        QStringLiteral("Password: "),
        [&replies](contour::askpass::AskpassReply reply) { replies.push_back(std::move(reply)); });

    // New Terminal and the Connect picker do nothing: no window appears, the prompt stays.
    app.app().runMenuBarAction(actions::Action { actions::NewTerminal {} },
                               contour::command::ActionScope::Application);
    app.app().runMenuBarAction(actions::Action { actions::NewWindowTo {} },
                               contour::command::ActionScope::Application);
    CHECK(replies.empty());
    CHECK(app.app().isStartingUp());

    // Quit is the one action that runs: it ends the event loop with status 0 (recorded, not performed) and
    // the prompt that is open goes with it: ssh's helper is told no.
    app.app().runMenuBarAction(actions::Action { actions::Quit {} },
                               contour::command::ActionScope::Application);
    CHECK(app.exitCodes() == std::vector<int> { 0 });
    REQUIRE(replies.size() == 1);
    CHECK(replies.front().outcome == contour::askpass::AskpassOutcome::Cancelled);
    app.app().askpassRouter()->detach(1);
}

TEST_CASE(
    "ContourGuiApp: while the launch lists its workspace and asks about the daemon, no window starts, and "
    "one opens once it attached",
    "[contour][app][startup]")
{
    // `contour client --host H --workspace W`: the listing, and the pin dialog it asks, come before the
    // attach and are part of the wait: the startup gate holds throughout, in the app's own event loop.
    namespace actions = contour::actions;
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto script = Script {};
    auto ready = contour::remote::StartupAttachReady {};
    auto steps = script.steps();
    steps.prepare = [&ready](contour::remote::StartupAttachReady told) {
        ready = std::move(told);
    };
    auto const restoreQuit = core::Finally { [] { QGuiApplication::setQuitOnLastWindowClosed(true); } };
    app.app().waitForAttach(std::move(steps));
    REQUIRE(app.app().isStartingUp()); // the quick terminal's hotkey is ignored while this holds
    REQUIRE(ready);

    // The listing asks about the daemon, in the stand-in window.
    auto replies = std::vector<contour::askpass::AskpassReply> {};
    app.app().askpassRouter()->ask(
        contour::askpass::AnyWindow,
        7,
        contour::askpass::AskpassPromptKind::PinConfirm,
        QStringLiteral("studio.example:7297"),
        QStringLiteral("SHA256:47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU"),
        [&replies](contour::askpass::AskpassReply reply) { replies.push_back(std::move(reply)); });
    REQUIRE(app.bootstrapPromptHosts().size() == 1);

    // New Terminal, the Connect picker and Settings are refused while it waits.
    {
        auto const capture = core::log::ScopedCapture { "gui.attach" };
        for (auto const& action: { actions::Action { actions::NewTerminal {} },
                                   actions::Action { actions::NewWindowTo {} },
                                   actions::Action { actions::OpenConfiguration {} } })
            app.app().runMenuBarAction(action, contour::command::ActionScope::Application);
        CHECK(capture.count("ignored: the launch still waits for its attach") == 3);
    }
    CHECK(app.app().isStartingUp());
    CHECK(script.firstWindows == 0);
    CHECK(script.looks == 0);

    // The user trusts the daemon; the listing answers and the attach starts, and is still going on.
    auto const host = app.bootstrapPromptHosts().front();
    REQUIRE(host);
    host->answerPrompt(7, static_cast<int>(contour::askpass::AskpassOutcome::Answered), QString {});
    REQUIRE(replies.size() == 1);
    script.deadline = 10ms; // looked at again on its own, as nothing here tells of progress
    ready({});
    CHECK(app.app().isStartingUp());
    CHECK(script.firstWindows == 0);

    // It attached: exactly one first window.
    script.outcome = std::expected<void, std::string> {};
    REQUIRE(QTest::qWaitFor([&] { return script.firstWindows > 0; }, 2000));
    QTest::qWait(20);
    CHECK(script.firstWindows == 1);
    CHECK_FALSE(app.app().isStartingUp());
}

TEST_CASE("ContourGuiApp: the stand-in prompt window ends with its last answer, and the next prompt gets a "
          "fresh one (offscreen)",
          "[contour][app][askpass][bootstrap]")
{
    auto app = contour::test::TestApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto replies = std::vector<contour::askpass::AskpassReply> {};
    auto const ask = [&](contour::askpass::AskpassPromptId id) {
        app.app().askpassRouter()->ask(
            contour::askpass::AnyWindow,
            id,
            contour::askpass::AskpassPromptKind::Secret,
            QStringLiteral("ssh://me@host"),
            QStringLiteral("Password: "),
            [&replies](contour::askpass::AskpassReply reply) { replies.push_back(std::move(reply)); });
    };

    ask(1);
    REQUIRE(app.bootstrapPromptHosts().size() == 1);
    auto const first = app.bootstrapPromptHosts().front();
    REQUIRE(first);
    CHECK_FALSE(first->pendingPrompt().isEmpty());

    // Answered: the stand-in goes, from the event loop, rather than staying as a hidden window.
    first->answerPrompt(
        1, static_cast<int>(contour::askpass::AskpassOutcome::Answered), QStringLiteral("pw"));
    REQUIRE(replies.size() == 1);
    REQUIRE(QTest::qWaitFor([&first] { return first.isNull(); }, 2000));

    // The next prompt is shown by a stand-in of its own.
    ask(2);
    REQUIRE(app.bootstrapPromptHosts().size() == 2);
    auto const second = app.bootstrapPromptHosts().back();
    REQUIRE(second);
    CHECK_FALSE(second->pendingPrompt().isEmpty());
    second->answerPrompt(2, static_cast<int>(contour::askpass::AskpassOutcome::Cancelled), QString {});
    REQUIRE(replies.size() == 2);
    REQUIRE(QTest::qWaitFor([&second] { return second.isNull(); }, 2000));
}
