// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/KittyInflate.hpp>

#include <algorithm>
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

    /// How many times its compressed size an output is reserved for up front. Only a hint: the cap
    /// may be a quota remainder far larger than the image, so it is never reserved outright.
    constexpr auto ReserveRatio = size_t { 4 };

    /// Ends an initialised zlib inflate stream when it goes out of scope.
    ///
    /// Deliberately not gsl::finally: the tree pins Microsoft GSL 3.1.0, which spells that header
    /// `gsl/gsl_util`, while GSL 4.x (what Homebrew ships) spells it `gsl/util`, so no single include
    /// builds everywhere. The stream is only referenced, never moved: zlib's state points back at it.
    class InflateStreamGuard
    {
      public:
        explicit InflateStreamGuard(z_stream& stream) noexcept: _stream { stream } {}
        InflateStreamGuard(InflateStreamGuard const&) = delete;
        InflateStreamGuard(InflateStreamGuard&&) = delete;
        InflateStreamGuard& operator=(InflateStreamGuard const&) = delete;
        InflateStreamGuard& operator=(InflateStreamGuard&&) = delete;
        ~InflateStreamGuard() { inflateEnd(&_stream); }

      private:
        z_stream& _stream;
    };
} // namespace

std::expected<std::vector<uint8_t>, InflateError> inflateZlib(std::span<uint8_t const> compressed,
                                                              size_t maxOutputBytes)
{
    // zlib counts in uInt; a payload that does not fit cannot be a legitimate image.
    if (compressed.size() > std::numeric_limits<uInt>::max())
        return std::unexpected { InflateError::TooLarge };

    // Not wrapped in a movable type: zlib's state points back at its z_stream, so the stream must
    // stay where it was initialised.
    auto stream = z_stream {};
    if (inflateInit(&stream) != Z_OK)
        return std::unexpected { InflateError::Corrupt };
    auto const release = InflateStreamGuard { stream };

    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());

    auto output = std::vector<uint8_t> {};
    output.reserve(std::min(maxOutputBytes, compressed.size() * ReserveRatio));
    for (;;)
    {
        // Inflated straight into the output's tail, which is trimmed back to what was produced.
        auto const kept = output.size();
        output.resize(kept + ChunkSize);
        stream.next_out = output.data() + kept;
        stream.avail_out = static_cast<uInt>(ChunkSize);
        auto const status = inflate(&stream, Z_NO_FLUSH);

        auto const produced = ChunkSize - stream.avail_out;
        if (produced > maxOutputBytes - kept)
            return std::unexpected { InflateError::TooLarge };
        output.resize(kept + produced);

        if (status == Z_STREAM_END)
            return output;
        // Z_BUF_ERROR is a stream that can make no progress: its input ended before its end marker.
        if (status != Z_OK)
            return std::unexpected { InflateError::Corrupt };
    }
}

} // namespace vtbackend::kitty_graphics
