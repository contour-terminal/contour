// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The rewritten-libutil imsg wire codec tmux ≥ 3.6 speaks between its
/// client and server: 16-byte host-order header `{type, len, peerid, pid}`
/// where `len` INCLUDES the header, the top bit of `len` marks "one
/// SCM_RIGHTS fd accompanies this message", and the masked length is
/// bounded by [16, 16384]. `peerid`'s low 8 bits carry PROTOCOL_VERSION.
///
/// Pure bytes-in/frames-out — no sockets — so every rule is table-testable.
/// Host byte order is exchanged verbatim: imsg runs over same-machine
/// AF_UNIX only, exactly as tmux itself assumes.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace vthost::imsg
{

/// tmux's protocol version (tmux-protocol.h), carried in peerid's low byte.
inline constexpr uint32_t ProtocolVersion = 8;

/// The fd-present mark on the wire `len` field.
inline constexpr uint32_t FdMark = 0x8000'0000U;

/// The largest message including its header.
inline constexpr uint32_t MaxMessageSize = 16384;

/// The header size (and therefore the smallest valid `len`).
inline constexpr uint32_t HeaderSize = 16;

/// tmux's message types (tmux-protocol.h enum msgtype), by wire value.
/// Deliberately raw uint32_t constants, not an enum: an incoming type is an
/// arbitrary 32-bit wire value (newer tmux adds types like MSG_READ_OPEN
/// 300), and frames must carry it losslessly.
namespace msgtype
{
    inline constexpr uint32_t Version = 12;

    inline constexpr uint32_t IdentifyFlags = 100;
    inline constexpr uint32_t IdentifyTerm = 101;
    inline constexpr uint32_t IdentifyTtyName = 102;
    inline constexpr uint32_t IdentifyOldCwd = 103;
    inline constexpr uint32_t IdentifyStdin = 104;
    inline constexpr uint32_t IdentifyEnviron = 105;
    inline constexpr uint32_t IdentifyDone = 106;
    inline constexpr uint32_t IdentifyClientPid = 107;
    inline constexpr uint32_t IdentifyCwd = 108;
    inline constexpr uint32_t IdentifyFeatures = 109;
    inline constexpr uint32_t IdentifyStdout = 110;
    inline constexpr uint32_t IdentifyLongFlags = 111;
    inline constexpr uint32_t IdentifyTerminfo = 112;

    inline constexpr uint32_t Command = 200;
    inline constexpr uint32_t Detach = 201;
    inline constexpr uint32_t DetachKill = 202;
    inline constexpr uint32_t Exit = 203;
    inline constexpr uint32_t Exited = 204;
    inline constexpr uint32_t Exiting = 205;
    inline constexpr uint32_t Lock = 206;
    inline constexpr uint32_t Ready = 207;
    inline constexpr uint32_t Resize = 208;
    inline constexpr uint32_t Shell = 209;
    inline constexpr uint32_t Shutdown = 210;
    inline constexpr uint32_t Suspend = 214;
    inline constexpr uint32_t Unlock = 215;
    inline constexpr uint32_t Wakeup = 216;
    inline constexpr uint32_t Exec = 217;
    inline constexpr uint32_t Flags = 218;
} // namespace msgtype

/// A movable owner of one received descriptor (closes via the injected
/// closer, so tests can observe closures without dup tricks).
class UniqueFd
{
  public:
    using Closer = std::function<void(int)>;

    UniqueFd() = default;

    /// @param fd The owned descriptor (-1 = none).
    /// @param closer Invoked with the fd on destruction (empty = ::close).
    explicit UniqueFd(int fd, Closer closer = {}): _fd(fd), _closer(std::move(closer)) {}

    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept: _fd(std::exchange(other._fd, -1)), _closer(std::move(other._closer))
    {
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _fd = std::exchange(other._fd, -1);
            _closer = std::move(other._closer);
        }
        return *this;
    }

    UniqueFd(UniqueFd const&) = delete;
    UniqueFd& operator=(UniqueFd const&) = delete;

    [[nodiscard]] int get() const noexcept { return _fd; }
    [[nodiscard]] bool valid() const noexcept { return _fd >= 0; }

    /// Yields ownership of the descriptor to the caller.
    [[nodiscard]] int release() noexcept { return std::exchange(_fd, -1); }

    /// Closes the owned descriptor, if any.
    void reset();

  private:
    int _fd = -1;
    Closer _closer;
};

/// One decoded message.
struct ImsgFrame
{
    uint32_t type = 0;
    uint32_t peerid = 0;
    uint32_t pid = 0;
    std::vector<std::byte> payload;
    UniqueFd fd; ///< The SCM_RIGHTS descriptor claimed by this message, if any.
};

/// Why bytes could not be decoded (fatal for the connection).
enum class ImsgError : uint8_t
{
    LengthOutOfRange,  ///< Masked len outside [16, 16384].
    MalformedString,   ///< A C-string payload without its terminating NUL.
    WrongPayloadSize,  ///< A fixed-size payload of the wrong size.
    BadArgv,           ///< MSG_COMMAND argv failed cmd_unpack_argv's rules.
    IdentifyAfterDone, ///< An identify message after MSG_IDENTIFY_DONE.
};

/// Encodes one message: header (peerid stamped with ProtocolVersion, fd
/// mark per @p hasFd) followed by @p payload.
/// @param type The message type (a msgtype constant).
/// @param payload The payload bytes (excluding the header).
/// @param hasFd Whether one SCM_RIGHTS fd travels with the message.
/// @param pid The sender pid stamped into the header.
/// @return The wire bytes.
[[nodiscard]] std::vector<std::byte> encodeFrame(uint32_t type,
                                                 std::span<std::byte const> payload,
                                                 bool hasFd = false,
                                                 uint32_t pid = 0);

/// The incremental decoder: feed read chunks (each with at most one
/// received fd), pull complete frames. The pending fd is claimed by the
/// FIRST fd-marked header parsed after it arrived — the rewritten imsg's
/// receive rule; a marked header with no pending fd is tolerated (the
/// EMSGSIZE-lost-fd case). An fd still unclaimed when the next one arrives
/// is closed via the injected closer.
class ImsgDecoder
{
  public:
    /// @param closeFd Closes unclaimed descriptors (empty = ::close).
    explicit ImsgDecoder(UniqueFd::Closer closeFd = {}): _closeFd(std::move(closeFd)) {}

    ~ImsgDecoder();

    ImsgDecoder(ImsgDecoder const&) = delete;
    ImsgDecoder& operator=(ImsgDecoder const&) = delete;
    ImsgDecoder(ImsgDecoder&&) = delete;
    ImsgDecoder& operator=(ImsgDecoder&&) = delete;

    /// Appends @p bytes; a valid @p fd (≥ 0) becomes the pending descriptor
    /// (closing any previously pending, unclaimed one).
    void feed(std::span<std::byte const> bytes, int fd = -1);

    /// @return The next complete frame; std::nullopt when more bytes are
    ///         needed; an ImsgError when the stream is unrecoverable.
    [[nodiscard]] std::expected<std::optional<ImsgFrame>, ImsgError> next();

  private:
    std::vector<std::byte> _buffer;
    std::size_t _consumed = 0;
    int _pendingFd = -1;
    UniqueFd::Closer _closeFd;
};

} // namespace vthost::imsg
