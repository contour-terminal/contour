// SPDX-License-Identifier: Apache-2.0
#include <crispy/base64.h>

#include <catch2/catch_test_macros.hpp>

#include <ranges>
#include <string>
#include <string_view>

using namespace crispy;

// Helper: encode a string using the streaming (byte-at-a-time) API.
static std::string streamingEncode(std::string_view input)
{
    std::string output;
    auto state = base64::encoder_state {};
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
