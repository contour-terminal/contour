// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Message bodies as streams: what a reader hands out from a connection, what a writer is fed
/// from, and how a body's shape picks its framing. A body is never held whole by the connection; a
/// reader hands out at most @c HttpLimits::maxReadChunk at a time, and a writer takes what its
/// source hands it one chunk at a time.

#include <core/async/Task.hpp>
#include <core/http/Framing.hpp>
#include <core/http/HttpExport.hpp>
#include <core/http/Message.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace core::http
{

/// Whether a body has more to come after a chunk.
enum class BodyEnd : std::uint8_t
{
    More,    ///< More of the body follows.
    Reached, ///< This chunk is the body's last; its trailers, if any, can be read now.
};

/// Whether a body source may have trailers to send once its bytes are done.
enum class TrailerIntent : std::uint8_t
{
    None,     ///< It has none; a known length can then frame it by Content-Length.
    Possible, ///< It may have some, which only the chunked coding can carry.
};

/// One piece of a body, owned by whoever holds it.
struct BodyChunk
{
    std::vector<std::byte> bytes; ///< The body bytes; may be empty, at the end in particular.
    BodyEnd end = BodyEnd::More;  ///< Whether this is the body's last chunk.
};

/// A message body as it arrives, read from the connection a chunk at a time.
class IBodyReader
{
  public:
    IBodyReader() = default;
    virtual ~IBodyReader() = default;
    IBodyReader(IBodyReader const&) = delete;
    IBodyReader& operator=(IBodyReader const&) = delete;
    IBodyReader(IBodyReader&&) = delete;
    IBodyReader& operator=(IBodyReader&&) = delete;

    /// Reads the next piece of the body.
    /// @param maxBytes The most bytes wanted; clamped to @c HttpLimits::maxReadChunk, so a caller
    ///        cannot make the host hold more than that. Zero reads nothing and reports where the
    ///        body is.
    /// @return Between none and @p maxBytes bytes, with @c BodyEnd::Reached on the last; every read
    ///         after that answers an empty @c Reached chunk. Or the failure, which ends the body.
    [[nodiscard]] virtual async::Task<HttpResult<BodyChunk>> read(std::size_t maxBytes) = 0;

    /// The trailer fields, held to the connection's @c HttpLimits. Requires @c BodyEnd::Reached.
    /// @return The trailers; empty for a body that was not chunked or sent none.
    [[nodiscard]] virtual async::Task<HttpResult<Fields>> trailers() = 0;
};

/// A message body as it is sent, which the connection pulls a chunk at a time.
class IBodySource
{
  public:
    IBodySource() = default;
    virtual ~IBodySource() = default;
    IBodySource(IBodySource const&) = delete;
    IBodySource& operator=(IBodySource const&) = delete;
    IBodySource(IBodySource&&) = delete;
    IBodySource& operator=(IBodySource&&) = delete;

    /// @return The body's length if the source knows it before sending, which a Length framing needs.
    [[nodiscard]] virtual std::optional<std::uint64_t> knownLength() const noexcept = 0;

    /// @return Whether trailers may follow the body.
    [[nodiscard]] virtual TrailerIntent trailerIntent() const noexcept = 0;

    /// The next piece of the body. Not called again after a @c BodyEnd::Reached chunk.
    /// @return The chunk, or the failure that ends the body.
    [[nodiscard]] virtual async::Task<HttpResult<BodyChunk>> next() = 0;

    /// The trailers, asked for once after the last chunk when @c trailerIntent is
    /// @c TrailerIntent::Possible.
    /// @return The trailers; none of them may be a framing field (@c isFramingField).
    [[nodiscard]] virtual async::Task<HttpResult<Fields>> trailers() = 0;
};

/// A body already in memory: handed out as one chunk, with its trailers.
class CORE_HTTP_API BufferBodySource final: public IBodySource
{
  public:
    /// @param bytes The whole body.
    /// @param trailers The trailers; none means @c TrailerIntent::None, so a known length frames it.
    explicit BufferBodySource(std::string bytes, Fields trailers = {});

    [[nodiscard]] std::optional<std::uint64_t> knownLength() const noexcept override;
    [[nodiscard]] TrailerIntent trailerIntent() const noexcept override;
    [[nodiscard]] async::Task<HttpResult<BodyChunk>> next() override;
    [[nodiscard]] async::Task<HttpResult<Fields>> trailers() override;

  private:
    std::string _bytes;
    std::uint64_t _length;
    Fields _trailers;
    TrailerIntent _trailerIntent;
};

/// The framing a body is sent with: a known length and no trailers is @c BodyFraming::Length,
/// anything else @c BodyFraming::Chunked, and no source at all @c BodyFraming::None. The
/// persistence is always @c Persistence::KeepAlive; the caller closes where it means to.
/// @param source The body, or null for none.
/// @return The framing.
[[nodiscard]] CORE_HTTP_API Framing framingFor(IBodySource const* source);

} // namespace core::http
