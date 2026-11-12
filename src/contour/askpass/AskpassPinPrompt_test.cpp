// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassPinPrompt.hpp>
#include <contour/remote/WorkspaceLister.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/WithTimeout.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QtTest/QTest>
#include <vthost/AttachBudget.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ControlClient.hpp>
#include <vthost/testing/CountingRandom.hpp>
#include <vthost/testing/TlsDaemon.hpp>

using namespace contour::askpass;

namespace
{

/// A window's sink that keeps what it was asked, so that the test answers (or does not) when it likes.
class KeepingSink final: public IAskpassPromptSink
{
  public:
    void ask(AskpassPromptId promptId,
             AskpassPromptKind kind,
             QString target,
             QString prompt,
             std::function<void(AskpassReply)> reply) override
    {
        asked = Asked { .id = promptId,
                        .kind = kind,
                        .target = std::move(target),
                        .text = std::move(prompt),
                        .reply = std::move(reply) };
        if (answersTrust)
            asked->reply(AskpassReply { .outcome = AskpassOutcome::Answered, .answer = {} });
    }
    void withdraw(AskpassPromptId promptId) override { withdrawn.push_back(promptId); }

    struct Asked
    {
        AskpassPromptId id = 0;
        AskpassPromptKind kind = AskpassPromptKind::Secret;
        QString target;
        QString text;
        std::function<void(AskpassReply)> reply;
    };
    bool answersTrust = false; ///< The user trusts at once, as the dialog is shown.
    std::optional<Asked> asked;
    std::vector<AskpassPromptId> withdrawn; ///< The prompts taken down again, in order.
};

/// Runs confirm() on a loop of its own thread, as a connection does, while this thread serves the GUI's.
class PinAsker
{
  public:
    PinAsker(AskpassPinPrompt& prompt, std::string hostPort, std::string fingerprint):
        _thread { [this, &prompt, hostPort = std::move(hostPort), fingerprint = std::move(fingerprint)] {
            auto const backend = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *backend };
            _decision = loop.blockOn(prompt.confirm(hostPort, fingerprint));
            _finished = true;
        } }
    {
    }
    ~PinAsker() { _thread.join(); }

    PinAsker(PinAsker const&) = delete;
    PinAsker& operator=(PinAsker const&) = delete;
    PinAsker(PinAsker&&) = delete;
    PinAsker& operator=(PinAsker&&) = delete;

    /// @return What was decided, once the GUI side has answered; waits for it, serving the GUI thread.
    [[nodiscard]] vthost::PinDecision decision()
    {
        REQUIRE(QTest::qWaitFor([this] { return _finished.load(); }, 5000));
        return _decision;
    }

  private:
    std::atomic<bool> _finished = false;
    vthost::PinDecision _decision = vthost::PinDecision::Trust; ///< Written before _finished is set.
    std::thread _thread;
};

} // namespace

TEST_CASE("the pin prompt asks the window and carries its answer back", "[askpass][pinprompt]")
{
    auto sink = KeepingSink {};
    auto context = QObject {};
    auto const router = std::make_shared<AskpassPromptRouter>();
    router->attach(1, sink);
    auto next = AskpassPromptId { 40 };
    auto prompt = AskpassPinPrompt { router, 1, context, [&next] { return ++next; } };

    SECTION("Trust")
    {
        auto asker = PinAsker { prompt, "studio.example:7297", "SHA256:abc" };
        REQUIRE(QTest::qWaitFor([&] { return sink.asked.has_value(); }, 5000));
        CHECK(sink.asked->id == 41);
        CHECK(sink.asked->kind == AskpassPromptKind::PinConfirm);
        CHECK(sink.asked->target == "studio.example:7297");
        CHECK(sink.asked->text.startsWith("SHA256:abc"));
        CHECK(sink.asked->text == "SHA256:abc");
        sink.asked->reply(AskpassReply { .outcome = AskpassOutcome::Answered, .answer = {} });
        CHECK(asker.decision() == vthost::PinDecision::Trust);
    }
    SECTION("Cancel declines")
    {
        auto asker = PinAsker { prompt, "studio.example:7297", "SHA256:abc" };
        REQUIRE(QTest::qWaitFor([&] { return sink.asked.has_value(); }, 5000));
        sink.asked->reply(AskpassReply { .outcome = AskpassOutcome::Cancelled, .answer = {} });
        CHECK(asker.decision() == vthost::PinDecision::Decline);
    }
    SECTION("the window going away declines")
    {
        auto asker = PinAsker { prompt, "studio.example:7297", "SHA256:abc" };
        REQUIRE(QTest::qWaitFor([&] { return sink.asked.has_value(); }, 5000));
        router->detach(1);
        CHECK(asker.decision() == vthost::PinDecision::Decline);
    }
    router->detach(1);
}

namespace
{

/// @return Whether @p transport connected.
core::async::Task<bool> connectedOnce(vthost::ITransportFactory* transport, core::net::EventLoop* loop)
{
    co_return (co_await transport->connect(*loop)).has_value();
}

/// Connects through @p transport under @p budget, as an attach does.
core::async::Task<void> attachUnder(vthost::ITransportFactory* transport,
                                    core::net::EventLoop* loop,
                                    vthost::AttachBudget budget,
                                    vthost::TransportReturnMark* mark,
                                    bool* attached)
{
    auto const connected =
        co_await vthost::withAttachBudget(loop, connectedOnce(transport, loop), budget, mark);
    *attached = connected.value_or(false);
}

/// Dials a daemon that is not pinned yet through a TLS transport whose first contact is asked in a window's
/// dialog, as an attach does, under a budget; on a thread of its own, as a connection's loop is. The daemon
/// accepts as many connections as the case expects to be made.
class BudgetedAttach
{
  public:
    BudgetedAttach(AskpassPinPrompt& prompt, vthost::AttachBudget const& budget, int accepts):
        _thread { [this, &prompt, budget, accepts] {
            auto const backend = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *backend };
            auto daemon = vthost::testing::TlsDaemon { loop };
            auto const files = core::platform::testing::InMemoryFileSystem {};
            auto random = vthost::testing::CountingRandom {};
            auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
            auto mark = vthost::TransportReturnMark {};
            auto factory = std::make_shared<vthost::TlsTransportFactory>(
                daemon.endpoint(),
                known,
                std::shared_ptr<vthost::IPinPrompt> { std::shared_ptr<void> {}, &prompt },
                std::function<std::shared_ptr<vthost::client::IClientKeySource>(core::net::EventLoop&)> {});
            auto observed = vthost::ConnectObservingFactory { factory, [&mark, &loop] {
                                                                 mark.note(loop.clock().now());
                                                             } };
            loop.blockOn(core::net::testing::allOf(daemon.serve(&loop, accepts),
                                                   attachUnder(&observed, &loop, budget, &mark, &_attached)));
            _pinned = known.pinOf(daemon.hostPort()).value().has_value();
            _finished = true;
        } }
    {
    }
    ~BudgetedAttach() { _thread.join(); }

    BudgetedAttach(BudgetedAttach const&) = delete;
    BudgetedAttach& operator=(BudgetedAttach const&) = delete;
    BudgetedAttach(BudgetedAttach&&) = delete;
    BudgetedAttach& operator=(BudgetedAttach&&) = delete;

    /// Waits for the attach to end, serving the GUI thread meanwhile.
    void wait()
    {
        REQUIRE(QTest::qWaitFor([this] { return _finished.load(); }, 10000));
    }
    [[nodiscard]] bool attached() const noexcept { return _attached; }
    [[nodiscard]] bool pinned() const noexcept { return _pinned; }

  private:
    std::atomic<bool> _finished = false;
    bool _attached = false; ///< Written before _finished is set.
    bool _pinned = false;   ///< Written before _finished is set.
    std::thread _thread;
};

} // namespace

TEST_CASE("an attach whose first contact waits for the user's answer is not timed out by it",
          "[askpass][pinprompt]")
{
    using namespace std::chrono_literals;
    auto sink = KeepingSink {};
    auto context = QObject {};
    auto const router = std::make_shared<AskpassPromptRouter>();
    router->attach(1, sink);
    auto next = std::atomic<AskpassPromptId> { 70 };
    auto prompt = AskpassPinPrompt { router, 1, context, [&next] { return ++next; } };

    SECTION("with the prompts-open budget the slow answer still attaches, and the daemon is pinned")
    {
        auto attach = BudgetedAttach { prompt, vthost::AttachBudget::promptsOpen(300ms), 2 };
        REQUIRE(QTest::qWaitFor([&] { return sink.asked.has_value(); }, 5000));
        // Longer than the whole 300 ms an ordinary budget gives the attach.
        QTest::qWait(700);
        sink.asked->reply(AskpassReply { .outcome = AskpassOutcome::Answered, .answer = {} });
        attach.wait();
        CHECK(attach.attached());
        CHECK(attach.pinned());
        CHECK(sink.withdrawn.empty());
    }
    SECTION("with a plain budget the same answer is too late, and its dialog is taken down")
    {
        auto attach = BudgetedAttach { prompt, vthost::AttachBudget { 300ms }, 1 };
        REQUIRE(QTest::qWaitFor([&] { return sink.asked.has_value(); }, 5000));
        attach.wait();
        CHECK_FALSE(attach.attached());
        CHECK_FALSE(attach.pinned());
        // The attach gave up: nobody waits for the answer any more, so the question is withdrawn.
        REQUIRE(QTest::qWaitFor([&] { return !sink.withdrawn.empty(); }, 5000));
        CHECK(sink.withdrawn.front() == sink.asked->id);
    }
    router->detach(1);
}

TEST_CASE("a workspace listing made while the GUI thread serves events reaches the pin dialog",
          "[askpass][pinprompt]")
{
    // What `contour client --host H --workspace W` does once the GUI is up: it lists the daemon's workspaces
    // off the GUI thread, which goes on serving the dialog's events -- in its own event loop, never a nested
    // one.
    auto sink = KeepingSink {};
    sink.answersTrust = true;
    auto context = QObject {};
    auto const router = std::make_shared<AskpassPromptRouter>();
    router->attach(1, sink);
    auto next = std::atomic<AskpassPromptId> { 90 };
    auto prompt = std::make_shared<AskpassPinPrompt>(router, 1, context, [&next] { return ++next; });
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto random = vthost::testing::CountingRandom {};
    auto known = vthost::KnownDaemons { files, "/state/known_daemons", random };
    auto const served = vthost::testing::TlsDaemonThread { 2 };
    auto const factory =
        std::make_shared<vthost::TlsTransportFactory>(served.daemon().endpoint, known, prompt, nullptr);

    // The daemon speaks no control API: what matters is that the question was asked and the daemon pinned.
    auto const lister = contour::remote::makeControlWorkspaceLister(std::chrono::seconds { 1 });
    auto reply = std::optional<contour::remote::WorkspaceListing> {};
    lister->list(contour::remote::ListTarget { .address = {}, .transport = factory },
                 [&reply](contour::remote::WorkspaceListing listed) { reply = std::move(listed); });
    REQUIRE(QTest::qWaitFor([&reply] { return reply.has_value(); }, 15000));

    REQUIRE(sink.asked.has_value());
    CHECK(sink.asked->kind == AskpassPromptKind::PinConfirm);
    CHECK(sink.asked->target == QString::fromStdString(served.daemon().hostPort));
    CHECK(known.pinOf(served.daemon().hostPort) == served.daemon().fingerprint);
    CHECK_FALSE(reply->has_value()); // nothing there answers a request
    router->detach(1);
}
