// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassProtocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
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
    CHECK(classifyAskpassPrompt("Warning: the ECDSA host key for 'h' differs from the key for the IP address "
                                "'1.2.3.4'\nAre you sure you want to continue connecting (yes/no)? ",
                                "")
          == AskpassPromptKind::HostKeyWarning);
    CHECK(classifyAskpassPrompt("Warning: the RSA host key for 'h' differs: keys of different type are "
                                "already known for this host.\nContinue (yes/no)? ",
                                "")
          == AskpassPromptKind::HostKeyWarning);
    CHECK(classifyAskpassPrompt("Accept updated hostkeys? (yes/no)", "") == AskpassPromptKind::HostKeyUpdate);
    CHECK(classifyAskpassPrompt("Some other question (yes/no)? ", "") == AskpassPromptKind::Question);
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
    for (auto const& reply:
         { answeredReply("hunter2"),
           AskpassReply { .outcome = AskpassOutcome::Cancelled, .answer = Secret { "" } } })
    {
        auto const decoded = decodeAskpassReply(encode(reply).view());
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
    CHECK_FALSE(decodeAskpassRequest(encode(answeredReply("x")).view()).has_value());
    // An outcome byte no enumerator names.
    auto bad = std::string { encode(answeredReply("x")).view() };
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
    auto const atLimit = encode(answeredReply(std::string(MaxAskpassPayload - 5, 'a')));
    CHECK(askpassFrameSize(atLimit.view()).has_value());
    CHECK(decodeAskpassReply(atLimit.view()).has_value());
    // One more byte of answer is not.
    auto const over = encode(answeredReply(std::string(MaxAskpassPayload - 4, 'a')));
    CHECK_FALSE(askpassFrameSize(over.view()).has_value());
    CHECK_FALSE(decodeAskpassReply(over.view()).has_value());
}

namespace
{
/// @return Whether every byte of @p secret's storage, its length and beyond, is zero.
[[nodiscard]] bool isZeroed(Secret const& secret)
{
    return std::all_of(
        secret.storage().begin(), secret.storage().end(), [](char byte) { return byte == '\0'; });
}
} // namespace

TEST_CASE("a secret wipes its storage when it goes, and when it is moved from", "[contour][askpass][secret]")
{
    // The sources live on the heap: what a moved-from object holds is the point of these checks, and the
    // analyzers would take reading a moved-from local for a mistake.

    // A short one lives in the string's own buffer, where a plain move leaves it behind.
    auto original = std::make_unique<Secret>("hunter2");
    auto const* const shortBuffer = original->storage().data();
    auto const moved = std::make_unique<Secret>(std::move(*original));
    CHECK(*moved == "hunter2");
    CHECK(original->empty());
    CHECK(isZeroed(*original));
    CHECK(shortBuffer == original->storage().data());

    // So does a long one, which lives on the heap: the new owner has it, the old has nothing.
    auto const longText = std::string(200, 'x');
    auto heap = std::make_unique<Secret>(longText);
    auto const heapMoved = std::make_unique<Secret>(std::move(*heap));
    CHECK(*heapMoved == longText);
    CHECK(heap->empty());
    CHECK(isZeroed(*heap));

    // Assigning wipes what the target held, and the source.
    auto source = std::make_unique<Secret>("hunter2");
    auto target = Secret { "old" };
    target = std::move(*source);
    CHECK(target == "hunter2");
    CHECK(isZeroed(*source));

    // A string handed over is wiped whole.
    auto plain = std::make_unique<std::string>("s3cret");
    auto const taken = Secret { std::move(*plain) };
    CHECK(taken == "s3cret");
    CHECK(std::ranges::all_of(std::string_view { plain->data(), 6 }, [](char byte) { return byte == '\0'; }));
}

TEST_CASE("a secret that grows wipes the buffer it left", "[contour][askpass][secret]")
{
    auto secret = Secret { "abc" };
    auto const* const before = secret.storage().data();
    auto const capacity = secret.storage().size();
    secret.append(std::string(capacity * 4, 'z'));
    CHECK(secret.size() == 3 + (capacity * 4));
    CHECK(secret.view().substr(0, 3) == "abc");
    // The old (small) buffer is inside the object, where the string now keeps its capacity: nothing of "abc"
    // is left in it, unless it is the buffer the secret still uses.
    if (secret.storage().data() != before)
        CHECK_FALSE(std::string_view { before, 15 }.contains("abc"));
}

TEST_CASE("a reply frame carries its answer in a secret and decodes back into one",
          "[contour][askpass][secret]")
{
    auto const frame =
        encode(AskpassReply { .outcome = AskpassOutcome::Answered, .answer = Secret { "pw" } });
    auto const decoded = decodeAskpassReply(frame.view());
    REQUIRE(decoded.has_value());
    CHECK(decoded->answer == "pw");
}
