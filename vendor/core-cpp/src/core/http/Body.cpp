// SPDX-License-Identifier: Apache-2.0
#include <core/http/Body.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace core::http
{

BufferBodySource::BufferBodySource(std::string bytes, Fields trailers):
    _bytes { std::move(bytes) },
    _length { _bytes.size() },
    _trailers { std::move(trailers) },
    _trailerIntent { _trailers.entries().empty() ? TrailerIntent::None : TrailerIntent::Possible }
{
}

std::optional<std::uint64_t> BufferBodySource::knownLength() const noexcept
{
    return _length;
}

TrailerIntent BufferBodySource::trailerIntent() const noexcept
{
    return _trailerIntent;
}

async::Task<HttpResult<BodyChunk>> BufferBodySource::next()
{
    auto chunk = BodyChunk { .bytes = std::vector<std::byte>(_bytes.size()), .end = BodyEnd::Reached };
    std::ranges::transform(
        _bytes, chunk.bytes.begin(), [](char octet) { return static_cast<std::byte>(octet); });
    _bytes.clear();
    co_return chunk;
}

async::Task<HttpResult<Fields>> BufferBodySource::trailers()
{
    co_return std::move(_trailers);
}

Framing framingFor(IBodySource const* source)
{
    if (source == nullptr)
        return Framing {};
    auto const length = source->knownLength();
    if (length.has_value() && source->trailerIntent() == TrailerIntent::None)
        return Framing { .kind = BodyFraming::Length,
                         .length = *length,
                         .persistence = Persistence::KeepAlive };
    return Framing { .kind = BodyFraming::Chunked, .length = 0, .persistence = Persistence::KeepAlive };
}

} // namespace core::http
