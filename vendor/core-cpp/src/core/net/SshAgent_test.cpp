// SPDX-License-Identifier: Apache-2.0
//
// The ssh-agent client against a fake agent: a scripted peer on the other end of a socketpair that
// checks the request frame it is sent and answers with the bytes the case chose.

#include <core/Base64.hpp>
#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/SshAgent.hpp>
#include <core/net/detail/SshWire.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using core::async::Task;
using core::net::EventLoop;
using core::net::ISocket;
using core::net::SshAgentError;
using core::net::SshKeyType;
using core::net::SshPublicKey;
using core::net::detail::appendSshByte;
using core::net::detail::appendSshString;
using core::net::detail::appendSshUint32;

namespace
{

constexpr auto Ed25519Line = std::string_view {
    "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIAPPsKR22SKf8kiPM0HVGK+R933e5unsB8NfOolV7LcR ed25519-fixture"
};
constexpr auto EcdsaLine =
    std::string_view { "ecdsa-sha2-nistp256 "
                       "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBNX8UjncwV+AO7NcejL7kBFjf8JYAn/"
                       "I9uSpVnU0OeehNcNQTG7crhz1QxfFSbldcNch/6ptv8CAn5Sg2ezYins= ecdsa-p256-fixture" };

enum class Msg : std::uint8_t
{
    Failure = 5,
    RequestIdentities = 11,
    IdentitiesAnswer = 12,
    SignRequest = 13,
    SignResponse = 14,
};

SshPublicKey keyOf(std::string_view line)
{
    auto key = core::net::parseSshPublicKey(line);
    REQUIRE(key.has_value());
    return *key;
}

/// A frame: length, type, body.
std::vector<std::byte> frameOf(Msg type, std::span<std::byte const> body)
{
    auto out = std::vector<std::byte> {};
    appendSshUint32(out, static_cast<std::uint32_t>(body.size() + 1));
    appendSshByte(out, static_cast<std::uint8_t>(type));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// An identities answer body over (blob, comment) pairs.
std::vector<std::byte> identitiesBody(std::vector<std::pair<std::vector<std::byte>, std::string>> const& keys)
{
    auto body = std::vector<std::byte> {};
    appendSshUint32(body, static_cast<std::uint32_t>(keys.size()));
    for (auto const& [blob, comment]: keys)
    {
        appendSshString(body, blob);
        appendSshString(body, comment);
    }
    return body;
}

/// What the fake agent saw and what it does.
struct Script
{
    std::vector<std::byte> reply;   ///< Sent back after the request; nothing when empty.
    std::vector<std::byte> request; ///< The request frame, as received.
};

/// Reads one request frame, records it, replies per @p script, then hangs up.
Task<void> serve(std::unique_ptr<ISocket> socket, std::shared_ptr<Script> script)
{
    auto header = std::vector<std::byte>(4);
    auto got = std::size_t { 0 };
    while (got < header.size())
    {
        auto const n = co_await socket->read(std::span { header }.subspan(got));
        if (!n || *n == 0)
            co_return;
        got += *n;
    }
    auto const length = core::net::detail::SshWireReader { header }.uint32().value_or(0);
    auto payload = std::vector<std::byte>(length);
    got = 0;
    while (got < payload.size())
    {
        auto const n = co_await socket->read(std::span { payload }.subspan(got));
        if (!n || *n == 0)
            co_return;
        got += *n;
    }
    script->request = header;
    script->request.insert(script->request.end(), payload.begin(), payload.end());
    if (!script->reply.empty())
        std::ignore = co_await socket->write(script->reply);
    socket->close();
}

struct Fixture
{
    std::unique_ptr<core::net::IoBackend> backend = core::net::makeBackend(core::net::preferredBackendKind());
    EventLoop loop { *backend };
    std::shared_ptr<Script> script = std::make_shared<Script>();

    /// One connection to the fake agent.
    Task<std::expected<std::unique_ptr<ISocket>, core::net::NetError>> connect()
    {
        auto pair = core::net::testing::makeSocketPair(loop);
        if (!pair)
            co_return std::unexpected(pair.error());
        loop.spawn(serve(std::move(pair->second), script));
        co_return std::move(pair->first);
    }

    /// An agent whose every connection reaches the fake.
    std::unique_ptr<core::net::ISshAgent> agent()
    {
        return core::net::makeSshAgent([this]() { return connect(); });
    }
};

} // namespace

TEST_CASE("SshAgent.identities: two keys, one of an unsupported type, come back as the supported one")
{
    auto fixture = Fixture {};
    auto const ed25519 = keyOf(Ed25519Line);
    auto const rsaBlob = [] {
        auto blob = std::vector<std::byte> {};
        appendSshString(blob, std::string_view { "ssh-rsa" });
        appendSshString(blob, std::string_view { "\x01\x00\x01" });
        appendSshString(blob, std::string_view { "\x00\xAB" });
        return blob;
    }();
    fixture.script->reply =
        frameOf(Msg::IdentitiesAnswer,
                identitiesBody({ { rsaBlob, "legacy" }, { ed25519.blob, "ed25519-fixture" } }));

    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());

    REQUIRE(keys.has_value());
    REQUIRE(keys->size() == 1);
    CHECK(keys->front() == ed25519);
    CHECK(keys->front().type == SshKeyType::Ed25519);
    CHECK(fixture.script->request == frameOf(Msg::RequestIdentities, {}));
}

TEST_CASE("SshAgent.identities: an agent holding nothing answers an empty list")
{
    auto fixture = Fixture {};
    fixture.script->reply = frameOf(Msg::IdentitiesAnswer, identitiesBody({}));
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE(keys.has_value());
    CHECK(keys->empty());
}

TEST_CASE("SshAgent.identities: a malformed answer is a protocol error")
{
    auto fixture = Fixture {};
    auto body = identitiesBody({ { keyOf(EcdsaLine).blob, "k" } });
    body.pop_back(); // the comment's last byte is missing
    fixture.script->reply = frameOf(Msg::IdentitiesAnswer, body);
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Protocol);
}

TEST_CASE("SshAgent.identities: another message type than the answer is a protocol error")
{
    auto fixture = Fixture {};
    fixture.script->reply = frameOf(Msg::SignResponse, {});
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Protocol);
}

TEST_CASE("SshAgent.identities: a reply past the size bound is a protocol error")
{
    auto fixture = Fixture {};
    appendSshUint32(fixture.script->reply, 1U << 20U);
    appendSshByte(fixture.script->reply, static_cast<std::uint8_t>(Msg::IdentitiesAnswer));
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Protocol);
}

TEST_CASE("SshAgent.sign: sends a sign request with flags 0 and returns the signature")
{
    auto fixture = Fixture {};
    auto const key = keyOf(Ed25519Line);
    auto const data = std::vector<std::byte> { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };

    auto signature = std::vector<std::byte> {};
    appendSshString(signature, std::string_view { "ssh-ed25519" });
    appendSshString(
        signature,
        std::string_view { "sixty-four-bytes-of-signature-go-here-xxxxxxxxxxxxxxxxxxxxxxxxxxxxx" });
    auto replyBody = std::vector<std::byte> {};
    appendSshString(replyBody, signature);
    fixture.script->reply = frameOf(Msg::SignResponse, replyBody);

    auto const agent = fixture.agent();
    auto const answer = fixture.loop.blockOn(agent->sign(key, data));

    REQUIRE(answer.has_value());
    CHECK(*answer == signature);

    auto expectedBody = std::vector<std::byte> {};
    appendSshString(expectedBody, key.blob);
    appendSshString(expectedBody, data);
    appendSshUint32(expectedBody, 0);
    CHECK(fixture.script->request == frameOf(Msg::SignRequest, expectedBody));
}

TEST_CASE("SshAgent.sign: SSH_AGENT_FAILURE is a refusal")
{
    auto fixture = Fixture {};
    fixture.script->reply = frameOf(Msg::Failure, {});
    auto const agent = fixture.agent();
    auto const data = std::vector<std::byte> { std::byte { 9 } };
    auto const signature = fixture.loop.blockOn(agent->sign(keyOf(Ed25519Line), data));
    REQUIRE_FALSE(signature.has_value());
    CHECK(signature.error() == SshAgentError::Refused);
}

TEST_CASE("SshAgent: an agent that hangs up without answering is unavailable")
{
    auto fixture = Fixture {};
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Unavailable);
}

TEST_CASE("SshAgent: an agent that cannot be reached is unavailable")
{
    auto backend = core::net::makeBackend(core::net::preferredBackendKind());
    auto loop = EventLoop { *backend };

    SECTION("an empty endpoint")
    {
        auto const agent = core::net::makeSshAgent(loop, {});
        auto const keys = loop.blockOn(agent->identities());
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error() == SshAgentError::Unavailable);
    }
    SECTION("an endpoint nobody listens on")
    {
        auto const agent = core::net::makeSshAgent(loop, "/nonexistent/ssh-agent.sock");
        auto const keys = loop.blockOn(agent->identities());
        REQUIRE_FALSE(keys.has_value());
        CHECK(keys.error() == SshAgentError::Unavailable);
    }
}

TEST_CASE("SshAgent.identities: a supported key that is not valid is dropped, not a protocol error")
{
    auto fixture = Fixture {};
    auto const good = keyOf(Ed25519Line);
    auto broken = good.blob;
    broken.pop_back(); // an ssh-ed25519 blob one byte short
    fixture.script->reply =
        frameOf(Msg::IdentitiesAnswer, identitiesBody({ { broken, "broken" }, { good.blob, "good" } }));

    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());

    REQUIRE(keys.has_value());
    REQUIRE(keys->size() == 1);
    CHECK(keys->front().blob == good.blob);
    CHECK(keys->front().comment == "good");
}

TEST_CASE("SshAgent.identities: a count the entries do not back is a protocol error")
{
    auto fixture = Fixture {};
    auto body = std::vector<std::byte> {};
    appendSshUint32(body, 0xFFFFFFFFU);
    fixture.script->reply = frameOf(Msg::IdentitiesAnswer, body);
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Protocol);
}

TEST_CASE("SshAgent.identities: bytes after the last key are a protocol error")
{
    auto fixture = Fixture {};
    auto body = identitiesBody({ { keyOf(Ed25519Line).blob, "k" } });
    body.push_back(std::byte { 0 });
    fixture.script->reply = frameOf(Msg::IdentitiesAnswer, body);
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Protocol);
}

TEST_CASE("SshAgent.identities: SSH_AGENT_FAILURE is a refusal")
{
    auto fixture = Fixture {};
    fixture.script->reply = frameOf(Msg::Failure, {});
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Refused);
}

TEST_CASE("SshAgent.identities: a frame cut off mid-payload is unavailable")
{
    auto fixture = Fixture {};
    auto reply = frameOf(Msg::IdentitiesAnswer, identitiesBody({ { keyOf(Ed25519Line).blob, "k" } }));
    reply.resize(reply.size() - 5);
    fixture.script->reply = std::move(reply);
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Unavailable);
}

TEST_CASE("SshAgent.identities: a zero-length frame is a protocol error")
{
    auto fixture = Fixture {};
    appendSshUint32(fixture.script->reply, 0);
    auto const agent = fixture.agent();
    auto const keys = fixture.loop.blockOn(agent->identities());
    REQUIRE_FALSE(keys.has_value());
    CHECK(keys.error() == SshAgentError::Protocol);
}

TEST_CASE("SshAgent.sign: bytes after the signature are a protocol error")
{
    auto fixture = Fixture {};
    auto body = std::vector<std::byte> {};
    appendSshString(body, std::string_view { "signature" });
    body.push_back(std::byte { 0 });
    fixture.script->reply = frameOf(Msg::SignResponse, body);
    auto const agent = fixture.agent();
    auto const data = std::vector<std::byte> { std::byte { 9 } };
    auto const answer = fixture.loop.blockOn(agent->sign(keyOf(Ed25519Line), data));
    REQUIRE_FALSE(answer.has_value());
    CHECK(answer.error() == SshAgentError::Protocol);
}
