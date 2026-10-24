// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How many clients a daemon serves right now. A client is a connection that is shown the daemon's sessions:
/// an attached native client, a tmux control-mode or an imsg client. A Control connection, which only asks
/// the daemon about itself, is no client, so a status probe never keeps an idle daemon alive.

#include <cstddef>
#include <utility>
#include <vector>

namespace vthost
{

/// Told of every change in a ClientCensus's count.
class CensusEvents
{
  public:
    virtual ~CensusEvents() = default;

    /// The census counts @p count clients now: one more or one fewer than at the last call.
    /// @param count The new count.
    virtual void clientCountChanged(std::size_t count) = 0;
};

/// Counts the clients a daemon serves, by the tickets it hands out. Loop-thread only.
///
/// The census outlives every ticket it issued, and every subscriber stays alive while subscribed. A
/// subscriber neither subscribes nor unsubscribes from within its callback.
class ClientCensus
{
  public:
    /// One counted client, for as long as it lives. Move-only; a default-constructed or moved-from ticket
    /// holds nothing and releases nothing.
    class Ticket
    {
      public:
        Ticket() = default;
        Ticket(Ticket const&) = delete;
        Ticket& operator=(Ticket const&) = delete;
        Ticket(Ticket&& other) noexcept: _census { std::exchange(other._census, nullptr) } {}
        Ticket& operator=(Ticket&& other) noexcept
        {
            if (this != &other)
            {
                release();
                _census = std::exchange(other._census, nullptr);
            }
            return *this;
        }
        ~Ticket() { release(); }

      private:
        friend class ClientCensus;
        explicit Ticket(ClientCensus& census) noexcept: _census { &census } {}

        void release() noexcept
        {
            if (auto* census = std::exchange(_census, nullptr))
                census->change(census->_count - 1);
        }

        ClientCensus* _census = nullptr; ///< The census that counts this ticket; null when it holds nothing.
    };

    ClientCensus() = default;
    ClientCensus(ClientCensus const&) = delete;
    ClientCensus& operator=(ClientCensus const&) = delete;
    ClientCensus(ClientCensus&&) = delete; // the tickets it issued point at it
    ClientCensus& operator=(ClientCensus&&) = delete;
    ~ClientCensus() = default;

    /// Counts one more client, until the returned ticket is destroyed.
    /// @return The ticket that holds the count.
    [[nodiscard]] Ticket admit()
    {
        change(_count + 1);
        return Ticket { *this };
    }

    /// @return How many clients hold a ticket now.
    [[nodiscard]] std::size_t count() const noexcept { return _count; }

    /// Tells @p observer of every change from now on.
    /// @param observer Not owned; must stay alive until unsubscribed.
    void subscribe(CensusEvents* observer) { _subscribers.push_back(observer); }

    /// Stops telling @p observer; harmless when it is not subscribed.
    /// @param observer A subscriber, or not.
    void unsubscribe(CensusEvents* observer) { std::erase(_subscribers, observer); }

  private:
    void change(std::size_t count) noexcept
    {
        _count = count;
        for (auto* observer: _subscribers)
            observer->clientCountChanged(count);
    }

    std::size_t _count = 0;
    std::vector<CensusEvents*> _subscribers;
};

} // namespace vthost
