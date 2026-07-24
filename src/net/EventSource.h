// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The blocking multiplexed wait the event loop drives, abstracted behind an
/// interface so the loop can be unit-tested with a scripted source instead of
/// real file descriptors.
///
/// The source is a *registry*: arbitrary file descriptors are attached so a
/// coroutine can `co_await` readiness on a pipe, PTY, or socket. Each `wait()`
/// reports which registered fds became ready via the outcome's @c readyRead /
/// @c readyWrite token lists.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <net/platform/NativeHandle.h>

namespace net
{

/// Readiness interest for a registered file descriptor. A bit set so a single
/// registration can watch readability and writability together.
enum class FdInterest : std::uint8_t
{
    None = 0,         ///< Watch nothing (mute the fd without detaching it).
    Read = 1U << 0U,  ///< Watch for readability (data available / EOF / HUP).
    Write = 1U << 1U, ///< Watch for writability (space available in the send buffer).
};

/// @return The union of two interest masks.
[[nodiscard]] constexpr FdInterest operator|(FdInterest a, FdInterest b) noexcept
{
    return static_cast<FdInterest>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

/// @param set The mask to test.
/// @param bit The single interest bit to check for.
/// @return True if @p bit is present in @p set.
[[nodiscard]] constexpr bool hasInterest(FdInterest set, FdInterest bit) noexcept
{
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(bit)) != 0;
}

/// Opaque token naming one fd registration. Returned by @c EventSource::attach
/// and passed back to @c detach. Stable for the registration's lifetime; a value
/// of 0 (the default) signals an invalid / failed registration.
///
/// A strong struct rather than an `enum class` because it is an opaque,
/// monotonically-allocated handle id (a wide value space that never wraps in a
/// session), not an enumeration of named cases.
struct FdToken
{
    std::uint64_t value = 0; ///< Registration id; 0 means invalid.

    /// @return True if two tokens name the same registration.
    [[nodiscard]] friend constexpr bool operator==(FdToken, FdToken) noexcept = default;

    /// @return True if this token names a live registration (non-zero).
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return value != 0; }

    /// Sentinel for an invalid / failed registration.
    [[nodiscard]] static constexpr FdToken invalid() noexcept { return FdToken { 0 }; }
};

/// What a single multiplexed wait observed: the tokens of the registered fds
/// that became ready. (Endo's upstream WaitOutcome also carries decoded terminal
/// input and interrupt flags; this port is a pure fd reactor, so those are gone
/// — cross-thread wakeup is EventLoop::post's self-pipe, an ordinary Read fd.)
struct WaitOutcome
{
    std::vector<FdToken> readyRead;  ///< Registered fds that became readable this wait.
    std::vector<FdToken> readyWrite; ///< Registered fds that became writable this wait.
};

/// One user-registered fd and its current readiness interest.
struct FdRegistration
{
    FdToken token {};                       ///< Identity returned to the caller.
    NativeHandle fd = InvalidHandle;        ///< The watched native handle.
    FdInterest interest = FdInterest::None; ///< Current readiness interest.
};

/// The shared fd-registration list every concrete @c EventSource embeds. Owns the
/// token allocation and the registration vector so the registry mechanism lives in
/// one place; each source's platform-specific @c wait() folds @c registrations()
/// into its native wait set and routes readiness back by token.
class FdRegistry
{
  public:
    /// Registers @p fd with @p interest.
    /// @param fd The native handle to watch (not owned).
    /// @param interest The readiness bits to start watching.
    /// @return A token naming the registration, or @c FdToken::invalid() on failure.
    [[nodiscard]] FdToken attach(NativeHandle fd, FdInterest interest)
    {
        if (fd == InvalidHandle)
            return FdToken::invalid();
        auto const token = FdToken { ++_nextToken };
        _registrations.push_back(FdRegistration { .token = token, .fd = fd, .interest = interest });
        return token;
    }

    /// Removes a registration. Idempotent.
    /// @param token The registration to drop.
    void detach(FdToken token)
    {
        std::erase_if(_registrations, [token](FdRegistration const& reg) { return reg.token == token; });
    }

    /// @return The current registrations, in registration order.
    [[nodiscard]] std::vector<FdRegistration> const& registrations() const noexcept { return _registrations; }

    /// @return The number of fds currently registered.
    [[nodiscard]] std::size_t size() const noexcept { return _registrations.size(); }

  private:
    std::vector<FdRegistration> _registrations; ///< Watched fds, in registration order.
    std::uint64_t _nextToken = 0;               ///< Source of never-zero registration tokens.
};

/// Abstraction over "block until something happens, with a timeout".
///
/// The production implementation (@c PollEventSource) multiplexes the registered
/// fds via poll(2) / WaitForMultipleObjects; tests inject a scripted source.
/// This is the single dependency-injection seam between the event loop and the
/// OS: keeping it an interface lets @c EventLoop be exercised deterministically.
class EventSource
{
  public:
    EventSource() = default;
    virtual ~EventSource() = default;

    EventSource(EventSource const&) = delete;
    EventSource& operator=(EventSource const&) = delete;
    EventSource(EventSource&&) = delete;
    EventSource& operator=(EventSource&&) = delete;

    /// Blocks until a registered fd becomes ready or the timeout elapses.
    /// @param timeoutMs -1 to block indefinitely, 0 to poll, >0 to wait that many ms.
    /// @return What was observed during the wait.
    [[nodiscard]] virtual WaitOutcome wait(int timeoutMs) = 0;

    /// Registers @p fd for multiplexing in subsequent waits.
    /// @param fd The native handle to watch (not owned; the caller keeps it valid
    ///        and detaches before closing it).
    /// @param interest The readiness bits to start watching.
    /// @return A token naming the registration, or @c FdToken::invalid() on failure.
    [[nodiscard]] virtual FdToken attach(NativeHandle fd, FdInterest interest) = 0;

    /// Removes a registration. Idempotent; a no-op for an unknown or invalid token.
    /// @param token The registration to drop.
    virtual void detach(FdToken token) = 0;
};

} // namespace net

namespace std
{

/// Hash specialization so @c FdToken can key an unordered container (the event
/// loop maps tokens to the coroutines parked on them).
template <>
struct hash<net::FdToken>
{
    /// @param token The token to hash.
    /// @return The hash of the token's underlying id.
    [[nodiscard]] std::size_t operator()(net::FdToken token) const noexcept
    {
        return std::hash<std::uint64_t> {}(token.value);
    }
};

} // namespace std
