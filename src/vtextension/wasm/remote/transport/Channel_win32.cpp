// SPDX-License-Identifier: Apache-2.0
//
// The channel on Windows: the two ends of a byte-mode named pipe, opened for overlapped I/O and waited on, so
// that shutdown() can cancel a read or write blocked on another thread. The pipe's name is random, the first
// instance is demanded (so that a pipe someone else created first fails the pair rather than joins it),
// remote clients are refused, and neither handle is inheritable.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <utility>

#include <vtextension/wasm/remote/transport/Channel.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// How large the pipe's buffers are, each way.
    constexpr auto PipeBufferBytes = DWORD { 64 * 1024 };

    /// The most bytes one ReadFile or WriteFile moves.
    constexpr auto MaxTransferBytes = std::size_t { 1024 } * 1024 * 1024;

    /// Whether an end was shut down.
    enum class EndState : uint8_t
    {
        Open,
        ShutDown,
    };

    /// An event an overlapped operation signals, closed with it.
    class OverlappedEvent
    {
      public:
        OverlappedEvent() noexcept: _event { ::CreateEventW(nullptr, TRUE, FALSE, nullptr) } {}
        ~OverlappedEvent()
        {
            if (_event)
                ::CloseHandle(_event);
        }
        OverlappedEvent(OverlappedEvent const&) = delete;
        OverlappedEvent& operator=(OverlappedEvent const&) = delete;
        OverlappedEvent(OverlappedEvent&&) = delete;
        OverlappedEvent& operator=(OverlappedEvent&&) = delete;

        /// @return The event; null if it could not be created.
        [[nodiscard]] HANDLE handle() const noexcept { return _event; }

      private:
        HANDLE _event;
    };

    /// @return Why a call that set @p error failed, for the channel.
    [[nodiscard]] ChannelError channelErrorOf(DWORD error) noexcept
    {
        switch (error)
        {
            case ERROR_BROKEN_PIPE:
            case ERROR_NO_DATA:
            case ERROR_PIPE_NOT_CONNECTED:
            case ERROR_OPERATION_ABORTED: return ChannelError::Closed;
            default: return ChannelError::Io;
        }
    }

    /// @return A pipe name no other pair uses: this process, a serial and 64 random bits.
    [[nodiscard]] std::wstring uniquePipeName()
    {
        static auto serial = std::atomic<uint64_t> { 0 };
        auto source = std::random_device {};
        auto const random = (static_cast<uint64_t>(source()) << 32) | source();
        return std::format(L"\\\\.\\pipe\\contour-extension-host-{}-{}-{:016x}",
                           ::GetCurrentProcessId(),
                           serial.fetch_add(1),
                           random);
    }

    /// Closes a handle that is not owned by a channel yet.
    struct HandleCloser
    {
        void operator()(HANDLE handle) const noexcept { ::CloseHandle(handle); }
    };
    using OwnedHandle = std::unique_ptr<void, HandleCloser>;
} // namespace

/// One end of the pipe, closed when it is destroyed.
struct Channel::Native
{
    HANDLE pipe;
    std::atomic<EndState> state { EndState::Open };

    explicit Native(HANDLE pipe) noexcept: pipe { pipe } {}
    ~Native() { ::CloseHandle(pipe); }
    Native(Native const&) = delete;
    Native& operator=(Native const&) = delete;
    Native(Native&&) = delete;
    Native& operator=(Native&&) = delete;

    /// Runs one overlapped transfer to completion, or until the end is shut down.
    /// @param start Starts it: ReadFile or WriteFile on the pipe with the given OVERLAPPED.
    /// @return How many bytes it moved.
    template <typename Start>
    [[nodiscard]] std::expected<std::size_t, ChannelError> transfer(Start start)
    {
        if (state.load() == EndState::ShutDown)
            return std::unexpected(ChannelError::Closed);
        auto const event = OverlappedEvent {};
        if (!event.handle())
            return std::unexpected(ChannelError::Io);
        auto overlapped = OVERLAPPED {};
        overlapped.hEvent = event.handle();
        if (!start(overlapped))
        {
            auto const error = ::GetLastError();
            if (error != ERROR_IO_PENDING)
                return std::unexpected(channelErrorOf(error));
        }
        // shutdown() sets the state before it cancels: a transfer started before the cancel is cancelled by
        // it, and one started after sees the state here and cancels itself.
        if (state.load() == EndState::ShutDown)
            ::CancelIoEx(pipe, &overlapped);
        auto moved = DWORD { 0 };
        if (!::GetOverlappedResult(pipe, &overlapped, &moved, TRUE))
            return std::unexpected(channelErrorOf(::GetLastError()));
        if (state.load() == EndState::ShutDown)
            return std::unexpected(ChannelError::Closed);
        return static_cast<std::size_t>(moved);
    }
};

Channel::Channel(std::unique_ptr<Native> native) noexcept: _native { std::move(native) }
{
}

Channel::~Channel() = default;
Channel::Channel(Channel&&) noexcept = default;
Channel& Channel::operator=(Channel&&) noexcept = default;

std::expected<void, ChannelError> Channel::send(std::span<uint8_t const> bytes)
{
    while (!bytes.empty())
    {
        auto const chunk = bytes.first(std::min(bytes.size(), MaxTransferBytes));
        auto const sent = _native->transfer([&](OVERLAPPED& overlapped) {
            return ::WriteFile(
                _native->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, &overlapped);
        });
        if (!sent)
            return std::unexpected(sent.error());
        bytes = bytes.subspan(*sent);
    }
    return {};
}

std::expected<std::size_t, ChannelError> Channel::receiveSome(std::span<uint8_t> buffer)
{
    auto const chunk = buffer.first(std::min(buffer.size(), MaxTransferBytes));
    auto const received = _native->transfer([&](OVERLAPPED& overlapped) {
        return ::ReadFile(
            _native->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, &overlapped);
    });
    if (received && *received == 0)
        return std::unexpected(ChannelError::Closed);
    return received;
}

void Channel::shutdown() noexcept
{
    if (!_native)
        return;
    _native->state.store(EndState::ShutDown);
    ::CancelIoEx(_native->pipe, nullptr);
}

NativeHandle Channel::nativeHandle() const noexcept
{
    return _native ? _native->pipe : INVALID_HANDLE_VALUE;
}

std::expected<ChannelPair, ChannelError> makeChannelPair()
{
    auto const name = uniquePipeName();
    auto server = OwnedHandle { ::CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        PipeBufferBytes,
        PipeBufferBytes,
        0,
        nullptr) };
    if (server.get() == INVALID_HANDLE_VALUE)
    {
        static_cast<void>(server.release());
        return std::unexpected(ChannelError::Io);
    }
    auto client = OwnedHandle { ::CreateFileW(name.c_str(),
                                              GENERIC_READ | GENERIC_WRITE,
                                              0,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_FLAG_OVERLAPPED,
                                              nullptr) };
    if (client.get() == INVALID_HANDLE_VALUE)
    {
        static_cast<void>(client.release());
        return std::unexpected(ChannelError::Io);
    }
    // The client connected when it opened the pipe; the server's connect only confirms it.
    auto const event = OverlappedEvent {};
    if (!event.handle())
        return std::unexpected(ChannelError::Io);
    auto overlapped = OVERLAPPED {};
    overlapped.hEvent = event.handle();
    if (!::ConnectNamedPipe(server.get(), &overlapped))
    {
        auto const error = ::GetLastError();
        auto connected = DWORD { 0 };
        if (error == ERROR_IO_PENDING)
        {
            if (!::GetOverlappedResult(server.get(), &overlapped, &connected, TRUE))
                return std::unexpected(ChannelError::Io);
        }
        else if (error != ERROR_PIPE_CONNECTED)
            return std::unexpected(ChannelError::Io);
    }
    return ChannelPair { .parent = Channel { std::make_unique<Channel::Native>(server.release()) },
                         .helper = Channel { std::make_unique<Channel::Native>(client.release()) } };
}

} // namespace vtextension::wasm::remote::transport
