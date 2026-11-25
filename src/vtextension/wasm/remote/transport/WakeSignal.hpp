// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The wake signal: how Contour tells its extension host process that it raised an interrupt flag
/// (ControlPage.hpp). A one-byte-per-wake stream socket pair on POSIX, a manual-reset event on Windows.
///
/// Sending never blocks and never raises SIGPIPE, whatever the helper does: a full buffer only means that a
/// wake is queued already, and the helper scans the whole page on every wake.

#include <cstdint>
#include <expected>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::transport
{

struct WakePair;

/// What a signal() did.
enum class WakeResult : uint8_t
{
    Pending, ///< Nothing: a wake is queued already, which the receiver has not taken yet.
    Sent,    ///< A wake is queued now; or the receiver is gone, and there is nobody left to wake.
};

/// Why a wait() returned.
enum class WakeWait : uint8_t
{
    Stopped,  ///< stop() was called.
    PeerGone, ///< The sender is gone, and no wake is queued (POSIX; Windows cannot tell).
    Woken,    ///< At least one wake was queued, and every queued one is taken.
};

/// The parent's end. Move-only; closed when destroyed.
class WakeSender
{
  public:
    WakeSender(WakeSender&& other) noexcept;
    WakeSender& operator=(WakeSender&& other) noexcept;
    WakeSender(WakeSender const&) = delete;
    WakeSender& operator=(WakeSender const&) = delete;
    ~WakeSender();

    /// Queues a wake. Never blocks, never raises SIGPIPE; any thread.
    /// @return Whether a wake was queued, or one was queued already.
    [[nodiscard]] WakeResult signal() const noexcept;

    /// @return The end's handle, still owned by it.
    [[nodiscard]] NativeHandle nativeHandle() const noexcept;

  private:
    friend std::expected<WakePair, ChannelError> makeWakePair();
    explicit WakeSender(NativeHandle handle) noexcept;

    NativeHandle _handle;
};

/// The handles by which a WakeReceiver stops its own wait: on POSIX a pipe's two ends, on Windows one event,
/// which both name.
struct WakeStopHandles
{
    NativeHandle waitedOn;  ///< What wait() waits on beside the wake end.
    NativeHandle signalled; ///< What stop() signals.
};

/// The helper's end. Move-only; closed when destroyed.
class WakeReceiver
{
  public:
    /// Takes ownership of the end this process inherited, made close-on-exec (not inheritable) again.
    /// @param handle The end.
    /// @return The receiver; `Io` if @p handle is no such end, in which case it stays the caller's.
    [[nodiscard]] static std::expected<WakeReceiver, ChannelError> adopt(NativeHandle handle);

    WakeReceiver(WakeReceiver&& other) noexcept;
    WakeReceiver& operator=(WakeReceiver&& other) noexcept;
    WakeReceiver(WakeReceiver const&) = delete;
    WakeReceiver& operator=(WakeReceiver const&) = delete;
    ~WakeReceiver();

    /// Blocks until a wake is queued, the sender is gone, or stop() was called. One thread at a time.
    /// @return Why it returned; `Woken` only once every queued wake is taken. `Stopped` wins over the others.
    [[nodiscard]] WakeWait wait() noexcept;

    /// Ends a wait() blocked now, and every later one, with `Stopped`. Thread-safe and idempotent.
    void stop() const noexcept;

    /// @return The end's handle, still owned by it.
    [[nodiscard]] NativeHandle nativeHandle() const noexcept;

    /// @return The handles stop() works through, still owned by it: what a sandbox that limits handle by
    ///         handle must leave it.
    [[nodiscard]] WakeStopHandles stopHandles() const noexcept;

  private:
    friend std::expected<WakePair, ChannelError> makeWakePair();
#ifdef _WIN32
    WakeReceiver(NativeHandle handle, void* stopEvent) noexcept;
#else
    WakeReceiver(NativeHandle handle, int stopRead, int stopWrite) noexcept;
#endif

    /// Closes what this end holds, if anything.
    void release() noexcept;

    NativeHandle _handle;
#ifdef _WIN32
    void* _stopEvent; ///< A manual-reset event stop() sets.
#else
    int _stopRead;  ///< The stop pipe's read end, readable once stop() was called.
    int _stopWrite; ///< Its write end.
#endif
};

/// A connected sender and receiver.
struct WakePair
{
    WakeSender parent;   ///< Contour's end.
    WakeReceiver helper; ///< The extension host process's end, to hand over.
};

/// Creates a connected pair. Neither end is inherited by a child process unless it is handed over on
/// purpose.
/// @return The pair; `Io` if the system refused.
[[nodiscard]] std::expected<WakePair, ChannelError> makeWakePair();

} // namespace vtextension::wasm::remote::transport
