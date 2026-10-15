// SPDX-License-Identifier: Apache-2.0
#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using core::hmacSha256;
using core::Sha256;
using core::toLowerHexString;

namespace
{

/// The bytes of @p text, as a message held in `std::byte`.
/// @param text The text.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text)
{
    auto bytes = std::vector<std::byte>(text.size());
    std::ranges::transform(text, bytes.begin(), [](char ch) { return static_cast<std::byte>(ch); });
    return bytes;
}

/// @p count copies of @p value.
///
/// The RFC 4231 keys and messages that are runs of one byte are built rather than typed as hex:
/// a run miscounted by hand tests a different input, and its failure accuses the implementation.
/// @param value The byte.
/// @param count How many.
/// @return The run.
[[nodiscard]] std::vector<std::byte> repeated(std::uint8_t value, std::size_t count)
{
    return std::vector<std::byte>(count, std::byte { value });
}

/// The digest @p hex spells, in a constant expression too, so a known answer can be checked at
/// compile time.
/// @param hex 64 lower-case hexadecimal digits.
/// @return The digest.
[[nodiscard]] constexpr Sha256::Digest digestOf(std::string_view hex)
{
    auto const nibble = [](char digit) {
        return digit <= '9' ? digit - '0' : digit - 'a' + 10;
    };
    auto digest = Sha256::Digest {};
    for (auto const index: std::views::iota(std::size_t { 0 }, digest.size()))
        digest[index] = static_cast<std::byte>((nibble(hex[2 * index]) << 4) | nibble(hex[(2 * index) + 1]));
    return digest;
}

/// A message and the digest it must have.
struct KnownDigest
{
    std::string_view what;
    std::string message;
    std::string_view digest;
};

/// A key, a message, and the HMAC-SHA256 tag they must have, or its first `tagBytes` bytes.
struct KnownTag
{
    std::string_view what;
    std::vector<std::byte> key;
    std::vector<std::byte> message;
    std::size_t tagBytes;
    std::string_view tag;
};

/// A 130-byte message that does not repeat with the block size, so a piece misplaced by a block
/// changes the digest.
/// @return The message.
[[nodiscard]] std::vector<std::byte> message130()
{
    auto message = std::vector<std::byte>(130);
    for (auto const index: std::views::iota(std::size_t { 0 }, message.size()))
        message[index] = static_cast<std::byte>(((index * 37) + 11) & 0xFFU);
    return message;
}

/// `message130()`'s digest, computed with Python's hashlib.
constexpr auto Message130Digest =
    std::string_view { "d35b74124cb85cfac62bfeaf3a9771efab3e0ef88fc43702c35a6efe09467cc1" };

} // namespace

TEST_CASE("Sha256 matches the FIPS 180-4 vectors", "[base][sha256]")
{
    // The examples FIPS 180-4 and RFC 6234 publish: the empty message is all padding and no
    // message; the 448-bit one leaves no room for the length in its block, so padding takes a
    // second; the 896-bit one spans two blocks with room to spare; the million 'a's end exactly on
    // a block boundary, so padding is a block of its own.
    auto const vectors = std::array {
        KnownDigest { .what = "the empty message",
                      .message = "",
                      .digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        KnownDigest { .what = "\"abc\"",
                      .message = "abc",
                      .digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        KnownDigest { .what = "the 448-bit message",
                      .message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                      .digest = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
        KnownDigest { .what = "the 896-bit message",
                      .message = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjkl"
                                 "mnopqklmnopqrlmnop"
                                 "qrsmnopqrstnopqrstu",
                      .digest = "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
        KnownDigest { .what = "one million 'a's",
                      .message = std::string(1'000'000, 'a'),
                      .digest = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" },
    };

    for (auto const& [what, message, digest]: vectors)
    {
        INFO(what << ", " << message.size() << " bytes");
        CHECK(toLowerHexString(Sha256::hash(message)) == digest);
        CHECK(toLowerHexString(Sha256::hash(bytesOf(message))) == digest);
    }
}

TEST_CASE("Sha256 pads correctly on either side of a block boundary", "[base][sha256]")
{
    // The padding has two branches -- the 64-bit length fits after the one bit, or it takes
    // another block -- and 55 and 56 bytes are the last length of the first and the first of the
    // second. The rest are a block's end, exactly one block, one past it, and the same boundary a
    // block later. Digests computed with Python's hashlib.
    auto const vectors = std::array {
        KnownDigest { .what = "55 'a's",
                      .message = std::string(55, 'a'),
                      .digest = "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
        KnownDigest { .what = "56 'a's",
                      .message = std::string(56, 'a'),
                      .digest = "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
        KnownDigest { .what = "63 'a's",
                      .message = std::string(63, 'a'),
                      .digest = "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
        KnownDigest { .what = "64 'a's",
                      .message = std::string(64, 'a'),
                      .digest = "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
        KnownDigest { .what = "65 'a's",
                      .message = std::string(65, 'a'),
                      .digest = "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
        KnownDigest { .what = "119 'a's",
                      .message = std::string(119, 'a'),
                      .digest = "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb" },
        KnownDigest { .what = "120 'a's",
                      .message = std::string(120, 'a'),
                      .digest = "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c" },
        KnownDigest { .what = "128 'a's",
                      .message = std::string(128, 'a'),
                      .digest = "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
    };

    for (auto const& [what, message, digest]: vectors)
    {
        INFO(what);
        CHECK(toLowerHexString(Sha256::hash(message)) == digest);
    }
}

TEST_CASE("Sha256 digests a message the same wherever update() splits it", "[base][sha256]")
{
    // The contract of an incremental hash. 130 bytes are two whole blocks and two bytes, so the
    // splits put the first piece's end at every offset within a block, on both boundaries, and at
    // both ends of the message.
    auto const message = message130();
    auto const whole = std::span<std::byte const> { message };
    auto const oneShot = Sha256::hash(whole);
    REQUIRE(toLowerHexString(oneShot) == Message130Digest);

    SECTION("in two pieces, split at every offset")
    {
        for (auto const split: std::views::iota(std::size_t { 0 }, whole.size() + 1))
        {
            INFO("split at " << split);
            auto hasher = Sha256 {};
            hasher.update(whole.first(split));
            hasher.update(whole.subspan(split));
            CHECK(hasher.finish() == oneShot);
        }
    }

    SECTION("in three pieces, split at every pair of offsets")
    {
        for (auto const first: std::views::iota(std::size_t { 0 }, whole.size() + 1))
        {
            for (auto const second: std::views::iota(first, whole.size() + 1))
            {
                INFO("split at " << first << " and " << second);
                auto hasher = Sha256 {};
                hasher.update(whole.first(first));
                hasher.update(whole.subspan(first, second - first));
                hasher.update(whole.subspan(second));
                CHECK(hasher.finish() == oneShot);
            }
        }
    }

    SECTION("one byte at a time")
    {
        auto hasher = Sha256 {};
        for (auto const& byte: message)
            hasher.update(std::span { &byte, 1 });
        CHECK(hasher.finish() == oneShot);
    }
}

TEST_CASE("Sha256::finish() leaves the hasher as it was", "[base][sha256]")
{
    auto hasher = Sha256 {};
    hasher.update("ab");
    auto const early = hasher.finish();
    CHECK(early == Sha256::hash("ab"));
    CHECK(hasher.finish() == early);

    // More input after a finish() continues the same message.
    hasher.update("c");
    CHECK(hasher.finish() == Sha256::hash("abc"));
}

TEST_CASE("hmacSha256 matches the RFC 4231 vectors", "[base][sha256][hmac]")
{
    // RFC 4231 §4, test cases 1 to 7. Case 2's key is shorter than a block and zero-padded, case
    // 4's is a sequence rather than a run, case 5's tag is published truncated to 128 bits, and
    // cases 6 and 7 have a 131-byte key, longer than the 64-byte block, so it is hashed first;
    // case 7's message is longer than a block too.
    auto keyOfCase4 = std::vector<std::byte>(25);
    for (auto const index: std::views::iota(std::size_t { 0 }, keyOfCase4.size()))
        keyOfCase4[index] = static_cast<std::byte>(index + 1);

    auto const vectors = std::array {
        KnownTag { .what = "case 1",
                   .key = repeated(0x0b, 20),
                   .message = bytesOf("Hi There"),
                   .tagBytes = 32,
                   .tag = "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
        KnownTag { .what = "case 2: a key shorter than the tag",
                   .key = bytesOf("Jefe"),
                   .message = bytesOf("what do ya want for nothing?"),
                   .tagBytes = 32,
                   .tag = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
        KnownTag { .what = "case 3",
                   .key = repeated(0xaa, 20),
                   .message = repeated(0xdd, 50),
                   .tagBytes = 32,
                   .tag = "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe" },
        KnownTag { .what = "case 4",
                   .key = keyOfCase4,
                   .message = repeated(0xcd, 50),
                   .tagBytes = 32,
                   .tag = "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b" },
        KnownTag { .what = "case 5: truncated to 128 bits",
                   .key = repeated(0x0c, 20),
                   .message = bytesOf("Test With Truncation"),
                   .tagBytes = 16,
                   .tag = "a3b6167473100ee06e0c796c2955552b" },
        KnownTag { .what = "case 6: a key longer than the block",
                   .key = repeated(0xaa, 131),
                   .message = bytesOf("Test Using Larger Than Block-Size Key - Hash Key First"),
                   .tagBytes = 32,
                   .tag = "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" },
        KnownTag { .what = "case 7: a key and a message longer than the block",
                   .key = repeated(0xaa, 131),
                   .message = bytesOf(
                       "This is a test using a larger than block-size key and a larger than block-size data. "
                       "The key needs to be hashed before being used by the HMAC algorithm."),
                   .tagBytes = 32,
                   .tag = "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2" },
    };

    for (auto const& [what, key, message, tagBytes, tag]: vectors)
    {
        INFO(what);
        REQUIRE(tag.size() == 2 * tagBytes);
        auto const computed = hmacSha256(key, message);
        CHECK(toLowerHexString(std::span { computed }.first(tagBytes)) == tag);
    }
}

TEST_CASE("hmacSha256 hashes a key longer than the block, and only such a key", "[base][sha256][hmac]")
{
    // RFC 4231 has no key of exactly one block, the length a `>=` written for `>` gets wrong.
    // Tags computed with Python's hmac module.
    auto const keyOf = [](std::size_t length) {
        auto key = std::vector<std::byte>(length);
        for (auto const index: std::views::iota(std::size_t { 0 }, length))
            key[index] = static_cast<std::byte>(index);
        return key;
    };
    auto const message = bytesOf("core-cpp");

    CHECK(toLowerHexString(hmacSha256(keyOf(Sha256::BlockSize), message))
          == "a17c5af7fd1d4de385d0e985b2d435fcb658cedc51b4a67609ce04d11f31e27c");
    CHECK(toLowerHexString(hmacSha256(keyOf(Sha256::BlockSize + 1), message))
          == "9c16eb4badf173c00cf843fb5db2ed1111cf637921867d322d8deab1b982ed64");
}

TEST_CASE("hmacSha256 over text equals hmacSha256 over its bytes", "[base][sha256][hmac]")
{
    // RFC 4231 case 2, whose key and message are both text.
    CHECK(hmacSha256("Jefe", "what do ya want for nothing?")
          == hmacSha256(bytesOf("Jefe"), bytesOf("what do ya want for nothing?")));
    CHECK(toLowerHexString(hmacSha256(std::string(131, 'k'), "message"))
          == toLowerHexString(hmacSha256(bytesOf(std::string(131, 'k')), bytesOf("message"))));
}

TEST_CASE("Sha256 and hmacSha256 are usable in a constant expression", "[base][sha256]")
{
    STATIC_CHECK(Sha256::hash("")
                 == digestOf("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    STATIC_CHECK(Sha256::hash("abc")
                 == digestOf("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    STATIC_CHECK(hmacSha256("Jefe", "what do ya want for nothing?")
                 == digestOf("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}
