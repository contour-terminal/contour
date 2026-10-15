// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>

namespace core
{

namespace detail::sha256
{
    /// The 64 round constants: the first 32 bits of the fractional parts of the cube roots of the
    /// first 64 primes (FIPS 180-4 §4.2.2).
    inline constexpr auto RoundConstants = std::array<std::uint32_t, 64> {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
        0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
        0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
        0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
        0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
        0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
        0xc67178f2U,
    };

    /// The initial hash value: the first 32 bits of the fractional parts of the square roots of the
    /// first eight primes (FIPS 180-4 §5.3.3).
    inline constexpr auto InitialState = std::array<std::uint32_t, 8> {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };

    /// The byte HMAC XORs into every byte of the key block for the inner hash (RFC 2104 §2, ipad).
    inline constexpr auto InnerPad = std::byte { 0x36 };

    /// The byte HMAC XORs into every byte of the key block for the outer hash (RFC 2104 §2, opad).
    inline constexpr auto OuterPad = std::byte { 0x5c };

    // The functions of FIPS 180-4 §4.1.2, spelled as the standard spells them, so a reader can
    // check each against it line by line rather than reverse the algebra of a "simplified" form.

    /// FIPS 180-4's Ch: each bit of @p x chooses the bit of @p y (set) or of @p z (clear).
    [[nodiscard]] constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        return (x & y) ^ (~x & z);
    }

    /// FIPS 180-4's Maj: each bit is the majority of the three inputs' bits.
    [[nodiscard]] constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    /// FIPS 180-4's upper-case Sigma 0.
    [[nodiscard]] constexpr std::uint32_t bigSigma0(std::uint32_t x) noexcept
    {
        return std::rotr(x, 2) ^ std::rotr(x, 13) ^ std::rotr(x, 22);
    }

    /// FIPS 180-4's upper-case Sigma 1.
    [[nodiscard]] constexpr std::uint32_t bigSigma1(std::uint32_t x) noexcept
    {
        return std::rotr(x, 6) ^ std::rotr(x, 11) ^ std::rotr(x, 25);
    }

    /// FIPS 180-4's lower-case sigma 0.
    [[nodiscard]] constexpr std::uint32_t smallSigma0(std::uint32_t x) noexcept
    {
        return std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3U);
    }

    /// FIPS 180-4's lower-case sigma 1.
    [[nodiscard]] constexpr std::uint32_t smallSigma1(std::uint32_t x) noexcept
    {
        return std::rotr(x, 17) ^ std::rotr(x, 19) ^ (x >> 10U);
    }

    /// Reads four bytes as the big-endian word SHA-256 defines them to be, whatever the host's
    /// byte order: a native-order read would make the digest differ between machines.
    /// @param bytes The word's four bytes, most significant first.
    /// @return The word.
    [[nodiscard]] constexpr std::uint32_t loadBigEndian(std::span<std::byte const, 4> bytes) noexcept
    {
        auto value = std::uint32_t { 0 };
        for (auto const byte: bytes)
            value = (value << 8U) | std::to_integer<std::uint32_t>(byte);
        return value;
    }

    /// Writes @p value into @p out big-endian, most significant byte first.
    /// @param out Exactly as many bytes as @p value has.
    /// @param value The value to write.
    template <std::unsigned_integral T>
    constexpr void storeBigEndian(std::span<std::byte, sizeof(T)> out, T value) noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, sizeof(T)))
            out[index] = static_cast<std::byte>(value >> ((sizeof(T) - 1 - index) * 8));
    }
} // namespace detail::sha256

/// SHA-256 (FIPS 180-4), incremental, and usable in a constant expression.
///
/// Feed the message to `update()` in as many pieces as is convenient, then ask `finish()` for the
/// digest; how the message was split never changes the digest. `hash()` does both for a message
/// held in one piece. A message is bytes: `std::byte`, or `char` through the `std::string_view`
/// overloads, which digest the same bytes the same way.
///
/// It needs nothing but the standard library, so a module that must not depend on TLS (or on
/// OpenSSL, which core-cpp links only behind `CORE_CPP_WITH_TLS`) can still hash. It is the
/// portable scalar algorithm alone: it uses no SHA instructions, so it suits identifiers, keys
/// and other short inputs rather than bulk data. A message must be shorter than 2^61 bytes, the
/// standard's 2^64 bits.
class Sha256
{
  public:
    /// Bytes in a SHA-256 digest.
    static constexpr std::size_t DigestSize = 32;

    /// Bytes in a SHA-256 compression block. HMAC's key padding depends on it.
    static constexpr std::size_t BlockSize = 64;

    /// A SHA-256 digest, the bytes in the order the standard writes them.
    using Digest = std::array<std::byte, DigestSize>;

    /// A hasher that has absorbed nothing yet.
    constexpr Sha256() noexcept = default;

    /// Absorbs more of the message. May be called any number of times, also after `finish()`.
    /// @param input The next bytes of the message.
    constexpr void update(std::span<std::byte const> input) noexcept;

    /// Absorbs more of the message, each `char` as the byte it holds.
    /// @param input The next bytes of the message.
    constexpr void update(std::string_view input) noexcept;

    /// The digest of everything absorbed so far.
    ///
    /// It leaves the hasher as it was, so asking twice gives the same digest, and more input may
    /// follow: a later `finish()` then digests the longer message.
    /// @return The 32-byte digest.
    [[nodiscard]] constexpr Digest finish() const noexcept
    {
        auto state = _state;
        auto block = _pending;
        auto const filled = pendingSize();

        // FIPS 180-4 §5.1.1: a one bit, then zeroes, then the message's length in bits as a
        // big-endian 64-bit number, which together make the padded message whole blocks.
        block[filled] = std::byte { 0x80 };
        std::ranges::fill(std::span { block }.subspan(filled + 1), std::byte { 0 });
        if (BlockSize - (filled + 1) < LengthSize)
        {
            // No room for the length after the one bit: this block ends in zeroes, and the length
            // goes at the end of one more.
            compress(state, block);
            block.fill(std::byte { 0 });
        }
        detail::sha256::storeBigEndian(std::span { block }.last<LengthSize>(), _bytesAbsorbed * 8);
        compress(state, block);

        auto digest = Digest {};
        for (auto const index: std::views::iota(std::size_t { 0 }, state.size()))
            detail::sha256::storeBigEndian(std::span { digest }.subspan(index * 4).first<4>(), state[index]);
        return digest;
    }

    /// The digest of a message held in one piece.
    /// @param input The whole message.
    /// @return The 32-byte digest.
    [[nodiscard]] static constexpr Digest hash(std::span<std::byte const> input) noexcept
    {
        auto hasher = Sha256 {};
        hasher.update(input);
        return hasher.finish();
    }

    /// The digest of a message held in one piece, each `char` as the byte it holds.
    /// @param input The whole message.
    /// @return The 32-byte digest.
    [[nodiscard]] static constexpr Digest hash(std::string_view input) noexcept
    {
        auto hasher = Sha256 {};
        hasher.update(input);
        return hasher.finish();
    }

  private:
    /// One compression block.
    using Block = std::array<std::byte, BlockSize>;

    /// The eight working variables, a to h.
    using State = std::array<std::uint32_t, 8>;

    /// Bytes at the end of the last block that hold the message length.
    static constexpr std::size_t LengthSize = 8;

    /// How much of `_pending` holds message bytes not yet compressed.
    /// @return A count below `BlockSize`.
    [[nodiscard]] constexpr std::size_t pendingSize() const noexcept
    {
        return static_cast<std::size_t>(_bytesAbsorbed % BlockSize);
    }

    /// Absorbs @p input, whichever byte type it is held in.
    ///
    /// Every byte passes through `_pending`, so one compression serves both byte types, in a
    /// constant expression too, and a caller's chunking cannot change which blocks are compressed.
    /// @param input The next bytes of the message.
    template <typename Byte>
    constexpr void absorb(std::span<Byte const> input) noexcept
    {
        while (!input.empty())
        {
            auto const filled = pendingSize();
            auto const take = std::min(BlockSize - filled, input.size());
            std::ranges::transform(input.first(take),
                                   std::span { _pending }.subspan(filled).begin(),
                                   [](Byte value) { return std::bit_cast<std::byte>(value); });
            _bytesAbsorbed += take;
            input = input.subspan(take);
            if (filled + take == BlockSize)
                compress(_state, _pending);
        }
    }

    /// Compresses one block into the running state (FIPS 180-4 §6.2.2), spelled as the standard
    /// spells it.
    /// @param state The running state.
    /// @param block The block.
    static constexpr void compress(State& state, Block const& block) noexcept
    {
        using detail::sha256::bigSigma0;
        using detail::sha256::bigSigma1;
        using detail::sha256::choose;
        using detail::sha256::majority;
        using detail::sha256::smallSigma0;
        using detail::sha256::smallSigma1;

        auto schedule = std::array<std::uint32_t, 64> {};
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 16 }))
            schedule[index] =
                detail::sha256::loadBigEndian(std::span { block }.subspan(index * 4).first<4>());
        for (auto const index: std::views::iota(std::size_t { 16 }, schedule.size()))
            schedule[index] = smallSigma1(schedule[index - 2]) + schedule[index - 7]
                              + smallSigma0(schedule[index - 15]) + schedule[index - 16];

        auto working = state;
        for (auto const index: std::views::iota(std::size_t { 0 }, schedule.size()))
        {
            auto const temp1 = working[7] + bigSigma1(working[4]) + choose(working[4], working[5], working[6])
                               + detail::sha256::RoundConstants[index] + schedule[index];
            auto const temp2 = bigSigma0(working[0]) + majority(working[0], working[1], working[2]);

            working[7] = working[6];
            working[6] = working[5];
            working[5] = working[4];
            working[4] = working[3] + temp1;
            working[3] = working[2];
            working[2] = working[1];
            working[1] = working[0];
            working[0] = temp1 + temp2;
        }

        for (auto const index: std::views::iota(std::size_t { 0 }, state.size()))
            state[index] += working[index];
    }

    /// The running state, which starts as FIPS 180-4's initial hash value.
    State _state = detail::sha256::InitialState;

    /// Bytes absorbed so far; the length is padded in as a bit count.
    std::uint64_t _bytesAbsorbed = 0;

    /// The partial block awaiting the rest of its bytes; its first `pendingSize()` bytes count.
    Block _pending {};
};

// Defined after the class, not in it: clang parses in-class member bodies in declaration order, so
// an in-class `update()` names the `absorb()` template before its body has been parsed, and clang
// then instantiates it only at the end of the translation unit -- after any constant expression
// that needed it ("undefined function 'absorb<char>' cannot be used in a constant expression").

constexpr void Sha256::update(std::span<std::byte const> input) noexcept
{
    absorb(input);
}

constexpr void Sha256::update(std::string_view input) noexcept
{
    absorb(std::span { input });
}

namespace detail::sha256
{
    /// HMAC-SHA256 over a key and a message, each a `std::span<std::byte const>` or a
    /// `std::string_view`.
    /// @param key The key; any length.
    /// @param message The message.
    /// @return The 32-byte tag.
    template <typename Key, typename Message>
    [[nodiscard]] constexpr Sha256::Digest hmac(Key key, Message message) noexcept
    {
        // RFC 2104 §2: a key longer than the block is replaced by its digest, and a shorter one is
        // padded with zeroes to the block.
        auto keyBlock = std::array<std::byte, Sha256::BlockSize> {};
        if (key.size() > Sha256::BlockSize)
            std::ranges::copy(Sha256::hash(key), keyBlock.begin());
        else
            std::ranges::transform(
                key, keyBlock.begin(), [](auto value) { return std::bit_cast<std::byte>(value); });

        auto const padded = [&keyBlock](std::byte pad) {
            auto block = keyBlock;
            for (auto& byte: block)
                byte ^= pad;
            return block;
        };

        auto inner = Sha256 {};
        inner.update(padded(InnerPad));
        inner.update(message);

        auto outer = Sha256 {};
        outer.update(padded(OuterPad));
        outer.update(inner.finish());
        return outer.finish();
    }
} // namespace detail::sha256

/// HMAC-SHA256 (RFC 2104 over FIPS 180-4), the keyed hash RFC 4231 gives the test vectors for.
///
/// The construction to use where a digest must depend on a secret: a bare hash of `key || message`
/// can be extended by anybody who knows the digest, without the key, which HMAC's nested hashes
/// prevent. It does not erase the copies of the key it makes on its stack.
/// @param key The key; any length. A key longer than `Sha256::BlockSize` is hashed first.
/// @param message The message.
/// @return The 32-byte tag.
[[nodiscard]] constexpr Sha256::Digest hmacSha256(std::span<std::byte const> key,
                                                  std::span<std::byte const> message) noexcept
{
    return detail::sha256::hmac(key, message);
}

/// HMAC-SHA256 of a key and a message held as text, each `char` as the byte it holds.
/// @param key The key; any length. A key longer than `Sha256::BlockSize` is hashed first.
/// @param message The message.
/// @return The 32-byte tag.
[[nodiscard]] constexpr Sha256::Digest hmacSha256(std::string_view key, std::string_view message) noexcept
{
    return detail::sha256::hmac(key, message);
}

} // namespace core
