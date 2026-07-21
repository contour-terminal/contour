// SPDX-License-Identifier: Apache-2.0
#include <muxserver/imsg/ImsgCodec.h>

#include <cstring>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace muxserver::imsg
{

namespace
{
    void closeRaw(int fd)
    {
#ifndef _WIN32
        ::close(fd);
#else
        static_cast<void>(fd);
#endif
    }

    /// The on-wire header, exchanged verbatim in host byte order.
    struct WireHeader
    {
        uint32_t type = 0;
        uint32_t len = 0;
        uint32_t peerid = 0;
        uint32_t pid = 0;
    };
    static_assert(sizeof(WireHeader) == HeaderSize);
} // namespace

void UniqueFd::reset()
{
    if (_fd < 0)
        return;
    if (_closer)
        _closer(_fd);
    else
        closeRaw(_fd);
    _fd = -1;
}

std::vector<std::byte> encodeFrame(uint32_t type,
                                   std::span<std::byte const> payload,
                                   bool hasFd,
                                   uint32_t pid)
{
    auto header = WireHeader {};
    header.type = type;
    header.len = static_cast<uint32_t>(HeaderSize + payload.size());
    if (hasFd)
        header.len |= FdMark;
    header.peerid = ProtocolVersion;
    header.pid = pid;

    auto out = std::vector<std::byte>(HeaderSize + payload.size());
    std::memcpy(out.data(), &header, HeaderSize);
    // An empty payload (e.g. the MSG_VERSION mismatch reply) yields a null
    // data() pointer, and memcpy from null is UB even for a zero count.
    if (!payload.empty())
        std::memcpy(out.data() + HeaderSize, payload.data(), payload.size());
    return out;
}

ImsgDecoder::~ImsgDecoder()
{
    if (_pendingFd >= 0)
    {
        if (_closeFd)
            _closeFd(_pendingFd);
        else
            closeRaw(_pendingFd);
    }
}

void ImsgDecoder::feed(std::span<std::byte const> bytes, int fd)
{
    if (fd >= 0)
    {
        // A newly received descriptor replaces (and closes) an unclaimed one,
        // exactly as the rewritten imsg's receive path does.
        if (_pendingFd >= 0)
        {
            if (_closeFd)
                _closeFd(_pendingFd);
            else
                closeRaw(_pendingFd);
        }
        _pendingFd = fd;
    }

    if (_consumed > 0 && _consumed == _buffer.size())
    {
        _buffer.clear();
        _consumed = 0;
    }
    _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
}

std::expected<std::optional<ImsgFrame>, ImsgError> ImsgDecoder::next()
{
    auto const available = _buffer.size() - _consumed;
    if (available < HeaderSize)
        return std::nullopt;

    auto header = WireHeader {};
    std::memcpy(&header, _buffer.data() + _consumed, HeaderSize);

    auto const hasFd = (header.len & FdMark) != 0;
    auto const length = header.len & ~FdMark;
    if (length < HeaderSize || length > MaxMessageSize)
        return std::unexpected(ImsgError::LengthOutOfRange);
    if (available < length)
        return std::nullopt;

    auto frame = ImsgFrame {};
    frame.type = header.type;
    frame.peerid = header.peerid;
    frame.pid = header.pid;
    frame.payload.assign(_buffer.begin() + static_cast<std::ptrdiff_t>(_consumed + HeaderSize),
                         _buffer.begin() + static_cast<std::ptrdiff_t>(_consumed + length));
    if (hasFd && _pendingFd >= 0)
        frame.fd = UniqueFd { std::exchange(_pendingFd, -1), _closeFd };
    _consumed += length;
    return std::optional<ImsgFrame> { std::move(frame) };
}

} // namespace muxserver::imsg
