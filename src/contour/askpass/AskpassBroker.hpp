// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The GUI side of askpass: one local server per ssh attempt, answering the helper processes that
/// ssh starts through `SSH_ASKPASS`.

#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/askpass/AskpassPromptSink.hpp>
#include <contour/platform/ApplicationReadiness.hpp>
#include <contour/platform/AsyncLatch.hpp>

#include <core/Random.hpp>
#include <core/platform/Clock.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QThread>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <vthost/Askpass.hpp>

namespace contour::askpass
{

/// How the broker's servers are set up, and what bounds them against connections that are not ssh's helpers.
struct AskpassBrokerConfig
{
    /// How many connections may be waiting to send their request at once; further ones are closed.
    std::size_t maxUnauthenticated = 4;
    /// How long a connection may take to send a whole, valid request before it is closed.
    std::chrono::milliseconds requestTimeout = std::chrono::seconds { 10 };
    /// Where the socket files go; empty: the user's runtime directory, else the temporary one. Not used under
    /// Windows, where the endpoint is a named pipe.
    QString socketDirectory;
};

/// Opens one askpass session for each ssh attempt.
///
/// A session owns a QLocalServer under a random name, reachable by the user alone, and a random nonce
/// that every request must repeat: a process that is not ssh's child of this attempt cannot make the
/// sink ask anything. The server closes when the session is destroyed.
///
/// The servers live on a thread of the broker's own, which runs an event loop of its own from the first
/// session to the broker's destruction and never waits for anyone: open() makes and starts a session's server
/// there before it returns, from whichever thread it is called on (so a failure to listen is its error, and
/// the transport runs without prompts), and a session closes its server there when destroyed on any thread.
///
/// The thread runs only while the application object exists: Qt runs no event loop without it, and
/// `contour client --host` makes the broker, and gets to its first session, while the GUI is still booting --
/// a thread started then warned "QEventLoop: Cannot be used without QCoreApplication" and on Linux crashed
/// the process now and again. So the thread is started by whoever says the object exists
/// (ApplicationReadiness::markReady(), on the GUI thread), or by the constructor where it exists already;
/// never by a session's loop. Once the object will not come, or is going, nothing starts it. whenServing()
/// waits for the outcome without holding up the awaiting loop; open() never waits.
/// All socket work (reading requests, the nonce check, the caps and timeouts, the writing of answers) runs
/// on that thread; only the sink is asked, and told of a helper's going, on the thread the broker was made
/// on, by queued calls. A helper that connects before that thread serves events waits for its answer, and so
/// does ssh.
class AskpassBroker final: public QObject, public vthost::IAskpassBroker
{
  public:
    /// @param router Finds the window that shows each prompt; shared, so that neither the broker nor a
    ///        window's sink has to outlive the other. Asked on the thread this object is made on, which must
    ///        run an event loop for any prompt to be asked.
    /// @param promptIds Hands out each prompt's id; shared with the prompts the application asks itself.
    /// @param application Whether the application object exists, which the broker's thread waits for; not
    ///        null. The thread is started on the thread that marks it ready, or here if it is ready already.
    /// @param executablePath The program ssh runs as its askpass helper: this application's own.
    /// @param random Where each session's nonce and endpoint name come from; borrowed. Called from whichever
    ///        thread open() is.
    /// @param clock Measures how long prompts stay open; borrowed.
    /// @param config How the servers are set up.
    AskpassBroker(std::shared_ptr<AskpassPromptRouter> router,
                  std::shared_ptr<AskpassPromptIds> promptIds,
                  std::shared_ptr<platform::ApplicationReadiness> application,
                  QString executablePath,
                  core::IRandom& random,
                  core::platform::IClock const& clock = core::platform::defaultSteadyClock(),
                  AskpassBrokerConfig config = {});

    /// A broker with one sink that shows every prompt, whichever window asked: the sink is attached to a
    /// router of the broker's own as AnyWindow; its prompt ids are its own too.
    /// @param sink Shows the prompts; borrowed, until the broker's shutdown() or destruction. May be gone
    ///        before the broker is, once shutdown() ran.
    /// @param application Whether the application object exists, which the broker's thread waits for; not
    ///        null.
    /// @param executablePath The program ssh runs as its askpass helper: this application's own.
    /// @param random Where each session's nonce and endpoint name come from; borrowed. Called from whichever
    ///        thread open() is.
    /// @param clock Measures how long prompts stay open; borrowed.
    /// @param config How the servers are set up.
    AskpassBroker(IAskpassPromptSink& sink,
                  std::shared_ptr<platform::ApplicationReadiness> application,
                  QString executablePath,
                  core::IRandom& random,
                  core::platform::IClock const& clock = core::platform::defaultSteadyClock(),
                  AskpassBrokerConfig config = {});
    ~AskpassBroker() override;

    AskpassBroker(AskpassBroker const&) = delete;
    AskpassBroker& operator=(AskpassBroker const&) = delete;
    AskpassBroker(AskpassBroker&&) = delete;
    AskpassBroker& operator=(AskpassBroker&&) = delete;

    /// Ends the service: later open() calls fail (so that ssh runs without prompts), and every open prompt is
    /// cancelled. GUI thread. Call before the windows' sinks go; the broker itself may outlive them, as the
    /// transport factories hold it too.
    void shutdown();

    /// @copydoc vthost::IAskpassBroker::whenServing
    /// Waits, without holding up the awaiting loop, until the broker's thread has started or never will.
    [[nodiscard]] core::async::Task<std::expected<void, std::string>> whenServing() override;

    /// @copydoc vthost::IAskpassBroker::open
    /// The prompts go to whichever window comes first (AnyWindow).
    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        vthost::SshTarget const& target) override;

    /// Opens a session whose prompts are shown in one window.
    /// @param target The host the attempt logs in to.
    /// @param route The window that started the attach.
    /// @return The session, or why it cannot be set up.
    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        vthost::SshTarget const& target, AskpassRouteId route);

  private:
    friend struct AskpassBrokerTestAccess;

    /// Where the broker's life stands. Every state but NotStarted and Serving is final, and only Serving
    /// serves.
    enum class State : std::uint8_t
    {
        NotStarted = 0, ///< The application object does not exist yet: nothing is served, nothing started.
        Serving,        ///< The thread runs: sessions are served.
        Failed,         ///< The thread did not start.
        Abandoned,      ///< The application object will not be made: the thread never starts.
        Closed,         ///< The application object is going: nothing more is served.
        ShutDown,       ///< shutdown() ran: nothing more is served.
    };

    /// Follows the application object: starts the thread once it exists, and ends the service once it will
    /// not come or is going. Runs on the thread that settles @p application's readiness, under its lock.
    void follow(platform::ApplicationReadiness::State application);

    /// Moves to the final state @p to, unless the broker is in one already, and tells the waiters.
    /// _lifeMutex held.
    void end(State to);

    /// @return Whether a session can be opened now, and if not, why.
    [[nodiscard]] std::expected<void, std::string> serving() const;

    std::shared_ptr<AskpassPromptRouter> _router;
    std::shared_ptr<AskpassPromptIds> _promptIds;
    std::shared_ptr<platform::ApplicationReadiness> _application;
    QString _executablePath;
    core::platform::IClock const& _clock;
    AskpassBrokerConfig _config;
    core::IRandom& _random;
    /// Held across every change of _state, the thread's start included, and by the destructor before it
    /// stops the thread: so a start is never half done when anyone else looks, or when the thread is stopped.
    std::mutex _lifeMutex;
    /// Written under _lifeMutex, read without it: open() and whenServing() never wait for a start.
    std::atomic<State> _state { State::NotStarted };
    /// Told once whether the broker serves: when its thread started, or it never will.
    platform::AsyncLatch<std::expected<void, std::string>> _outcome;
    /// Runs just before the thread is started, with _lifeMutex held: a test's seam for the interleavings.
    std::function<void()> _beforeThreadStart;
    QThread _thread; ///< Serves the sessions' servers.
    /// Lives on _thread; the context the work for it is posted to. Declared after the thread, which is
    /// stopped and joined by the destructor's body, before it is destroyed.
    std::unique_ptr<QObject> _worker;
    /// Follows _application; dropped first in the destructor, so that no start begins once it runs.
    std::optional<platform::ApplicationReadiness::Subscription> _followed;
};

/// The broker as one window sees it: every session it opens shows its prompts in that window.
class RoutedAskpassBroker final: public vthost::IAskpassBroker
{
  public:
    /// @param broker The application's broker; shared, as the transports that hold this one may outlive
    ///        everyone else.
    /// @param route The window that starts the attaches.
    RoutedAskpassBroker(std::shared_ptr<AskpassBroker> broker, AskpassRouteId route):
        _broker { std::move(broker) }, _route { route }
    {
    }

    /// @copydoc vthost::IAskpassBroker::whenServing
    [[nodiscard]] core::async::Task<std::expected<void, std::string>> whenServing() override
    {
        return _broker->whenServing();
    }

    /// @copydoc vthost::IAskpassBroker::open
    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        vthost::SshTarget const& target) override
    {
        return _broker->open(target, _route);
    }

  private:
    std::shared_ptr<AskpassBroker> _broker;
    AskpassRouteId _route;
};

} // namespace contour::askpass
