// SPDX-License-Identifier: Apache-2.0
//
// The channel on Windows: the two ends of a byte-mode named pipe, opened for overlapped I/O and waited on, so
// that shutdown() can cancel a read or write blocked on another thread. The pipe's name is random, the first
// instance is demanded (so that a pipe someone else created first fails the pair rather than joins it),
// remote clients are refused, only the current user may open it, and neither handle is inheritable.
//
// A pipe has no half-close: its peer's stream ends only when this end's handle is closed. So shutdown()
// cancels what is in flight and closes the handle as soon as the last transfer on it has returned. Every
// transfer, and shutdown()'s own cancel, is counted while it may touch the handle, each counting itself
// before it reads or changes the state; whichever finds the end shut down with nothing in flight closes the
// handle, exactly once (a compare-exchange from ShutDown to Closed). All of it is sequentially consistent.
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
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// How large the pipe's buffers are, each way.
    constexpr auto PipeBufferBytes = DWORD { 64 * 1024 };

    /// The most bytes one ReadFile or WriteFile moves.
    constexpr auto MaxTransferBytes = std::size_t { 1024 } * 1024 * 1024;

    /// How long a transfer may take.
    enum class Completion : uint8_t
    {
        Poll, ///< Only what completes at once: a transfer still pending is cancelled.
        Wait, ///< Until it completed, or the end was shut down.
    };

    /// Where an end is in its life.
    enum class EndState : uint8_t
    {
        Open,     ///< Transfers run.
        ShutDown, ///< Transfers fail `Closed`; the handle is closed once none is in flight.
        Closed,   ///< The handle is closed.
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
        auto const high = static_cast<uint64_t>(source());
        auto const random = (high << 32) | source();
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

    /// @return @p handle owned; nothing if it is INVALID_HANDLE_VALUE, as a failed create returns.
    [[nodiscard]] OwnedHandle own(HANDLE handle) noexcept
    {
        return OwnedHandle { handle == INVALID_HANDLE_VALUE ? nullptr : handle };
    }

    /// Security attributes that let only the current user open the pipe, and no child inherit it. Not
    /// movable: the attributes point into the descriptor, and the descriptor into the ACL.
    class OwnerOnlySecurity
    {
        struct Key
        {
        };

      public:
        /// Builds a DACL with one entry, granting the process's user full access.
        /// @return The attributes; `Io` if the token could not be read or the ACL not made.
        [[nodiscard]] static std::expected<std::unique_ptr<OwnerOnlySecurity>, ChannelError> create()
        {
            auto security = std::make_unique<OwnerOnlySecurity>(Key {});
            auto token = HANDLE { nullptr };
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
                return std::unexpected(ChannelError::Io);
            auto const tokenOwner = OwnedHandle { token };
            auto size = DWORD { 0 };
            ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            if (size == 0)
                return std::unexpected(ChannelError::Io);
            // uint64_t storage, so that the TOKEN_USER and the ACL are aligned as their headers need.
            auto tokenUser = std::vector<uint64_t>((size + sizeof(uint64_t) - 1) / sizeof(uint64_t));
            if (!::GetTokenInformation(token, TokenUser, tokenUser.data(), size, &size))
                return std::unexpected(ChannelError::Io);
            auto* const user = reinterpret_cast<TOKEN_USER const*>(tokenUser.data())->User.Sid;

            auto const aclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + ::GetLengthSid(user);
            security->_acl.resize((aclBytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
            auto* const acl = reinterpret_cast<ACL*>(security->_acl.data());
            // The ACE copies the SID, so the token's buffer may go once it is added.
            if (!::InitializeAcl(
                    acl, static_cast<DWORD>(security->_acl.size() * sizeof(uint64_t)), ACL_REVISION)
                || !::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, user)
                || !::InitializeSecurityDescriptor(&security->_descriptor, SECURITY_DESCRIPTOR_REVISION)
                || !::SetSecurityDescriptorDacl(&security->_descriptor, TRUE, acl, FALSE))
                return std::unexpected(ChannelError::Io);
            security->_attributes = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                                          .lpSecurityDescriptor = &security->_descriptor,
                                                          .bInheritHandle = FALSE };
            return security;
        }

        /// Constructs empty attributes; only create() can, and it fills them before handing them out.
        explicit OwnerOnlySecurity(Key /*key*/) noexcept {}
        OwnerOnlySecurity(OwnerOnlySecurity const&) = delete;
        OwnerOnlySecurity& operator=(OwnerOnlySecurity const&) = delete;
        OwnerOnlySecurity(OwnerOnlySecurity&&) = delete;
        OwnerOnlySecurity& operator=(OwnerOnlySecurity&&) = delete;
        ~OwnerOnlySecurity() = default;

        /// @return The attributes to create both ends with.
        [[nodiscard]] SECURITY_ATTRIBUTES* attributes() noexcept { return &_attributes; }

      private:
        std::vector<uint64_t> _acl;
        SECURITY_DESCRIPTOR _descriptor {};
        SECURITY_ATTRIBUTES _attributes {};
    };
} // namespace

/// One end of the pipe: closed by shutdown() once it drained, or else when it is destroyed.
struct Channel::Native
{
    HANDLE pipe;
    std::atomic<EndState> state { EndState::Open };
    /// Who may touch the handle: every transfer in flight, and every shutdown() from before its state change
    /// until after its cancel, so that no close slips in ahead of that cancel.
    std::atomic<uint32_t> inFlight { 0 };

    explicit Native(HANDLE pipe) noexcept: pipe { pipe } {}
    ~Native()
    {
        if (state.load() != EndState::Closed)
            ::CloseHandle(pipe);
    }
    Native(Native const&) = delete;
    Native& operator=(Native const&) = delete;
    Native(Native&&) = delete;
    Native& operator=(Native&&) = delete;

    /// Closes the handle if the end is shut down and nothing is in flight; whoever gets here first does.
    void closeIfDrained() noexcept
    {
        if (inFlight.load() != 0)
            return;
        auto expected = EndState::ShutDown;
        if (state.compare_exchange_strong(expected, EndState::Closed))
            ::CloseHandle(pipe);
    }

    /// Runs one overlapped transfer to completion, or until the end is shut down; or, polled, only as far as
    /// it completes at once.
    /// @param start Starts it: ReadFile or WriteFile on the pipe with the given OVERLAPPED.
    /// @param completion Whether to wait for it.
    /// @return How many bytes it moved.
    template <typename Start>
    [[nodiscard]] std::expected<std::size_t, ChannelError> transfer(Start start, Completion completion)
    {
        // Counted before the state is read, and shutdown() stores the state before it reads the count: either
        // this sees the end shut down, or shutdown() sees this in flight and leaves the handle to it.
        inFlight.fetch_add(1);
        auto const result = transferCounted(start, completion);
        inFlight.fetch_sub(1);
        if (state.load() != EndState::Open)
            closeIfDrained();
        return result;
    }

  private:
    template <typename Start>
    [[nodiscard]] std::expected<std::size_t, ChannelError> transferCounted(Start start, Completion completion)
    {
        if (state.load() != EndState::Open)
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
        if (state.load() != EndState::Open)
            ::CancelIoEx(pipe, &overlapped);
        auto moved = DWORD { 0 };
        if (!::GetOverlappedResult(pipe, &overlapped, &moved, completion == Completion::Wait ? TRUE : FALSE))
        {
            auto const error = ::GetLastError();
            if (error != ERROR_IO_INCOMPLETE)
                return std::unexpected(channelErrorOf(error));
            // Polled and still pending: what does not fit now is not sent. Cancelled, the transfer reports
            // how much of it the reader took meanwhile, from the front.
            ::CancelIoEx(pipe, &overlapped);
            if (!::GetOverlappedResult(pipe, &overlapped, &moved, TRUE))
            {
                auto const cancelled = ::GetLastError();
                if (cancelled != ERROR_OPERATION_ABORTED)
                    return std::unexpected(channelErrorOf(cancelled));
            }
        }
        if (state.load() != EndState::Open)
            return std::unexpected(ChannelError::Closed);
        return static_cast<std::size_t>(moved);
    }
};

Channel::Channel(std::unique_ptr<Native> native) noexcept: _native { std::move(native) }
{
}

std::expected<Channel, ChannelError> Channel::adopt(NativeHandle handle)
{
    // GetFileType() reports FILE_TYPE_UNKNOWN, with an error, for a handle that is not open.
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || ::GetFileType(handle) != FILE_TYPE_PIPE)
        return std::unexpected(ChannelError::Io);
    if (!::SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0))
        return std::unexpected(ChannelError::Io);
    return Channel { std::make_unique<Native>(handle) };
}

Channel::~Channel() = default;
Channel::Channel(Channel&&) noexcept = default;
Channel& Channel::operator=(Channel&&) noexcept = default;

std::expected<void, ChannelError> Channel::send(std::span<uint8_t const> bytes)
{
    while (!bytes.empty())
    {
        auto const chunk = bytes.first(std::min(bytes.size(), MaxTransferBytes));
        auto const sent = _native->transfer(
            [&](OVERLAPPED& overlapped) {
                return ::WriteFile(
                    _native->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, &overlapped);
            },
            Completion::Wait);
        if (!sent)
            return std::unexpected(sent.error());
        bytes = bytes.subspan(*sent);
    }
    return {};
}

std::expected<std::size_t, ChannelError> Channel::trySend(std::span<uint8_t const> bytes)
{
    if (bytes.empty())
        return 0;
    auto const chunk = bytes.first(std::min(bytes.size(), MaxTransferBytes));
    return _native->transfer(
        [&](OVERLAPPED& overlapped) {
            return ::WriteFile(
                _native->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, &overlapped);
        },
        Completion::Poll);
}

std::expected<std::size_t, ChannelError> Channel::receiveSome(std::span<uint8_t> buffer)
{
    auto const chunk = buffer.first(std::min(buffer.size(), MaxTransferBytes));
    auto const received = _native->transfer(
        [&](OVERLAPPED& overlapped) {
            return ::ReadFile(
                _native->pipe, chunk.data(), static_cast<DWORD>(chunk.size()), nullptr, &overlapped);
        },
        Completion::Wait);
    if (received && *received == 0)
        return std::unexpected(ChannelError::Closed);
    return received;
}

void Channel::shutdown() noexcept
{
    if (!_native)
        return;
    // Counted, as a transfer is, before the state changes: a closer acts only once it sees the end shut down,
    // which is after this count is visible, so it finds the count non-zero until the cancel is done. Counting
    // after the state change would leave a window where a finishing transfer closes the handle, and the
    // cancel then reaches a closed handle whose value the system may already have handed out again.
    _native->inFlight.fetch_add(1);
    auto expected = EndState::Open;
    auto const shutsDown = _native->state.compare_exchange_strong(expected, EndState::ShutDown);
    if (shutsDown)
        ::CancelIoEx(_native->pipe, nullptr);
    _native->inFlight.fetch_sub(1);
    // Even a repeated shutdown() closes if it drained last: a transfer that finished while this one was
    // counted left the close to it.
    _native->closeIfDrained();
}

NativeHandle Channel::nativeHandle() const noexcept
{
    return _native && _native->state.load() != EndState::Closed ? _native->pipe : INVALID_HANDLE_VALUE;
}

std::expected<ChannelPair, ChannelError> makeChannelPair()
{
    auto const security = OwnerOnlySecurity::create();
    if (!security)
        return std::unexpected(security.error());

    auto const name = uniquePipeName();
    auto server =
        own(::CreateNamedPipeW(name.c_str(),
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                               1,
                               PipeBufferBytes,
                               PipeBufferBytes,
                               0,
                               (*security)->attributes()));
    if (!server)
        return std::unexpected(ChannelError::Io);
    auto client = own(::CreateFileW(name.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    0,
                                    (*security)->attributes(),
                                    OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED,
                                    nullptr));
    if (!client)
        return std::unexpected(ChannelError::Io);
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
    // No handle travels beside a pipe's bytes: the helper has nothing to send up.
    return ChannelPair { .parent = Channel { std::make_unique<Channel::Native>(server.release()) },
                         .helper = Channel { std::make_unique<Channel::Native>(client.release()) },
                         .upward = UpwardDescriptors::Refused };
}

void closeNativeHandle(NativeHandle handle) noexcept
{
    ::CloseHandle(handle);
}

} // namespace vtextension::wasm::remote::transport
