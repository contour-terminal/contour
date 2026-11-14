// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassBroker.hpp>
#include <contour/askpass/AskpassClient.hpp>
#include <contour/platform/ApplicationReadiness.hpp>
#include <contour/remote/RemoteController.hpp>
#include <contour/window/PromptHost.hpp>

#include <core/Random.hpp>
#include <core/async/StopToken.hpp>
#include <core/async/Task.hpp>
#include <core/async/WhenAll.hpp>
#include <core/async/WhenAny.hpp>
#include <core/log/LogSink.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/InterruptibleSleep.hpp>
#include <core/net/IoBackend.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtNetwork/QLocalSocket>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <vthost/SshTransport.hpp>
#include <vthost/testing/CountingRandom.hpp>

#ifndef _WIN32

using namespace std::chrono_literals;
using namespace contour::askpass;

namespace contour::askpass
{
/// What the tests may see of, and do to, an AskpassBroker.
struct AskpassBrokerTestAccess
{
    /// @return Whether @p broker's thread runs, serving sessions.
    [[nodiscard]] static bool isServing(AskpassBroker const& broker) noexcept
    {
        return broker._state.load() == AskpassBroker::State::Serving;
    }

    /// @return How many coroutines wait in @p broker's whenServing().
    [[nodiscard]] static std::size_t waiting(AskpassBroker const& broker)
    {
        return contour::platform::AsyncLatchTestAccess::waiting(broker._outcome);
    }

    /// Spins until @p count coroutines wait in @p broker's whenServing(): the wait is under way.
    static void awaitWaiters(AskpassBroker const& broker, std::size_t count)
    {
        while (waiting(broker) != count)
            std::this_thread::yield();
    }

    /// Has @p hook run just before @p broker starts its thread, under its life lock.
    static void beforeThreadStart(AskpassBroker& broker, std::function<void()> hook)
    {
        auto const lock = std::scoped_lock { broker._lifeMutex };
        broker._beforeThreadStart = std::move(hook);
    }
};
} // namespace contour::askpass

namespace
{
/// A sink that records what it is asked, and answers by a rule given up front or not at all.
class FakeSink final: public IAskpassPromptSink
{
  public:
    struct Ask
    {
        AskpassPromptId promptId;
        AskpassPromptKind kind;
        QString target;
        QString prompt;
    };

    /// Told what to answer for each ask; empty keeps every prompt open.
    std::function<std::optional<AskpassReply>(Ask const&)> answer;
    std::vector<Ask> asks;
    std::vector<AskpassPromptId> withdrawn; ///< The prompts whose helper went away.
    std::function<void(AskpassReply)> held; ///< The reply of the latest prompt that was left open.

    void ask(AskpassPromptId promptId,
             AskpassPromptKind kind,
             QString target,
             QString prompt,
             std::function<void(AskpassReply)> reply) override
    {
        asks.push_back(Ask { promptId, kind, std::move(target), std::move(prompt) });
        if (auto given = answer ? answer(asks.back()) : std::nullopt)
            reply(std::move(*given));
        else
            held = std::move(reply);
    }

    void withdraw(AskpassPromptId promptId) override { withdrawn.push_back(promptId); }
};

struct HelperRun
{
    std::optional<int> exitCode;
    std::string out;
};

[[nodiscard]] std::map<std::string, std::string, std::less<>> variablesOf(
    vthost::IAskpassSession const& session)
{
    auto variables = std::map<std::string, std::string, std::less<>> {};
    for (auto const& [name, value]: session.environment().set)
        variables.insert_or_assign(name, value);
    return variables;
}

/// Starts the helper as ssh would, on a thread of its own.
[[nodiscard]] std::future<HelperRun> startHelper(std::map<std::string, std::string, std::less<>> variables,
                                                 std::string prompt = "Password: ")
{
    return std::async(std::launch::async, [variables = std::move(variables), prompt = std::move(prompt)] {
        auto const environment = core::testing::FakeEnvironment { variables };
        auto const argv = std::array<char const*, 2> { "contour", prompt.c_str() };
        auto out = std::ostringstream {};
        auto err = std::ostringstream {};
        auto const code = runAskpassIfRequested(environment, argv, out, err);
        return HelperRun { .exitCode = code, .out = out.str() };
    });
}

/// Runs the helper to its end while this thread serves the GUI's events, which the helper's prompt needs.
[[nodiscard]] HelperRun runHelper(std::map<std::string, std::string, std::less<>> variables,
                                  std::string prompt = "Password: ")
{
    auto done = startHelper(std::move(variables), std::move(prompt));
    while (done.wait_for(0ms) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done.get();
}

/// What the helper of a cancelled prompt does: it stays blocked, no answer written, for as long as the
/// session lives -- ssh must be ended first, or it would be handed an empty answer -- and exits 1 only once
/// the session is destroyed.
/// @param session The cancelled attempt's session; destroyed here.
/// @param helpers The helpers that asked, each still blocked.
/// @return What each helper did once the session was gone.
[[nodiscard]] std::vector<HelperRun> endCancelled(std::unique_ptr<vthost::IAskpassSession>& session,
                                                  std::vector<std::future<HelperRun>> helpers)
{
    auto const settle = std::chrono::steady_clock::now() + 300ms;
    while (std::chrono::steady_clock::now() < settle)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    for (auto& helper: helpers)
        CHECK(helper.wait_for(0ms) != std::future_status::ready);
    session.reset();
    auto runs = std::vector<HelperRun> {};
    for (auto& helper: helpers)
    {
        while (helper.wait_for(0ms) != std::future_status::ready)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        runs.push_back(helper.get());
    }
    return runs;
}

/// Serves events until @p until holds, or fails after a few seconds.
template <typename Until>
void pumpUntil(Until const& until)
{
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!until() && std::chrono::steady_clock::now() < deadline)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    REQUIRE(until());
}

[[nodiscard]] vthost::SshTarget targetOf()
{
    return vthost::SshTarget { .user = "me", .host = "host", .port = std::nullopt };
}

/// What a broker made where the application object exists waits for: nothing.
[[nodiscard]] std::shared_ptr<contour::platform::ApplicationReadiness> readyApplication()
{
    return std::make_shared<contour::platform::ApplicationReadiness>(
        contour::platform::ApplicationReadiness::State::Ready);
}

/// Runs a broker's whenServing() on an event loop of its own thread, as an ssh attempt does, so that this
/// thread can settle what it waits for.
class ServingWait
{
  public:
    explicit ServingWait(AskpassBroker& broker):
        _thread { [this, &broker] {
            auto const backend = core::net::makeDefaultBackend();
            auto loop = core::net::EventLoop { *backend };
            loop.blockOn(run(&loop, &broker, &_result, _stop.get_token()));
        } }
    {
    }
    ~ServingWait()
    {
        if (_thread.joinable())
            _thread.join();
    }
    ServingWait(ServingWait const&) = delete;
    ServingWait& operator=(ServingWait const&) = delete;
    ServingWait(ServingWait&&) = delete;
    ServingWait& operator=(ServingWait&&) = delete;

    /// Stops the waiting coroutine, as a link's stop() does.
    void stop() { _stop.request_stop(); }

    /// @return What whenServing() said; nothing if it was stopped first.
    [[nodiscard]] std::optional<std::expected<void, std::string>> result()
    {
        _thread.join();
        return _result;
    }

  private:
    static core::async::Task<void> serve(AskpassBroker* broker,
                                         std::optional<std::expected<void, std::string>>* result)
    {
        *result = co_await broker->whenServing();
    }

    static core::async::Task<void> untilStopped(core::net::EventLoop* loop, core::async::StopToken token)
    {
        std::ignore = co_await core::net::interruptibleSleepUntil(
            loop, token, loop->clock().now() + std::chrono::hours { 1 });
    }

    static core::async::Task<void> run(core::net::EventLoop* loop,
                                       AskpassBroker* broker,
                                       std::optional<std::expected<void, std::string>>* result,
                                       core::async::StopToken token)
    {
        std::ignore = co_await core::async::whenAny(serve(broker, result), untilStopped(loop, token));
    }

    std::optional<std::expected<void, std::string>> _result;
    core::async::StopSource _stop;
    std::thread _thread;
};

/// Says the application object exists from a coroutine on @p loop, once the loop has turned: the loop must
/// still run while another of its coroutines waits in whenServing().
core::async::Task<void> readyLater(core::net::EventLoop* loop,
                                   contour::platform::ApplicationReadiness* application)
{
    co_await loop->delay(std::chrono::milliseconds { 1 });
    application->markReady();
}

core::async::Task<void> serveInto(AskpassBroker* broker,
                                  std::optional<std::expected<void, std::string>>* served)
{
    *served = co_await broker->whenServing();
}

core::async::Task<void> serveWhileReadying(core::net::EventLoop* loop,
                                           AskpassBroker* broker,
                                           contour::platform::ApplicationReadiness* application,
                                           std::optional<std::expected<void, std::string>>* served)
{
    co_await core::async::whenAll(serveInto(broker, served), readyLater(loop, application));
}

/// Sends @p request as a raw client would.
[[nodiscard]] std::unique_ptr<QLocalSocket> connectRaw(
    std::map<std::string, std::string, std::less<>> const& v)
{
    auto socket = std::make_unique<QLocalSocket>();
    socket->connectToServer(QString::fromStdString(v.at("CONTOUR_ASKPASS_ENDPOINT")));
    REQUIRE(socket->waitForConnected(2000)); // pumps no GUI events, which the server needs only to accept
    return socket;
}

void send(QLocalSocket& socket, AskpassRequest const& request)
{
    auto const frame = encode(request);
    socket.write(frame.data(), static_cast<qint64>(frame.size()));
    socket.flush();
}
} // namespace

TEST_CASE("the askpass broker's session names ssh's askpass environment", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);
    CHECK(variables.at("SSH_ASKPASS") == "/opt/contour/contour");
    CHECK(variables.at("SSH_ASKPASS_REQUIRE") == "force");
    CHECK_FALSE(variables.at("CONTOUR_ASKPASS_ENDPOINT").empty());
    CHECK(variables.at("CONTOUR_ASKPASS_NONCE").size() == 64);

    // A second session has a socket and a nonce of its own.
    auto other = broker.open(targetOf());
    REQUIRE(other.has_value());
    auto const otherVariables = variablesOf(**other);
    CHECK(otherVariables.at("CONTOUR_ASKPASS_ENDPOINT") != variables.at("CONTOUR_ASKPASS_ENDPOINT"));
    CHECK(otherVariables.at("CONTOUR_ASKPASS_NONCE") != variables.at("CONTOUR_ASKPASS_NONCE"));

    // Reachable by the user alone.
    auto const permissions = std::filesystem::status(variables.at("CONTOUR_ASKPASS_ENDPOINT")).permissions();
    using std::filesystem::perms;
    CHECK((permissions & (perms::group_all | perms::others_all)) == perms::none);
}

TEST_CASE("the askpass broker draws its nonce and endpoint name from the injected source",
          "[askpass][broker]")
{
    auto sink = FakeSink {};
    auto random = vthost::testing::CountingRandom {};
    auto broker = AskpassBroker { sink,
                                  readyApplication(),
                                  "/opt/contour/contour",
                                  random,
                                  core::platform::defaultSteadyClock(),
                                  AskpassBrokerConfig { .maxUnauthenticated = 4,
                                                        .requestTimeout = std::chrono::seconds { 10 },
                                                        .socketDirectory = {} } };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);
    // The nonce first: bytes 1..32, then the endpoint name's 33..40.
    CHECK(variables.at("CONTOUR_ASKPASS_NONCE")
          == "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20");
    CHECK(variables.at("CONTOUR_ASKPASS_ENDPOINT").contains("contour-askpass-2122232425262728"));
}

TEST_CASE("the askpass broker starts no thread before the application object exists", "[askpass][broker]")
{
    // `contour client --host` makes the broker, and gets to its first session, before the GUI has made its
    // QApplication: Qt runs no event loop on a thread before then.
    using contour::platform::ApplicationReadiness;
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto const application = std::make_shared<ApplicationReadiness>(ApplicationReadiness::State::Pending);
    auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
    CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));

    // open() never waits: it fails while the object is not there, and starts nothing.
    auto const early = broker.open(targetOf());
    REQUIRE_FALSE(early.has_value());
    CHECK(early.error() == "cannot serve ssh's prompts: the application has not started yet");
    CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));

    // whenServing() parks its coroutine, not its loop; whoever says the object exists starts the thread.
    auto waiting = ServingWait { broker };
    AskpassBrokerTestAccess::awaitWaiters(broker, 1);
    CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));
    application->markReady();
    CHECK(AskpassBrokerTestAccess::isServing(broker));
    auto const served = waiting.result();
    REQUIRE(served.has_value());
    CHECK(served->has_value());
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    CHECK_FALSE(variablesOf(**session).at("CONTOUR_ASKPASS_ENDPOINT").empty());
}

TEST_CASE("the askpass broker serves nothing when the application object will not come", "[askpass][broker]")
{
    using contour::platform::ApplicationReadiness;
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto const application = std::make_shared<ApplicationReadiness>(ApplicationReadiness::State::Pending);
    auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
    auto waiting = ServingWait { broker };
    AskpassBrokerTestAccess::awaitWaiters(broker, 1);
    // A launch that gives up before it made the QApplication lets the waiter go.
    application->abandon();
    auto const served = waiting.result();
    REQUIRE(served.has_value());
    REQUIRE_FALSE(served->has_value());
    CHECK(served->error() == "cannot serve ssh's prompts: the application did not start");
    CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));
    CHECK(broker.open(targetOf()).error() == "cannot serve ssh's prompts: the application did not start");
}

TEST_CASE("the askpass broker serves nothing once the application object is going", "[askpass][broker]")
{
    // The links outlive the QApplication on the way out: a reconnect then must not start a Qt thread.
    using contour::platform::ApplicationReadiness;
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto const application = std::make_shared<ApplicationReadiness>(ApplicationReadiness::State::Ready);

    SECTION("a broker made after it closed never starts")
    {
        application->close();
        auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
        CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));
        CHECK(broker.open(targetOf()).error() == "cannot serve ssh's prompts: the application has quit");
        auto waiting = ServingWait { broker };
        auto const served = waiting.result();
        REQUIRE(served.has_value());
        CHECK(served->error() == "cannot serve ssh's prompts: the application has quit");
    }
    SECTION("a broker that serves opens nothing more")
    {
        auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
        REQUIRE(AskpassBrokerTestAccess::isServing(broker));
        application->close();
        CHECK(broker.open(targetOf()).error() == "cannot serve ssh's prompts: the application has quit");
    }
}

TEST_CASE("a stop ends the askpass broker's wait for the application object", "[askpass][broker]")
{
    // A link stopped, or an attach given up, while its ssh attempt waits for the GUI: the wait ends with it.
    using contour::platform::ApplicationReadiness;
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto const application = std::make_shared<ApplicationReadiness>(ApplicationReadiness::State::Pending);
    auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
    auto waiting = ServingWait { broker };
    AskpassBrokerTestAccess::awaitWaiters(broker, 1);
    waiting.stop();
    CHECK_FALSE(waiting.result().has_value());
    CHECK(AskpassBrokerTestAccess::waiting(broker) == 0);
    CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));
}

TEST_CASE("the askpass broker's wait leaves its loop running", "[askpass][broker]")
{
    // The loop that awaits whenServing() goes on serving its other coroutines: here the one that says the
    // application object exists, which a loop blocked by the wait would never get to.
    using contour::platform::ApplicationReadiness;
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto const application = std::make_shared<ApplicationReadiness>(ApplicationReadiness::State::Pending);
    auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
    auto served = std::optional<std::expected<void, std::string>> {};
    auto const backend = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *backend };
    loop.blockOn(serveWhileReadying(&loop, &broker, application.get(), &served));
    REQUIRE(served.has_value());
    CHECK(served->has_value());
    CHECK(AskpassBrokerTestAccess::isServing(broker));
}

TEST_CASE("a shutdown never races the askpass broker's thread start", "[askpass][broker]")
{
    using contour::platform::ApplicationReadiness;
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto const application = std::make_shared<ApplicationReadiness>(ApplicationReadiness::State::Pending);

    SECTION("a shutdown before the application object exists: the thread never starts")
    {
        auto broker = AskpassBroker { sink, application, "/opt/contour/contour", random };
        auto starts = 0;
        AskpassBrokerTestAccess::beforeThreadStart(broker, [&] { ++starts; });
        broker.shutdown();
        application->markReady();
        CHECK(starts == 0);
        CHECK_FALSE(AskpassBrokerTestAccess::isServing(broker));
        CHECK(broker.open(targetOf()).error() == "the application no longer answers ssh's prompts");
    }
    SECTION("a shutdown and a destruction while the thread is being started wait for the start")
    {
        auto broker = std::make_unique<AskpassBroker>(sink, application, "/opt/contour/contour", random);
        auto inStart = std::promise<void> {};
        auto release = std::promise<void> {};
        auto released = release.get_future().share();
        auto starts = 0;
        AskpassBrokerTestAccess::beforeThreadStart(*broker, [&] {
            ++starts;
            inStart.set_value();
            released.wait();
        });
        // The GUI thread, saying the object exists, is held just before it starts the thread.
        auto readying = std::thread { [&] { application->markReady(); } };
        inStart.get_future().wait();
        // Meanwhile the broker is shut down and destroyed on another thread: both wait for the start, and the
        // destructor then stops the thread it started. A destroyed QThread that still runs aborts the
        // process.
        auto destroying = std::thread { [&] {
            broker->shutdown();
            broker.reset();
        } };
        release.set_value();
        readying.join();
        destroying.join();
        CHECK(starts == 1);
        CHECK(broker == nullptr);
    }
}

TEST_CASE("the askpass broker's prompt ids come from the source it shares", "[askpass][broker]")
{
    // A fingerprint prompt takes its id from the same source, without a broker: the ids never collide.
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return answeredReply("hunter2");
    };
    auto const router = std::make_shared<AskpassPromptRouter>();
    router->attach(AnyWindow, sink);
    auto const ids = std::make_shared<AskpassPromptIds>();
    CHECK(ids->next() == 1);
    auto broker = AskpassBroker { router, ids, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const run = runHelper(variablesOf(**session), "me@host's password: ");
    CHECK(run.exitCode == 0);
    REQUIRE(sink.asks.size() == 1);
    CHECK(sink.asks.front().promptId == 2);
    CHECK(ids->next() == 3);
}

TEST_CASE("a secret prompt is answered through the sink and printed", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto const capture = core::log::ScopedCapture {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return answeredReply("hunter2");
    };
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());

    auto const run = runHelper(variablesOf(**session), "me@host's password: ");
    CHECK(run.exitCode == 0);
    CHECK(run.out == "hunter2\n");
    REQUIRE(sink.asks.size() == 1);
    CHECK(sink.asks.front().kind == AskpassPromptKind::Secret);
    CHECK(sink.asks.front().target == "ssh://me@host");
    CHECK(sink.asks.front().prompt == "me@host's password: ");
    CHECK_FALSE((*session)->wasCancelled());

    // The answer is a secret: no log line, at any level, says it -- and the capture did see the broker's
    // lines.
    CHECK(capture.contains("askpass:"));
    CHECK_FALSE(capture.contains("hunter2"));
}

TEST_CASE("the kind ssh's environment names reaches the sink", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return answeredReply("yes");
    };
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto variables = variablesOf(**session);
    variables["SSH_ASKPASS_PROMPT"] = "confirm";
    CHECK(runHelper(variables).exitCode == 0);
    REQUIRE(sink.asks.size() == 1);
    CHECK(sink.asks.front().kind == AskpassPromptKind::Confirm);
}

TEST_CASE("a cancelled prompt exits 1 and marks the session cancelled", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { .outcome = AskpassOutcome::Cancelled, .answer = Secret { "leaked?" } };
    };
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto helper = std::vector<std::future<HelperRun>> {};
    helper.push_back(startHelper(variablesOf(**session)));
    pumpUntil([&] { return (*session)->wasCancelled(); });
    auto const runs = endCancelled(*session, std::move(helper));
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().exitCode == 1);
    CHECK(runs.front().out.empty());
}

TEST_CASE(
    "dismissing a prompt wakes the transport, and a second request of the attempt never reaches the sink",
    "[askpass][broker][cancel]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { AskpassOutcome::Cancelled, {} };
    };
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto woken = std::atomic<int> { 0 };
    (*session)->onCancelled([&woken] { ++woken; });

    auto helpers = std::vector<std::future<HelperRun>> {};
    helpers.push_back(startHelper(variablesOf(**session)));
    // The transport was told, from the broker's thread, as the user dismissed the prompt.
    pumpUntil([&] { return woken.load() == 1; });
    CHECK((*session)->wasCancelled());

    // ssh asks again before it has been ended: nobody is asked, and it is held like the first.
    helpers.push_back(startHelper(variablesOf(**session), "Password: "));
    pumpUntil([&] { return (*session)->wasCancelled(); });
    CHECK(woken.load() == 1);

    // A transport that listens late is told at once.
    auto late = std::atomic<int> { 0 };
    (*session)->onCancelled([&late] { ++late; });
    CHECK(late.load() == 1);

    // Neither helper has an answer until ssh is ended and the session dropped; then both see EOF and exit 1.
    auto const runs = endCancelled(*session, std::move(helpers));
    REQUIRE(runs.size() == 2);
    CHECK(sink.asks.size() == 1);
    for (auto const& run: runs)
    {
        CHECK(run.exitCode == 1);
        CHECK(run.out.empty());
    }
}

TEST_CASE("cancelling a notice, a security key's touch, ends the attempt", "[askpass][broker][cancel]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto woken = std::atomic<int> { 0 };
    (*session)->onCancelled([&woken] { ++woken; });
    auto variables = variablesOf(**session);
    variables["SSH_ASKPASS_PROMPT"] = "none";
    auto helper = startHelper(variables, "Confirm user presence for key ED25519-SK");
    pumpUntil([&] { return sink.asks.size() == 1; });
    CHECK(sink.asks.front().kind == AskpassPromptKind::Notice);
    CHECK_FALSE((*session)->wasCancelled());

    // The notice's one button is Cancel.
    sink.held(AskpassReply { AskpassOutcome::Cancelled, {} });
    pumpUntil([&] { return woken.load() == 1; });
    CHECK((*session)->wasCancelled());
    auto helpers = std::vector<std::future<HelperRun>> {};
    helpers.push_back(std::move(helper));
    auto const runs = endCancelled(*session, std::move(helpers));
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().exitCode == 1);
}

TEST_CASE("a window's Cancel on a notice ends the attempt, end to end", "[askpass][broker][cancel]")
{
    auto random = vthost::testing::CountingRandom {};
    // The dialog's host takes the notice, its Cancel button is answerPrompt(Cancelled): the transport is
    // woken, and the helper exits 1 once the transport has dropped the session.
    auto router = std::make_shared<AskpassPromptRouter>();
    auto host = contour::window::PromptHost { router, 1 };
    auto const broker = std::make_shared<AskpassBroker>(router,
                                                        std::make_shared<AskpassPromptIds>(),
                                                        readyApplication(),
                                                        QStringLiteral("/opt/contour/contour"),
                                                        random);
    auto session = RoutedAskpassBroker { broker, 1 }.open(targetOf());
    REQUIRE(session.has_value());
    auto woken = std::atomic<int> { 0 };
    (*session)->onCancelled([&woken] { ++woken; });
    auto variables = variablesOf(**session);
    variables["SSH_ASKPASS_PROMPT"] = "none";
    auto helper = startHelper(variables, "Confirm user presence");
    pumpUntil([&] { return !host.pendingPrompt().isEmpty(); });

    host.answerPrompt(
        host.pendingPrompt().value("promptId").toInt(), static_cast<int>(AskpassOutcome::Cancelled), {});
    pumpUntil([&] { return woken.load() == 1; });
    CHECK((*session)->wasCancelled());
    auto helpers = std::vector<std::future<HelperRun>> {};
    helpers.push_back(std::move(helper));
    auto const runs = endCancelled(*session, std::move(helpers));
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().exitCode == 1);
}

TEST_CASE("a request with the wrong nonce is refused without a prompt", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto variables = variablesOf(**session);
    variables["CONTOUR_ASKPASS_NONCE"] = std::string(64, '0');
    auto const run = runHelper(variables);
    CHECK(run.exitCode == 1);
    CHECK(run.out.empty());
    CHECK(sink.asks.empty());
    CHECK_FALSE((*session)->wasCancelled());
}

TEST_CASE("a helper that goes away mid-prompt has the prompt withdrawn", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);

    auto client = connectRaw(variables);
    send(*client,
         AskpassRequest { .nonce = variables.at("CONTOUR_ASKPASS_NONCE"),
                          .prompt = "Confirm user presence for key",
                          .sshAskpassPrompt = "none" });
    pumpUntil([&] { return !sink.asks.empty(); });
    CHECK(sink.asks.front().kind == AskpassPromptKind::Notice);
    CHECK(sink.withdrawn.empty());

    client->disconnectFromServer();
    client = nullptr;
    pumpUntil([&] { return !sink.withdrawn.empty(); });
    CHECK(sink.withdrawn == std::vector { sink.asks.front().promptId });
    CHECK_FALSE((*session)->wasCancelled());
}

TEST_CASE("the time a prompt is open is counted, and the server closes with its session", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto clock = core::platform::ManualClock {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random, clock };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);
    CHECK((*session)->promptTime() == 0ms);

    auto client = connectRaw(variables);
    send(*client,
         AskpassRequest {
             .nonce = variables.at("CONTOUR_ASKPASS_NONCE"), .prompt = "pw: ", .sshAskpassPrompt = "" });
    pumpUntil([&] { return !sink.asks.empty(); });
    clock.advance(3s);
    CHECK((*session)->promptTime() == 3s);

    sink.held(answeredReply("x"));
    // The answer is written once the broker's thread has closed the prompt; the client has it by then.
    REQUIRE(client->waitForReadyRead(2000));
    clock.advance(10s);
    CHECK((*session)->promptTime() == 3s); // closed: later time is not the prompt's

    client = nullptr;
    auto const endpoint = variables.at("CONTOUR_ASKPASS_ENDPOINT");
    session->reset();
    CHECK_FALSE(std::filesystem::exists(endpoint));
    auto late = QLocalSocket {};
    late.connectToServer(QString::fromStdString(variables.at("CONTOUR_ASKPASS_ENDPOINT")));
    CHECK_FALSE(late.waitForConnected(500));
}

namespace
{
/// A controller whose connect needs a prompt answered on the GUI thread, as an ssh attach does: its reactor
/// opens a broker session and runs the askpass helper against it, and is ready once the helper got its
/// answer.
class PromptingController final: public contour::remote::RemoteController
{
  public:
    explicit PromptingController(AskpassBroker& broker):
        RemoteController(core::platform::defaultSteadyClock(), 3000ms), _broker { broker }
    {
    }
    PromptingController(PromptingController const&) = delete;
    PromptingController& operator=(PromptingController const&) = delete;
    PromptingController(PromptingController&&) = delete;
    PromptingController& operator=(PromptingController&&) = delete;
    ~PromptingController() { stop(); }

    std::string answer; ///< What the helper printed; read after the connect.

  protected:
    [[nodiscard]] core::async::Task<void> runClient(core::net::EventLoop* /*loop*/) override
    {
        auto session = _broker.open(
            targetOf()); // the reactor thread opens the session; the broker's own thread serves its socket
        if (!session)
        {
            failConnect(session.error());
            co_return;
        }
        // Blocks this thread, not the GUI's: the helper's prompt is answered over there.
        auto const run = startHelper(variablesOf(**session)).get();
        answer = run.out;
        {
            auto const lock = std::lock_guard { _mutex };
            _state = State::Ready;
        }
        _connected.notify_all();
    }
    [[nodiscard]] bool detachOnReactor() override { return false; }
    void closeReactorBindings() override {}
    [[nodiscard]] std::string connectTimeoutMessage() const override { return "timed out"; }
    [[nodiscard]] std::string connectClosedMessage() const override { return "closed"; }

  private:
    AskpassBroker& _broker;
};
} // namespace

TEST_CASE("an attach polled from the event loop completes once its prompt is answered, and the loop is free "
          "meanwhile",
          "[askpass][broker][controller]")
{
    auto random = vthost::testing::CountingRandom {};
    // What the launch of `contour client --host` does: it looks at the attach from the event loop, which is
    // what serves the broker's server and the window that shows the prompt. Nothing waits on the GUI thread.
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto controller = PromptingController { broker };
    auto const budget = vthost::AttachBudget { 10s };

    CHECK_FALSE(controller.pollConnected(budget).has_value());
    pumpUntil([&] { return sink.asks.size() == 1; });
    // The prompt is open and nobody answered: the attach goes on, and the look returns at once.
    CHECK_FALSE(controller.pollConnected(budget).has_value());

    sink.held(answeredReply("hunter2"));
    auto settled = std::optional<std::expected<void, std::string>> {};
    pumpUntil([&] {
        settled = controller.pollConnected(budget);
        return settled.has_value();
    });
    REQUIRE(settled.has_value());
    CHECK(settled->has_value());
    CHECK(controller.answer == "hunter2\n");
    CHECK(sink.asks.size() == 1);
}

TEST_CASE("a request that is oversized or malformed is refused without a prompt", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);

    SECTION("oversized")
    {
        auto client = connectRaw(variables);
        auto frame = std::string(AskpassFramePrefixSize, '\xFF'); // declares a 4 GiB payload
        client->write(frame.data(), static_cast<qint64>(frame.size()));
        client->flush();
        pumpUntil([&] { return client->state() == QLocalSocket::UnconnectedState; });
    }
    SECTION("malformed")
    {
        auto client = connectRaw(variables);
        auto const garbage = std::string { "\x03\0\0\0xyz", 7 };
        client->write(garbage.data(), static_cast<qint64>(garbage.size()));
        client->flush();
        pumpUntil([&] { return client->state() == QLocalSocket::UnconnectedState; });
    }
    CHECK(sink.asks.empty());
}

TEST_CASE("a session serves one request after another, as ssh asks a password and then a code",
          "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const& ask) {
        return answeredReply(ask.prompt.toStdString());
    };
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);
    CHECK(runHelper(variables, "password: ").out == "password: \n");
    // The right nonce again, after the first request was answered: it is a request of its own.
    CHECK(runHelper(variables, "code: ").out == "code: \n");
    CHECK(sink.asks.size() == 2);
}

TEST_CASE("connections that do not send their request are bounded in number and in time", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker {
        sink,
        readyApplication(),
        "/opt/contour/contour",
        random,
        core::platform::defaultSteadyClock(),
        AskpassBrokerConfig { .maxUnauthenticated = 2, .requestTimeout = 300ms, .socketDirectory = {} }
    };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);

    auto first = connectRaw(variables);
    auto second = connectRaw(variables);
    auto third = connectRaw(variables);
    pumpUntil([&] { return third->state() == QLocalSocket::UnconnectedState; });
    CHECK(first->state() == QLocalSocket::ConnectedState);
    CHECK(second->state() == QLocalSocket::ConnectedState);

    // Silent ones are closed once the time is up.
    pumpUntil([&] {
        return first->state() == QLocalSocket::UnconnectedState
               && second->state() == QLocalSocket::UnconnectedState;
    });
    CHECK(sink.asks.empty());
}

TEST_CASE("a session opened and destroyed off the GUI thread needs no GUI event loop",
          "[askpass][broker][threading]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    // This thread serves no events while the other one opens and drops its session.
    auto opened = false;
    auto endpoint = std::string {};
    auto existedWhileOpen = false;
    auto const started = std::chrono::steady_clock::now();
    std::thread { [&] {
        auto session = broker.open(targetOf());
        opened = session.has_value();
        if (opened)
        {
            endpoint = variablesOf(**session).at("CONTOUR_ASKPASS_ENDPOINT");
            // Listening before open() returned, with nothing here to serve the event loop.
            existedWhileOpen = std::filesystem::exists(endpoint);
        }
    } }.join();
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    REQUIRE(opened);
    CHECK(existedWhileOpen);
    CHECK_FALSE(std::filesystem::exists(endpoint)); // closed with the session, on the broker's thread
}

TEST_CASE("a listen failure off the GUI thread is open()'s error", "[askpass][broker][threading]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker =
        AskpassBroker { sink,
                        readyApplication(),
                        "/opt/contour/contour",
                        random,
                        core::platform::defaultSteadyClock(),
                        AskpassBrokerConfig { .socketDirectory = "/nonexistent-askpass-directory/x" } };
    auto opened = true;
    auto reason = std::string {};
    std::thread { [&] {
        auto session = broker.open(targetOf());
        opened = session.has_value();
        if (!opened)
            reason = session.error();
    } }.join();
    CHECK_FALSE(opened);
    CHECK(reason.starts_with("cannot listen for ssh's prompts"));
}

TEST_CASE("a helper that connects before the GUI serves events waits for its answer", "[askpass][broker]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return answeredReply("hunter2");
    };
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());

    auto helper = startHelper(variablesOf(**session));
    std::this_thread::sleep_for(300ms); // no event loop here meanwhile
    CHECK(helper.wait_for(0ms) != std::future_status::ready);
    CHECK(sink.asks.empty());

    while (helper.wait_for(0ms) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    auto const run = helper.get();
    CHECK(run.exitCode == 0);
    CHECK(run.out == "hunter2\n");
}

namespace
{
/// A controller whose reactor opens a broker session and goes on connecting, as an ssh attach does.
class OpeningController final: public contour::remote::RemoteController
{
  public:
    explicit OpeningController(AskpassBroker& broker):
        RemoteController(core::platform::defaultSteadyClock(), 3000ms), _broker { broker }
    {
    }
    OpeningController(OpeningController const&) = delete;
    OpeningController& operator=(OpeningController const&) = delete;
    OpeningController(OpeningController&&) = delete;
    OpeningController& operator=(OpeningController&&) = delete;
    ~OpeningController() { stop(); }

    std::atomic<bool> opened = false;

  protected:
    [[nodiscard]] core::async::Task<void> runClient(core::net::EventLoop* loop) override
    {
        auto session = _broker.open(targetOf());
        opened = session.has_value();
        while (true)
            co_await loop->delay(1h);
    }
    [[nodiscard]] bool detachOnReactor() override { return false; }
    void closeReactorBindings() override {}
    [[nodiscard]] std::string connectTimeoutMessage() const override { return "timed out"; }
    [[nodiscard]] std::string connectClosedMessage() const override { return "closed"; }

  private:
    AskpassBroker& _broker;
};
} // namespace

TEST_CASE("a stop while the GUI thread serves no events does not hang on the broker",
          "[askpass][broker][threading]")
{
    auto random = vthost::testing::CountingRandom {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, readyApplication(), "/opt/contour/contour", random };
    auto controller = OpeningController { broker };
    controller.startConnecting();
    // Never pumped here: a quitting or not yet running GUI thread.
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!controller.opened && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    REQUIRE(controller.opened);
    auto const started = std::chrono::steady_clock::now();
    controller.stop();
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

TEST_CASE("each window's attach prompts in its own window", "[askpass][broker][route]")
{
    auto random = vthost::testing::CountingRandom {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto left = FakeSink {};
    auto right = FakeSink {};
    left.answer = [](FakeSink::Ask const&) {
        return answeredReply("left-secret");
    };
    right.answer = [](FakeSink::Ask const&) {
        return answeredReply("right-secret");
    };
    router->attach(1, left);
    router->attach(2, right);
    auto const broker = std::make_shared<AskpassBroker>(router,
                                                        std::make_shared<AskpassPromptIds>(),
                                                        readyApplication(),
                                                        QStringLiteral("/opt/contour/contour"),
                                                        random);
    // Two windows attach to the same host.
    auto leftSession = RoutedAskpassBroker { broker, 1 }.open(targetOf());
    auto rightSession = RoutedAskpassBroker { broker, 2 }.open(targetOf());
    REQUIRE(leftSession.has_value());
    REQUIRE(rightSession.has_value());

    auto const rightRun = runHelper(variablesOf(**rightSession));
    auto const leftRun = runHelper(variablesOf(**leftSession));

    CHECK(rightRun.out == "right-secret\n");
    CHECK(leftRun.out == "left-secret\n");
    CHECK(left.asks.size() == 1);
    CHECK(right.asks.size() == 1);
}

TEST_CASE("an attach with no window waits for the first window to appear", "[askpass][broker][route]")
{
    auto random = vthost::testing::CountingRandom {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto const broker = std::make_shared<AskpassBroker>(router,
                                                        std::make_shared<AskpassPromptIds>(),
                                                        readyApplication(),
                                                        QStringLiteral("/opt/contour/contour"),
                                                        random);
    auto session = broker->open(targetOf());
    REQUIRE(session.has_value());
    auto helper = startHelper(variablesOf(**session));

    pumpUntil([&] { return router->waitingCount() == 1; });

    auto window = FakeSink {};
    window.answer = [](FakeSink::Ask const&) {
        return answeredReply("late-window");
    };
    router->attach(7, window);
    while (helper.wait_for(0ms) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    CHECK(helper.get().out == "late-window\n");
}

TEST_CASE("a broker a transport kept alive past the windows neither crashes nor prompts",
          "[askpass][broker][lifetime]")
{
    auto random = vthost::testing::CountingRandom {};
    auto router = std::make_shared<AskpassPromptRouter>();
    auto sink = std::make_unique<FakeSink>();
    router->attach(1, *sink);
    auto const broker = std::make_shared<AskpassBroker>(router,
                                                        std::make_shared<AskpassPromptIds>(),
                                                        readyApplication(),
                                                        QStringLiteral("/opt/contour/contour"),
                                                        random);
    auto session = broker->open(targetOf());
    REQUIRE(session.has_value());
    auto const variables = variablesOf(**session);

    // The application goes: the broker is shut down, the sink dies, a factory still holds the broker.
    broker->shutdown();
    router->detach(1);
    sink.reset();

    // A helper that is still around is cancelled without the sink being asked (and exits 1 once the session
    // is dropped), and no new attach is given a session: ssh then runs without prompts.
    auto helpers = std::vector<std::future<HelperRun>> {};
    helpers.push_back(startHelper(variables));
    pumpUntil([&] { return (*session)->wasCancelled(); });
    auto const runs = endCancelled(*session, std::move(helpers));
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().exitCode == 1);
    CHECK(runs.front().out.empty());
    auto const later = broker->open(targetOf());
    CHECK_FALSE(later.has_value());
}

#endif
