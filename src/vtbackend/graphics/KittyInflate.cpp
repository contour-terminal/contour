// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyInflate.hpp>

#include <array>
#include <limits>

// Makes z_stream::next_in point to const, which is what inflate treats it as.
#define ZLIB_CONST
#include <zlib.h>

namespace vtbackend::kitty_graphics
{

namespace
{
    /// Bytes inflated per call; also the most a bomb can overshoot the cap by in scratch space.
    constexpr auto ChunkSize = size_t { 16 } * 1024;

    /// Owns an initialised inflate stream and releases it on every exit path.
    class InflateStream
    {
      public:
        InflateStream() noexcept: _initialised { inflateInit(&_stream) == Z_OK } {}
        ~InflateStream() { inflateEnd(&_stream); }
        InflateStream(InflateStream const&) = delete;
        InflateStream& operator=(InflateStream const&) = delete;
        InflateStream(InflateStream&&) = delete;
        InflateStream& operator=(InflateStream&&) = delete;

        [[nodiscard]] bool initialised() const noexcept { return _initialised; }
        [[nodiscard]] z_stream& get() noexcept { return _stream; }

      private:
        z_stream _stream {};
        bool _initialised;
    };
} // namespace

std::expected<std::vector<uint8_t>, InflateError> inflateZlib(std::span<uint8_t const> compressed,
                                                              size_t maxOutputBytes)
{
    // zlib counts in uInt; a payload that does not fit cannot be a legitimate image.
    if (compressed.size() > std::numeric_limits<uInt>::max())
        return std::unexpected { InflateError::TooLarge };

    auto inflater = InflateStream {};
    if (!inflater.initialised())
        return std::unexpected { InflateError::Corrupt };

    auto& stream = inflater.get();
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());

    auto output = std::vector<uint8_t> {};
    auto chunk = std::array<uint8_t, ChunkSize> {};
    for (;;)
    {
        stream.next_out = chunk.data();
        stream.avail_out = static_cast<uInt>(chunk.size());
        auto const status = inflate(&stream, Z_NO_FLUSH);

        auto const produced = chunk.size() - stream.avail_out;
        if (produced > maxOutputBytes - output.size())
            return std::unexpected { InflateError::TooLarge };
        output.insert(output.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(produced));

        if (status == Z_STREAM_END)
            return output;
        // Z_BUF_ERROR with input exhausted is a stream that stops before its end marker.
        if (status != Z_OK)
            return std::unexpected { InflateError::Corrupt };
        if (stream.avail_in == 0 && produced == 0)
            return std::unexpected { InflateError::Corrupt };
    }
}

} // namespace vtbackend::kitty_graphics
