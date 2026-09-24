// SPDX-License-Identifier: Apache-2.0
#include <core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <string_view>

using namespace core;
using namespace std::string_view_literals;

// Helper: encode a string using the streaming (byte-at-a-time) API.
static std::string streamingEncode(std::string_view input)
{
    std::string output;
    auto state = base64::EncoderState {};
    auto const writer = [&](char a, char b, char c, char d) {
        output += a;
        output += b;
        output += c;
        output += d;
    };
    for (auto const byte: input)
        base64::encode(static_cast<uint8_t>(byte), state, writer);
    base64::finish(state, writer);
    return output;
}

TEST_CASE("base64.encode", "[base64]")
{
    CHECK("YQ==" == base64::encode("a"));
    CHECK("YWI=" == base64::encode("ab"));
    CHECK("YWJj" == base64::encode("abc"));
    CHECK("YWJjZA==" == base64::encode("abcd"));
    CHECK("Zm9vOmJhcg==" == base64::encode("foo:bar"));
}

TEST_CASE("base64.decode", "[base64]")
{
    CHECK("a" == base64::decode("YQ=="));
    CHECK("ab" == base64::decode("YWI="));
    CHECK("abc" == base64::decode("YWJj"));
    CHECK("abcd" == base64::decode("YWJjZA=="));
    CHECK("foo:bar" == base64::decode("Zm9vOmJhcg=="));
}

TEST_CASE("base64.streaming_encode_matches_batch", "[base64]")
{
    // Length % 3 == 1 → finish() produces 2 encoded chars + "=="
    CHECK(streamingEncode("a") == base64::encode("a"));

    // Length % 3 == 2 → finish() produces 3 encoded chars + "="
    CHECK(streamingEncode("ab") == base64::encode("ab"));

    // Length % 3 == 0 → finish() is a no-op
    CHECK(streamingEncode("abc") == base64::encode("abc"));

    // Longer strings covering all padding cases
    CHECK(streamingEncode("abcd") == base64::encode("abcd"));
    CHECK(streamingEncode("foo:bar") == base64::encode("foo:bar"));
    CHECK(streamingEncode("Hello, World!") == base64::encode("Hello, World!"));
}

TEST_CASE("base64.streaming_encode_roundtrip", "[base64]")
{
    // Verify streaming encode → batch decode produces the original data.
    auto const inputs = { "a", "ab", "abc", "abcd", "Hello", "foo:bar", "test1234" };
    for (auto const* input: inputs)
    {
        auto const encoded = streamingEncode(input);
        auto const decoded = base64::decode(encoded);
        CHECK(decoded == input);
    }
}

TEST_CASE("base64.streaming_encode_binary_roundtrip", "[base64]")
{
    // Test with binary data (non-printable bytes) to verify all byte values survive.
    std::string binary;
    for (auto const i: std::views::iota(0, 256))
        binary.push_back(static_cast<char>(i));

    auto const encoded = streamingEncode(binary);
    auto const batchEncoded = base64::encode(binary);
    CHECK(encoded == batchEncoded);

    // Verify roundtrip
    auto const decoded = base64::decode(encoded);
    CHECK(decoded == binary);
}

// decodeLength() sizes the buffer decode() then fills. Its scan for the end of the base64 prefix
// compared the index-table entry against the table's own size (256) instead of against the 64 the
// table stores for a byte that is not a base64 digit, so every byte passed and the length was
// taken from the whole input -- padding, terminator and trailing junk included.
TEST_CASE("base64.decodeLength", "[base64]")
{
    SECTION("a bare payload")
    {
        CHECK(base64::decodeLength("YQ=="sv) >= base64::decode("YQ==").size());
        CHECK(base64::decodeLength("YWJj"sv) == 3);
        CHECK(base64::decodeLength("YWJjZA=="sv) == 6);
    }

    SECTION("the scan stops at the first byte that is not a base64 digit")
    {
        // "YWJj" decodes to "abc"; what follows is not part of it.
        CHECK(base64::decodeLength("YWJj"sv) == base64::decodeLength("YWJj!!!!!!!!"sv));
        CHECK(base64::decodeLength("YWJj"sv) == base64::decodeLength("YWJj\n\n\n\n"sv));
        CHECK(base64::decodeLength("YWJj"sv) == base64::decodeLength("YWJj and then some prose"sv));
    }

    SECTION("the size is enough for what decode() writes, and not wildly more")
    {
        for (auto const* input: { "", "YQ==", "YWI=", "YWJj", "YWJjZA==", "Zm9vOmJhcg==" })
        {
            auto const reserved = base64::decodeLength(std::string_view { input });
            auto const decoded = base64::decode(input);
            INFO(input);
            CHECK(reserved >= decoded.size());
            CHECK(reserved <= decoded.size() + 2);
        }
    }
}
