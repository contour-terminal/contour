// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassBroker.hpp>
#include <contour/askpass/AskpassClient.hpp>
#include <contour/remote/RemoteController.hpp>

#include <core/log/LogSink.hpp>
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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <vthost/SshTransport.hpp>

#ifndef _WIN32

using namespace std::chrono_literals;
using namespace contour::askpass;

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
        if (auto const given = answer ? answer(asks.back()) : std::nullopt)
            reply(*given);
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
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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

TEST_CASE("a secret prompt is answered through the sink and printed", "[askpass][broker]")
{
    auto const capture = core::log::ScopedCapture {};
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { AskpassOutcome::Answered, "hunter2" };
    };
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { AskpassOutcome::Answered, "yes" };
    };
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { AskpassOutcome::Cancelled, "leaked?" };
    };
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
    auto session = broker.open(targetOf());
    REQUIRE(session.has_value());
    auto const run = runHelper(variablesOf(**session));
    CHECK(run.exitCode == 1);
    CHECK(run.out.empty());
    CHECK((*session)->wasCancelled());
}

TEST_CASE("a request with the wrong nonce is refused without a prompt", "[askpass][broker]")
{
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto clock = core::platform::ManualClock {};
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour", clock };
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

    sink.held(AskpassReply { AskpassOutcome::Answered, "x" });
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
    PromptingController(AskpassBroker& broker, contour::remote::WaitPump pump):
        RemoteController(core::platform::defaultSteadyClock(), 3000ms, std::move(pump)), _broker { broker }
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
        auto session = _broker.open(targetOf()); // the reactor thread asks the GUI thread's broker
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

TEST_CASE("a connect awaited on the GUI thread completes while its prompt is answered there",
          "[askpass][broker][controller]")
{
    // The reactor thread below must not pump the GUI's events: the helper it starts is awaited by the GUI
    // thread, whose pump is what serves the broker's server and the sink.
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { AskpassOutcome::Answered, "hunter2" };
    };
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
    auto controller =
        PromptingController { broker,
                              [] { QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents); } };
    auto const connected = controller.connectAndWait(10s);
    REQUIRE(connected.has_value());
    CHECK(controller.answer == "hunter2\n");
    CHECK(sink.asks.size() == 1);
}

TEST_CASE("a request that is oversized or malformed is refused without a prompt", "[askpass][broker]")
{
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const& ask) {
        return AskpassReply { AskpassOutcome::Answered, ask.prompt.toStdString() };
    };
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink,
                                  "/opt/contour/contour",
                                  core::platform::defaultSteadyClock(),
                                  AskpassBrokerConfig { .maxUnauthenticated = 2, .requestTimeout = 300ms, .socketDirectory = {} } };
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
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    auto broker =
        AskpassBroker { sink,
                        "/opt/contour/contour",
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
    auto sink = FakeSink {};
    sink.answer = [](FakeSink::Ask const&) {
        return AskpassReply { AskpassOutcome::Answered, "hunter2" };
    };
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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
    auto sink = FakeSink {};
    auto broker = AskpassBroker { sink, "/opt/contour/contour" };
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

#endif
