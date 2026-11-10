// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/askpass/AskpassBroker.hpp>

#include <core/net/Tls.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QRandomGenerator>
#include <QtCore/QStandardPaths>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtNetwork/QLocalServer>
#include <QtNetwork/QLocalSocket>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <vthost/SpawnEnvironment.hpp>
#include <vthost/SshTransport.hpp>

namespace contour::askpass
{

namespace
{
    /// What the thread that polls a session (the ssh transport's) and the broker's thread share.
    struct SessionState
    {
        std::mutex mutex;
        bool isCancelled = false;
        std::chrono::milliseconds closedTime { 0 };               ///< The prompts that have ended.
        std::optional<core::platform::SteadyTimePoint> openSince; ///< The prompt that has not.
        /// The session's server, which lives on the broker's thread; owned by the session. Guarded by @c
        /// mutex.
        QObject* endpoint = nullptr;
    };

    /// How a prompt ended.
    enum class PromptEnd : std::uint8_t
    {
        Withdrawn = 0, ///< The helper went away.
        Cancelled,     ///< The user dismissed it.
        Answered,
    };

    /// Names one connection to a session's server.
    using ConnectionId = std::uint64_t;

    /// What the servers of a broker reach the rest of the program through.
    struct Hub
    {
        std::shared_ptr<AskpassPromptRouter> router; ///< Finds the window that shows a prompt.
        AskpassRouteId route;                        ///< The window that started the attach.
        QObject* guiContext;    ///< Lives on the thread the sink is asked on; outlives every session.
        QObject* workerContext; ///< Lives on the broker's thread; outlives every session.
        std::atomic<AskpassPromptId>& lastPromptId;
        core::platform::IClock const& clock;
    };

    [[nodiscard]] std::string_view nameOf(AskpassPromptKind kind) noexcept
    {
        switch (kind)
        {
            case AskpassPromptKind::Secret: return "secret";
            case AskpassPromptKind::Confirm: return "confirm";
            case AskpassPromptKind::Notice: return "notice";
            case AskpassPromptKind::HostKey: return "host key";
        }
        return "?";
    }

    /// @return @p byteCount random bytes from the operating system's source, as hex.
    [[nodiscard]] QByteArray randomHex(int byteCount)
    {
        auto bytes = QByteArray(byteCount, '\0');
        for (auto const index: std::views::iota(0, byteCount))
            bytes[index] = static_cast<char>(QRandomGenerator::system()->bounded(256U));
        return bytes.toHex();
    }

    /// The server of one session, living on the broker's thread: the helper processes connect here.
    class Endpoint final: public QObject
    {
      public:
        Endpoint(Hub hub,
                 std::shared_ptr<SessionState> state,
                 std::string nonce,
                 QString target,
                 AskpassBrokerConfig config):
            _hub { std::move(hub) },
            _state { std::move(state) },
            _nonce { std::move(nonce) },
            _target { std::move(target) },
            _config { std::move(config) },
            _server { this }
        {
            _server.setSocketOptions(QLocalServer::UserAccessOption);
            connect(&_server, &QLocalServer::newConnection, this, [this] { acceptConnections(); });
        }

        /// Takes down the prompts that are still open.
        ~Endpoint() override
        {
            for (auto const& [id, connection]: _connections)
                if (connection.prompt)
                    postWithdraw(*connection.prompt);
        }

        Endpoint(Endpoint const&) = delete;
        Endpoint& operator=(Endpoint const&) = delete;
        Endpoint(Endpoint&&) = delete;
        Endpoint& operator=(Endpoint&&) = delete;

        /// @param name The unique part of the endpoint's name.
        /// @return Where the server listens, or why it cannot.
        [[nodiscard]] std::expected<QString, std::string> listen(QString const& name)
        {
#ifdef Q_OS_WIN
            auto const listenName = name;
#else
            auto directory = _config.socketDirectory;
            if (directory.isEmpty())
                directory = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
            if (directory.isEmpty())
                directory = QDir::tempPath();
            auto const listenName = QDir(directory).filePath(name);
#endif
            if (!_server.listen(listenName))
                return std::unexpected { _server.errorString().toStdString() };
            return _server.fullServerName();
        }

      private:
        enum class Phase : std::uint8_t
        {
            Receiving = 0, ///< The request is still arriving.
            Asking,        ///< The sink has the prompt.
            Done,          ///< Answered, or refused.
        };

        struct Connection
        {
            QLocalSocket* socket = nullptr;
            std::string received;
            Phase phase = Phase::Receiving;
            std::optional<AskpassPromptId> prompt;
        };

        void acceptConnections()
        {
            while (auto* const socket = _server.nextPendingConnection())
            {
                socket->setParent(this);
                if (unauthenticated() >= _config.maxUnauthenticated)
                {
                    askpassLog()("askpass: refused a connection: too many wait for their request");
                    socket->abort();
                    socket->deleteLater();
                    continue;
                }
                auto const id = ++_lastConnectionId;
                _connections.emplace(id,
                                     Connection { .socket = socket,
                                                  .received = {},
                                                  .phase = Phase::Receiving,
                                                  .prompt = std::nullopt });
                // The context is the socket, so the timer goes with it.
                QTimer::singleShot(_config.requestTimeout, socket, [this, id] {
                    auto const found = _connections.find(id);
                    if (found != _connections.end() && found->second.phase == Phase::Receiving)
                        refuse(id, "no whole request in time");
                });
                connect(socket, &QLocalSocket::readyRead, this, [this, id] { onReadable(id); });
                connect(socket, &QLocalSocket::disconnected, this, [this, id] { onDisconnected(id); });
                // Bytes may have arrived before the slot was connected.
                if (socket->bytesAvailable() > 0)
                    onReadable(id);
            }
        }

        /// @return How many connections have not yet sent a whole, valid request.
        [[nodiscard]] std::size_t unauthenticated() const
        {
            return static_cast<std::size_t>(std::ranges::count_if(
                _connections, [](auto const& each) { return each.second.phase == Phase::Receiving; }));
        }

        void onReadable(ConnectionId id)
        {
            auto const found = _connections.find(id);
            if (found == _connections.end())
                return;
            auto& connection = found->second;
            auto const data = connection.socket->readAll();
            if (connection.phase != Phase::Receiving)
                return;
            connection.received.append(data.constData(), static_cast<std::size_t>(data.size()));

            auto const size = askpassFrameSize(connection.received);
            if (!size)
            {
                refuse(id, "an oversized frame");
                return;
            }
            if (!size->has_value() || connection.received.size() < **size)
                return;
            auto const request =
                decodeAskpassRequest(std::string_view { connection.received }.substr(0, **size));
            if (!request)
            {
                refuse(id, "a malformed request");
                return;
            }
            if (!core::net::constantTimeEquals(request->nonce, _nonce))
            {
                refuse(id, "a wrong nonce");
                return;
            }
            ask(id, connection, *request);
        }

        void ask(ConnectionId id, Connection& connection, AskpassRequest const& request)
        {
            connection.phase = Phase::Asking;
            auto const kind = classifyAskpassPrompt(request.prompt, request.sshAskpassPrompt);
            auto const promptId = ++_hub.lastPromptId;
            connection.prompt = promptId;
            askpassLog()("askpass: asking a {} prompt", nameOf(kind));
            if (_asking++ == 0)
            {
                auto const lock = std::lock_guard { _state->mutex };
                _state->openSince = _hub.clock.now();
            }
            // The reply comes from the sink's thread, back to this one by a call queued to it: this
            // endpoint may be gone by then, which is only certain to be seen from this thread.
            auto reply = [worker = _hub.workerContext, self = QPointer { this }, id](AskpassReply given) {
                QMetaObject::invokeMethod(
                    worker,
                    [self, id, given = std::move(given)]() mutable {
                        if (self)
                            self->answer(id, std::move(given));
                        else
                            wipe(given.answer);
                    },
                    Qt::QueuedConnection);
            };
            QMetaObject::invokeMethod(
                _hub.guiContext,
                [router = _hub.router,
                 route = _hub.route,
                 promptId,
                 kind,
                 target = _target,
                 prompt = QString::fromStdString(request.prompt),
                 reply = std::move(reply)]() mutable {
                    router->ask(
                        route, promptId, kind, std::move(target), std::move(prompt), std::move(reply));
                },
                Qt::QueuedConnection);
        }

        void answer(ConnectionId id, AskpassReply reply)
        {
            auto const found = _connections.find(id);
            if (found == _connections.end() || found->second.phase != Phase::Asking)
            {
                wipe(reply.answer);
                return;
            }
            auto& connection = found->second;
            connection.phase = Phase::Done;
            auto const cancelled = reply.outcome == AskpassOutcome::Cancelled;
            askpassLog()("askpass: the prompt was {}", cancelled ? "cancelled" : "answered");
            // A cancelled prompt carries no answer, whatever the sink put there.
            if (cancelled)
                wipe(reply.answer);
            closePrompt(cancelled ? PromptEnd::Cancelled : PromptEnd::Answered);
            auto frame = encode(reply);
            wipe(reply.answer);
            connection.socket->write(frame.data(), static_cast<qint64>(frame.size()));
            connection.socket->flush();
            wipe(frame);
            connection.socket->disconnectFromServer();
        }

        void onDisconnected(ConnectionId id)
        {
            auto const found = _connections.find(id);
            if (found == _connections.end())
                return;
            if (found->second.phase == Phase::Asking)
            {
                askpassLog()("askpass: the helper went away; withdrawing the prompt");
                closePrompt(PromptEnd::Withdrawn);
                postWithdraw(*found->second.prompt);
            }
            found->second.socket->deleteLater();
            _connections.erase(found);
        }

        void refuse(ConnectionId id, std::string_view reason)
        {
            askpassLog()("askpass: refused a request: {}", reason);
            auto const found = _connections.find(id);
            if (found == _connections.end())
                return;
            auto* const socket = found->second.socket;
            _connections.erase(found);
            socket->abort();
            socket->deleteLater();
        }

        void postWithdraw(AskpassPromptId promptId) const
        {
            QMetaObject::invokeMethod(
                _hub.guiContext,
                [router = _hub.router, promptId] { router->withdraw(promptId); },
                Qt::QueuedConnection);
        }

        void closePrompt(PromptEnd end)
        {
            --_asking;
            auto const lock = std::lock_guard { _state->mutex };
            if (end == PromptEnd::Cancelled)
                _state->isCancelled = true;
            if (_asking == 0 && _state->openSince)
            {
                _state->closedTime +=
                    std::chrono::ceil<std::chrono::milliseconds>(_hub.clock.now() - *_state->openSince);
                _state->openSince.reset();
            }
        }

        Hub _hub;
        std::shared_ptr<SessionState> _state;
        std::string _nonce;
        QString _target;
        AskpassBrokerConfig _config;
        QLocalServer _server;
        std::map<ConnectionId, Connection> _connections;
        ConnectionId _lastConnectionId = 0;
        int _asking = 0; ///< Prompts the sink holds.
    };

    /// The session as the ssh transport sees it: a handle on the endpoint on the broker's thread.
    class Session final: public vthost::IAskpassSession
    {
      public:
        Session(std::shared_ptr<SessionState> state,
                QObject* workerContext,
                core::platform::IClock const& clock,
                vthost::SpawnEnvironment environment):
            _state { std::move(state) },
            _workerContext { workerContext },
            _clock { clock },
            _environment { std::move(environment) }
        {
        }

        /// Closes the server, on the broker's thread, before it returns: the socket file is gone by then.
        /// That thread waits for nobody, so this cannot wait for long, on whichever thread it runs.
        ~Session() override
        {
            auto* endpoint = static_cast<QObject*>(nullptr);
            {
                auto const lock = std::lock_guard { _state->mutex };
                endpoint = std::exchange(_state->endpoint, nullptr);
            }
            if (endpoint != nullptr)
                QMetaObject::invokeMethod(
                    _workerContext, [endpoint] { delete endpoint; }, Qt::BlockingQueuedConnection);
        }

        Session(Session const&) = delete;
        Session& operator=(Session const&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        [[nodiscard]] vthost::SpawnEnvironment environment() const override { return _environment; }

        [[nodiscard]] bool wasCancelled() const noexcept override
        {
            auto const lock = std::lock_guard { _state->mutex };
            return _state->isCancelled;
        }

        [[nodiscard]] std::chrono::milliseconds promptTime() const noexcept override
        {
            auto const lock = std::lock_guard { _state->mutex };
            auto total = _state->closedTime;
            if (_state->openSince)
                total += std::chrono::ceil<std::chrono::milliseconds>(_clock.now() - *_state->openSince);
            return total;
        }

      private:
        std::shared_ptr<SessionState> _state;
        QObject* _workerContext;
        core::platform::IClock const& _clock;
        vthost::SpawnEnvironment _environment;
    };
} // namespace

AskpassBroker::AskpassBroker(std::shared_ptr<AskpassPromptRouter> router,
                             QString executablePath,
                             core::platform::IClock const& clock,
                             AskpassBrokerConfig config):
    _router { std::move(router) },
    _executablePath { std::move(executablePath) },
    _clock { clock },
    _config { std::move(config) },
    _worker { std::make_unique<QObject>() }
{
    _thread.setObjectName(QStringLiteral("askpass-broker"));
    _worker->moveToThread(&_thread);
    _thread.start();
    _state = _thread.isRunning() ? State::Serving : State::NotStarted;
}

AskpassBroker::AskpassBroker(IAskpassPromptSink& sink,
                             QString executablePath,
                             core::platform::IClock const& clock,
                             AskpassBrokerConfig config):
    AskpassBroker(
        std::make_shared<AskpassPromptRouter>(), std::move(executablePath), clock, std::move(config))
{
    _router->attach(AnyWindow, sink);
}

void AskpassBroker::shutdown()
{
    _state = State::ShutDown;
    _router->shutdown();
}

AskpassBroker::~AskpassBroker()
{
    // Every session is gone by now (they are destroyed before the broker), so nothing is left on the thread.
    _thread.quit();
    _thread.wait();
}

std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> AskpassBroker::open(
    vthost::SshTarget const& target)
{
    return open(target, AnyWindow);
}

std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> AskpassBroker::open(
    vthost::SshTarget const& target, AskpassRouteId route)
{
    switch (_state.load())
    {
        case State::Serving: break;
        case State::ShutDown:
            return std::unexpected { std::string { "the application no longer answers ssh's prompts" } };
        case State::NotStarted:
            return std::unexpected { std::string {
                "cannot serve ssh's prompts: the broker's thread did not start" } };
    }
    auto state = std::make_shared<SessionState>();
    auto nonce = randomHex(32).toStdString();
    auto const name = QString::fromLatin1("contour-askpass-") + QString::fromLatin1(randomHex(8));
    auto const targetText = QString::fromStdString(vthost::describeSshTarget(target));

    // On the broker's thread, which waits for nobody. The server listens before ssh is even started.
    auto listening = std::expected<QString, std::string> { std::unexpected { std::string {} } };
    QMetaObject::invokeMethod(
        _worker.get(),
        [&] {
            auto server =
                std::make_unique<Endpoint>(Hub { _router, route, this, _worker.get(), _lastPromptId, _clock },
                                           state,
                                           nonce,
                                           targetText,
                                           _config);
            listening = server->listen(name);
            if (listening)
            {
                auto const lock = std::lock_guard { state->mutex };
                state->endpoint = server.release();
            }
        },
        Qt::BlockingQueuedConnection);
    if (!listening)
        return std::unexpected { "cannot listen for ssh's prompts: " + listening.error() };

    auto environment =
        vthost::SpawnEnvironment { .set = {
                                       { "SSH_ASKPASS", _executablePath.toStdString() },
                                       { "SSH_ASKPASS_REQUIRE", "force" },
                                       { std::string { AskpassEndpointVariable }, listening->toStdString() },
                                       { std::string { AskpassNonceVariable }, std::move(nonce) },
                                   } };
    askpassLog()("askpass: opened a session for {}", vthost::describeSshTarget(target));
    return std::make_unique<Session>(std::move(state), _worker.get(), _clock, std::move(environment));
}

} // namespace contour::askpass
