// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether the application object exists, for what must not run without it.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace contour::platform
{

/// Says whether the Qt application object exists, to whoever must not run without it. Any thread.
///
/// Qt runs no event loop on a thread of its own while no application object exists ("QEventLoop: Cannot be
/// used without QCoreApplication"): not before it is made, and not after it is gone. Yet `contour client`
/// starts its attach -- and with it ssh, whose prompts are served on such a thread -- while the GUI is still
/// booting, and its links outlive the application object on the way out. What starts such a thread watches
/// this, and starts it only while the object exists.
///
/// Qt-free: what it says is told by whoever makes and destroys the application object.
class ApplicationReadiness
{
  public:
    /// Where the application object stands. Pending goes to Ready or Abandoned, and Ready to Closed;
    /// Abandoned and Closed are final.
    enum class State : std::uint8_t
    {
        Pending = 0, ///< Not made yet.
        Ready,       ///< Made, and not destroyed: Qt may run event loops on threads of their own.
        Abandoned,   ///< It will not be made.
        Closed,      ///< It was made, and is being destroyed or is gone.
    };

    /// Told each state from subscribe() on, on the thread that settles it, under this object's lock: it must
    /// not call back into this object.
    using Observer = std::function<void(State)>;

    /// Ends an observer's subscription when destroyed; once that returns, the observer is not running and
    /// will not run again.
    class Subscription
    {
      public:
        Subscription(ApplicationReadiness& readiness, std::uint64_t id): _readiness { &readiness }, _id { id }
        {
        }
        ~Subscription()
        {
            if (_readiness != nullptr)
                _readiness->unsubscribe(_id);
        }
        Subscription(Subscription&& other) noexcept:
            _readiness { std::exchange(other._readiness, nullptr) }, _id { other._id }
        {
        }
        Subscription& operator=(Subscription&&) = delete;
        Subscription(Subscription const&) = delete;
        Subscription& operator=(Subscription const&) = delete;

      private:
        ApplicationReadiness* _readiness;
        std::uint64_t _id;
    };

    /// @param initial Where it stands: Ready where the application object exists already.
    explicit ApplicationReadiness(State initial): _state { initial } {}

    ApplicationReadiness(ApplicationReadiness const&) = delete;
    ApplicationReadiness& operator=(ApplicationReadiness const&) = delete;
    ApplicationReadiness(ApplicationReadiness&&) = delete;
    ApplicationReadiness& operator=(ApplicationReadiness&&) = delete;
    ~ApplicationReadiness() = default;

    /// Says the application object exists now. From Pending only; ignored otherwise.
    void markReady() { settle(State::Pending, State::Ready); }

    /// Says the application object will not be made. From Pending only, so that it may be said on every way
    /// out and counts only for those taken before the object was made.
    void abandon() { settle(State::Pending, State::Abandoned); }

    /// Says the application object is going: said as it is destroyed. From Ready only; ignored otherwise.
    void close() { settle(State::Ready, State::Closed); }

    /// @return Where it stands now.
    [[nodiscard]] State state() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _state;
    }

    /// Has @p observer told the current state at once, and every later one as it is settled.
    /// @param observer What is told; see Observer.
    /// @return What keeps it subscribed; it must not outlive this object.
    [[nodiscard]] Subscription subscribe(Observer observer)
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const id = _nextId++;
        observer(_state);
        _observers.emplace(id, std::move(observer));
        return Subscription { *this, id };
    }

  private:
    void settle(State from, State to)
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_state != from)
            return;
        _state = to;
        for (auto const& observer: _observers)
            observer.second(to);
    }

    void unsubscribe(std::uint64_t id)
    {
        auto const lock = std::scoped_lock { _mutex };
        _observers.erase(id);
    }

    mutable std::mutex _mutex;
    State _state;
    std::uint64_t _nextId = 0;
    std::map<std::uint64_t, Observer> _observers;
};

} // namespace contour::platform
