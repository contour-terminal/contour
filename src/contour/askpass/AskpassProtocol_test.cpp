// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassProtocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

using namespace contour::askpass;

TEST_CASE("askpass classifies a prompt from its environment and text", "[contour][askpass]")
{
    CHECK(classifyAskpassPrompt("Allow?", "confirm") == AskpassPromptKind::Confirm);
    CHECK(classifyAskpassPrompt("Confirm user presence for key ED25519-SK SHA256:abc", "none")
          == AskpassPromptKind::Notice);
    CHECK(classifyAskpassPrompt("The authenticity of host 'h' can't be established.\n"
                                "Are you sure you want to continue connecting (yes/no/[fingerprint])? ",
                                "")
          == AskpassPromptKind::HostKey);
    CHECK(classifyAskpassPrompt("Enter passphrase for key '/home/me/.ssh/id_ed25519': ", "")
          == AskpassPromptKind::Secret);
    CHECK(classifyAskpassPrompt("me@host's password: ", "") == AskpassPromptKind::Secret);
    // The environment names the kind outright, so it wins over what the text happens to say.
    CHECK(classifyAskpassPrompt("(yes/no)", "confirm") == AskpassPromptKind::Confirm);
}

TEST_CASE("askpass request round-trips", "[contour][askpass]")
{
    auto const request = AskpassRequest { .nonce = "abcd", .prompt = "pw: ", .sshAskpassPrompt = "confirm" };
    auto const frame = encode(request);
    auto const size = askpassFrameSize(frame);
    REQUIRE(size.has_value());
    CHECK(*size == frame.size());
    auto const decoded = decodeAskpassRequest(frame);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == request);

    auto const empty = decodeAskpassRequest(encode(AskpassRequest {}));
    REQUIRE(empty.has_value());
    CHECK(*empty == AskpassRequest {});
}

TEST_CASE("askpass reply round-trips", "[contour][askpass]")
{
    for (auto const& reply: { AskpassReply { AskpassOutcome::Answered, "hunter2" },
                              AskpassReply { AskpassOutcome::Cancelled, "" } })
    {
        auto const decoded = decodeAskpassReply(encode(reply));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == reply);
    }
}

TEST_CASE("askpass frames: a prefix still arriving is not an error", "[contour][askpass]")
{
    auto const frame = encode(AskpassRequest { .nonce = "n", .prompt = "p", .sshAskpassPrompt = "" });
    auto const partial = askpassFrameSize(frame.substr(0, AskpassFramePrefixSize - 1));
    REQUIRE(partial.has_value());
    CHECK_FALSE(partial->has_value());
}

TEST_CASE("askpass frames: truncated and malformed payloads are rejected", "[contour][askpass]")
{
    auto const frame =
        encode(AskpassRequest { .nonce = "nonce", .prompt = "prompt", .sshAskpassPrompt = "" });
    CHECK_FALSE(decodeAskpassRequest(frame.substr(0, frame.size() - 1)).has_value());
    CHECK_FALSE(decodeAskpassRequest(frame.substr(0, AskpassFramePrefixSize)).has_value());
    CHECK_FALSE(decodeAskpassRequest("").has_value());
    CHECK_FALSE(decodeAskpassRequest(frame + "x").has_value());
    // A reply is not a request.
    CHECK_FALSE(decodeAskpassRequest(encode(AskpassReply { AskpassOutcome::Answered, "x" })).has_value());
    // An outcome byte no enumerator names.
    auto bad = encode(AskpassReply { AskpassOutcome::Answered, "x" });
    bad[AskpassFramePrefixSize] = static_cast<char>(9);
    CHECK_FALSE(decodeAskpassReply(bad).has_value());
}

TEST_CASE("askpass frames: an oversized frame is rejected", "[contour][askpass]")
{
    auto frame = std::string(AskpassFramePrefixSize, '\0');
    auto const declared = static_cast<std::uint32_t>(MaxAskpassPayload + 1);
    for (auto const shift: { 0U, 8U, 16U, 24U })
        frame[shift / 8U] = static_cast<char>((declared >> shift) & 0xFFU);
    CHECK_FALSE(askpassFrameSize(frame).has_value());

    // A frame exactly at the limit is fine.
    auto const atLimit =
        encode(AskpassReply { AskpassOutcome::Answered, std::string(MaxAskpassPayload - 5, 'a') });
    CHECK(askpassFrameSize(atLimit).has_value());
    CHECK(decodeAskpassReply(atLimit).has_value());
    // One more byte of answer is not.
    auto const over =
        encode(AskpassReply { AskpassOutcome::Answered, std::string(MaxAskpassPayload - 4, 'a') });
    CHECK_FALSE(askpassFrameSize(over).has_value());
    CHECK_FALSE(decodeAskpassReply(over).has_value());
}

TEST_CASE("askpass wipe empties a secret after zeroing it", "[contour][askpass]")
{
    auto secret = std::string { "hunter2" };
    auto const* const bytes = secret.data();
    wipe(secret);
    CHECK(secret.empty());
    // The storage the secret lived in no longer holds it (the buffer is small-string, so still valid to
    // read).
    CHECK(std::string_view { bytes, 7 } == std::string_view { "\0\0\0\0\0\0\0", 7 });
}
