// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The GUI side of askpass: one local server per ssh attempt, answering the helper processes that
/// ssh starts through `SSH_ASKPASS`.

#include <contour/askpass/AskpassPromptSink.hpp>

#include <core/platform/Clock.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QThread>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
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
/// The servers live on a thread of the broker's own, which runs an event loop of its own from the broker's
/// construction to its destruction and never waits for anyone: open() makes and starts a session's server
/// there before it returns, from whichever thread it is called on (so a failure to listen is its error, and
/// the transport runs without prompts), and a session closes its server there when destroyed on any thread.
/// All socket work (reading requests, the nonce check, the caps and timeouts, the writing of answers) runs
/// on that thread; only the sink is asked, and told of a helper's going, on the thread the broker was made
/// on, by queued calls. A helper that connects before that thread serves events waits for its answer, and so
/// does ssh.
class AskpassBroker final: public QObject, public vthost::IAskpassBroker
{
  public:
    /// @param sink Shows the prompts; borrowed, outliving every session. It is asked on the thread this
    /// object is
    ///        made on, which must run an event loop for the sink to be asked at all.
    /// @param executablePath The program ssh runs as its askpass helper: this application's own.
    /// @param clock Measures how long prompts stay open; borrowed.
    /// @param config How the servers are set up.
    AskpassBroker(IAskpassPromptSink& sink,
                  QString executablePath,
                  core::platform::IClock const& clock = core::platform::defaultSteadyClock(),
                  AskpassBrokerConfig config = {});
    ~AskpassBroker() override;

    AskpassBroker(AskpassBroker const&) = delete;
    AskpassBroker& operator=(AskpassBroker const&) = delete;
    AskpassBroker(AskpassBroker&&) = delete;
    AskpassBroker& operator=(AskpassBroker&&) = delete;

    /// @copydoc vthost::IAskpassBroker::open
    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        vthost::SshTarget const& target) override;

  private:
    IAskpassPromptSink& _sink;
    QString _executablePath;
    core::platform::IClock const& _clock;
    AskpassBrokerConfig _config;
    std::atomic<AskpassPromptId> _lastPromptId { 0 }; ///< The id of the latest prompt any session asked.
    QThread _thread;                                  ///< Serves the sessions' servers.
    /// Lives on _thread; the context the work for it is posted to. Declared after the thread, which is
    /// stopped and joined by the destructor's body, before it is destroyed.
    std::unique_ptr<QObject> _worker;
};

} // namespace contour::askpass
