// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/Frame.hpp>

#include <core/codec/Stream.hpp>

#include <algorithm>
#include <iterator>
#include <ranges>

namespace vthost::keeper
{

std::vector<uint8_t> encodeFrame(Frame const& frame)
{
    auto writer = core::codec::Writer {};
    writer.varint(frame.body.size());
    writer.varint(frame.fds.size());
    writer.raw(frame.body);
    return std::move(writer).take();
}

void FrameAssembler::feed(std::span<uint8_t const> bytes, std::span<int const> fds)
{
    if (!fds.empty())
        _arrivals.push_back({ .offset = _buffer.size(), .fds = { fds.begin(), fds.end() } });
    _buffer.insert(_buffer.end(), bytes.begin(), bytes.end());
}

std::optional<Frame> FrameAssembler::next()
{
    if (_error)
        return std::nullopt;

    auto reader = core::codec::Reader { _buffer };
    auto const length = reader.varint();
    auto const fdCount = length ? reader.varint() : length;
    if (!length || !fdCount)
    {
        // A header that merely ends is more bytes to come; one that is overlong is a lie.
        auto const failure = !length ? length.error() : fdCount.error();
        if (failure != core::codec::DecodeError::Truncated)
            _error = FrameError::MalformedHeader;
        return std::nullopt;
    }
    if (*length > MaxFrameBodySize)
    {
        _error = FrameError::BodyTooLarge;
        return std::nullopt;
    }
    if (*fdCount > MaxFdsPerFrame)
    {
        _error = FrameError::TooManyDescriptors;
        return std::nullopt;
    }
    auto const headerSize = _buffer.size() - reader.remaining();
    auto const total = headerSize + static_cast<std::size_t>(*length);
    if (_buffer.size() < total)
        return std::nullopt;

    auto frame = Frame {};
    frame.body.assign(_buffer.begin() + static_cast<std::ptrdiff_t>(headerSize),
                      _buffer.begin() + static_cast<std::ptrdiff_t>(total));
    for (auto const& arrival: _arrivals | std::views::filter([&](auto const& a) { return a.offset < total; }))
        frame.fds.insert(frame.fds.end(), arrival.fds.begin(), arrival.fds.end());
    std::erase_if(_arrivals, [&](auto const& a) { return a.offset < total; });
    if (frame.fds.size() != *fdCount)
    {
        _error = FrameError::DescriptorMismatch;
        _arrivals.push_back({ .offset = 0, .fds = std::move(frame.fds) });
        return std::nullopt;
    }

    _buffer.erase(_buffer.begin(), _buffer.begin() + static_cast<std::ptrdiff_t>(total));
    for (auto& arrival: _arrivals)
        arrival.offset -= total;
    return frame;
}

std::vector<int> FrameAssembler::takePendingFds()
{
    auto pending = std::vector<int> {};
    for (auto& arrival: _arrivals)
        pending.insert(pending.end(), arrival.fds.begin(), arrival.fds.end());
    _arrivals.clear();
    return pending;
}

} // namespace vthost::keeper
