// SPDX-License-Identifier: Apache-2.0
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/testing/CoroTestSupport.hpp>
#include <core/net/testing/InMemoryTransport.hpp>
#include <core/net/testing/SoftwareSecurityKey.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <expected>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/KeyAuth.hpp>
#include <vthost/client/KeySource.hpp>
#include <vthost/testing/SoftwareKeySource.hpp>

using core::async::Task;

namespace
{

/// An agent holding the public keys it is given, and signing with one software key (or refusing).
class FakeAgent final: public core::net::ISshAgent
{
  public:
    FakeAgent(std::vector<core::net::SshPublicKey> keys,
              core::net::testing::SoftwareSecurityKey const* signer):
        _keys(std::move(keys)), _signer(signer)
    {
    }

    Task<std::expected<std::vector<core::net::SshPublicKey>, core::net::SshAgentError>> identities() override
    {
        co_return _keys;
    }

    Task<std::expected<std::vector<std::byte>, core::net::SshAgentError>> sign(
        core::net::SshPublicKey const& /*key*/, std::span<std::byte const> data) override
    {
        // Answered at once, from what it holds: no reference of the caller's is kept across a suspension.
        auto answer = std::expected<std::vector<std::byte>, core::net::SshAgentError> { std::unexpected(
            core::net::SshAgentError::Refused) };
        if (_signer != nullptr)
            if (auto signature = _signer->sign(data, core::net::testing::UserPresence::Present, 1))
                answer = *std::move(signature);
        return ready(std::move(answer));
    }

  private:
    static Task<std::expected<std::vector<std::byte>, core::net::SshAgentError>> ready(
        std::expected<std::vector<std::byte>, core::net::SshAgentError> answer)
    {
        co_return answer;
    }

    std::vector<core::net::SshPublicKey> _keys;
    core::net::testing::SoftwareSecurityKey const* _signer;
};

/// A child that has exited with a scripted code.
class EndedChild final: public vthost::IStdioChild
{
  public:
    EndedChild(int code, int slowPolls): _code(code), _slowPolls(slowPolls) {}
    [[nodiscard]] std::string stderrTail() const override { return "boom"; }
    [[nodiscard]] std::optional<int> exitCode(std::chrono::milliseconds /*wait*/) override
    {
        // Not reaped yet for the first polls, as on a loaded machine.
        return _slowPolls-- > 0 ? std::nullopt : std::optional { _code };
    }
    void terminate() noexcept override {}

  private:
    int _code;
    int _slowPolls;
};

/// A spawner that records argv and hands out prepared transports.
class RecordingSpawner final: public vthost::IStdioSpawner
{
  public:
    std::deque<std::unique_ptr<core::net::ISocket>> prepared;
    std::vector<std::vector<std::string>> spawned;
    int slowPolls = 0; ///< How many times each child answers that it still runs.

    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& /*loop*/,
        std::span<std::string const> argv,
        vthost::SpawnEnvironment const& /*environment*/) override
    {
        spawned.emplace_back(argv.begin(), argv.end());
        auto transport = std::move(prepared.front());
        prepared.pop_front();
        return vthost::SpawnedStdioChild { .transport = std::move(transport),
                                           .process = std::make_unique<EndedChild>(0, slowPolls) };
    }
};

/// The far end of ssh-keygen: reads its stdin to the end, keeps it, and answers with @p reply.
Task<void> keygenRemote(core::net::ISocket* socket, std::string reply, std::string* stdinSeen)
{
    auto buffer = std::vector<std::byte>(1024);
    while (true)
    {
        auto const count = co_await socket->read(buffer);
        if (!count || *count == 0)
            break;
        stdinSeen->append(reinterpret_cast<char const*>(buffer.data()), *count);
    }
    auto const bytes = std::as_bytes(std::span { reply });
    std::ignore = co_await socket->write(bytes);
    socket->close();
}

Task<void> signInto(vthost::client::IClientKeySource* source,
                    core::net::SshPublicKey key,
                    std::vector<std::byte> message,
                    std::expected<std::string, std::string>* out)
{
    *out = co_await source->sign(key, message);
}

Task<void> candidatesInto(vthost::client::IClientKeySource* source,
                          std::expected<std::vector<core::net::SshPublicKey>, std::string>* out)
{
    *out = co_await source->candidates();
}

struct Keys
{
    std::unique_ptr<core::net::testing::SoftwareSecurityKey> first =
        *core::net::testing::SoftwareSecurityKey::generate(core::net::SshKeyType::SkEd25519);
    std::unique_ptr<core::net::testing::SoftwareSecurityKey> second =
        *core::net::testing::SoftwareSecurityKey::generate(core::net::SshKeyType::SkEd25519);
    core::net::SshPublicKey firstPublic = *core::net::parseSshPublicKey(first->publicKeyLine("one"));
    core::net::SshPublicKey secondPublic = *core::net::parseSshPublicKey(second->publicKeyLine("two"));
};

constexpr auto Message = std::array { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } };

} // namespace

TEST_CASE("an identity file narrows the offered keys to its own", "[vthost][keysource]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = Keys {};
    auto const files = core::platform::testing::InMemoryFileSystem {
        { .path = "/home/me/.ssh/id_work.pub", .content = keys.second->publicKeyLine("work") + "\n" }
    };
    auto const agent =
        std::make_shared<FakeAgent>(std::vector { keys.firstPublic, keys.secondPublic }, keys.first.get());

    auto narrowed = vthost::client::makeClientKeySource(
        loop, agent, std::make_shared<RecordingSpawner>(), nullptr, files, "/home/me/.ssh/id_work", {});
    auto offered = std::expected<std::vector<core::net::SshPublicKey>, std::string> {};
    loop.blockOn(candidatesInto(narrowed.get(), &offered));
    REQUIRE(offered);
    REQUIRE(offered->size() == 1);
    CHECK(offered->front().blob == keys.secondPublic.blob);

    auto all = vthost::client::makeClientKeySource(
        loop, agent, std::make_shared<RecordingSpawner>(), nullptr, files, {}, {});
    loop.blockOn(candidatesInto(all.get(), &offered));
    REQUIRE(offered);
    CHECK(offered->size() == 2);

    // A named key whose .pub cannot be read says which file.
    auto missing = vthost::client::makeClientKeySource(
        loop, agent, std::make_shared<RecordingSpawner>(), nullptr, files, "/home/me/.ssh/nope", {});
    loop.blockOn(candidatesInto(missing.get(), &offered));
    REQUIRE_FALSE(offered);
    CHECK(offered.error().contains("/home/me/.ssh/nope.pub"));
}

TEST_CASE("the agent signs a key proof that verifies in the contour-auth namespace", "[vthost][keysource]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = Keys {};
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto const spawner = std::make_shared<RecordingSpawner>();
    auto signer = vthost::client::makeClientKeySource(
        loop,
        std::make_shared<FakeAgent>(std::vector { keys.firstPublic }, keys.first.get()),
        spawner,
        nullptr,
        files,
        {},
        {});

    auto signature = std::expected<std::string, std::string> {};
    loop.blockOn(signInto(signer.get(), keys.firstPublic, { Message.begin(), Message.end() }, &signature));
    REQUIRE(signature);
    CHECK(core::net::verifySshSignature(keys.firstPublic, vthost::KeyAuthNamespace, Message, *signature)
              .has_value());
    CHECK(spawner->spawned.empty());
}

TEST_CASE("a refusing agent falls back to ssh-keygen for the named key file", "[vthost][keysource][afunix]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = Keys {};
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto spawner = std::make_shared<RecordingSpawner>();
    // ssh-keygen is not reaped at once: a good signature is still taken.
    spawner->slowPolls = 5;
    auto pair = *core::net::testing::makeSocketPair(loop);
    spawner->prepared.push_back(std::move(pair.second));

    auto signer = vthost::client::makeClientKeySource(
        loop,
        std::make_shared<FakeAgent>(std::vector { keys.firstPublic }, nullptr), // refuses
        spawner,
        nullptr,
        files,
        "/home/me/.ssh/id_work",
        {});

    auto signature = std::expected<std::string, std::string> {};
    auto stdinSeen = std::string {};
    loop.blockOn(core::net::testing::allOf(
        signInto(signer.get(), keys.firstPublic, { Message.begin(), Message.end() }, &signature),
        keygenRemote(pair.first.get(), "-----BEGIN SSH SIGNATURE-----\nABC\n", &stdinSeen)));

    REQUIRE(signature);
    CHECK(*signature == "-----BEGIN SSH SIGNATURE-----\nABC\n");
    CHECK(stdinSeen == std::string { reinterpret_cast<char const*>(Message.data()), Message.size() });
    REQUIRE(spawner->spawned.size() == 1);
    CHECK(spawner->spawned.front()
          == std::vector<std::string> {
              "ssh-keygen", "-Y", "sign", "-n", "contour-auth", "-f", "/home/me/.ssh/id_work" });
}

TEST_CASE("a refusing agent without a named key file signs nothing", "[vthost][keysource]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = Keys {};
    auto const files = core::platform::testing::InMemoryFileSystem {};
    auto spawner = std::make_shared<RecordingSpawner>();
    auto signer = vthost::client::makeClientKeySource(
        loop,
        std::make_shared<FakeAgent>(std::vector { keys.firstPublic }, nullptr),
        spawner,
        nullptr,
        files,
        {},
        {});
    auto signature = std::expected<std::string, std::string> {};
    loop.blockOn(signInto(signer.get(), keys.firstPublic, { Message.begin(), Message.end() }, &signature));
    CHECK_FALSE(signature);
    CHECK(spawner->spawned.empty());
}

namespace
{
/// Answers @p count challenges in a row, each with a fresh nonce, and records every proof or its absence.
Task<void> answerChallenges(vthost::client::KeyAuthenticator* authenticator,
                            std::size_t count,
                            std::vector<bool>* gotProof)
{
    for ([[maybe_unused]] auto const round: std::views::iota(std::size_t { 0 }, count))
    {
        auto const proof =
            co_await authenticator->proofFor(vthost::proto::AuthChallenge {}, std::array<std::byte, 32> {});
        gotProof->push_back(proof.has_value());
    }
}
} // namespace

TEST_CASE("a key authenticator offers at most five keys, however many challenges come",
          "[vthost][keysource][keyauth]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = std::make_shared<vthost::testing::SoftwareKeySource>(6);
    auto authenticator = vthost::client::KeyAuthenticator { keys };
    auto gotProof = std::vector<bool> {};
    loop.blockOn(answerChallenges(&authenticator, 7, &gotProof));

    CHECK(gotProof == std::vector<bool> { true, true, true, true, true, false, false });
    CHECK(keys->signCount() == 5);
    // The sixth key was never offered.
    CHECK_FALSE(
        authenticator.explainRefusal("refused").contains(core::net::sshFingerprintOf(keys->publicKey(5))));
}

namespace
{
/// The software keys, but the ones at @c refused cannot sign, as a key whose passphrase the user cancelled.
class PartlyRefusingKeySource final: public vthost::client::IClientKeySource
{
  public:
    PartlyRefusingKeySource(std::shared_ptr<vthost::testing::SoftwareKeySource> keys,
                            std::vector<std::size_t> refused):
        _keys(std::move(keys)), _refused(std::move(refused))
    {
    }

    Task<std::expected<std::vector<core::net::SshPublicKey>, std::string>> candidates() override
    {
        return _keys->candidates();
    }

    Task<std::expected<std::string, std::string>> sign(core::net::SshPublicKey const& key,
                                                       std::span<std::byte const> message) override
    {
        auto const refuses = std::ranges::any_of(
            _refused, [&](std::size_t index) { return _keys->publicKey(index).blob == key.blob; });
        if (refuses)
            return refuse();
        return _keys->sign(key, message);
    }

  private:
    static Task<std::expected<std::string, std::string>> refuse()
    {
        co_return std::unexpected(std::string { "the passphrase was cancelled" });
    }

    std::shared_ptr<vthost::testing::SoftwareKeySource> _keys;
    std::vector<std::size_t> _refused;
};
} // namespace

TEST_CASE("a key that cannot sign counts toward neither the cap nor the keys offered",
          "[vthost][keysource][keyauth]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = std::make_shared<vthost::testing::SoftwareKeySource>(7);
    auto authenticator = vthost::client::KeyAuthenticator { std::make_shared<PartlyRefusingKeySource>(
        keys, std::vector<std::size_t> { 0, 2 }) };
    auto gotProof = std::vector<bool> {};
    loop.blockOn(answerChallenges(&authenticator, 7, &gotProof));

    // Five proofs reached the daemon, by keys 1, 3, 4, 5 and 6; the two that could not sign used up nothing.
    CHECK(gotProof == std::vector<bool> { true, true, true, true, true, false, false });
    auto const explained = authenticator.explainRefusal("refused");
    for (auto const index: { 1U, 3U, 4U, 5U, 6U })
        CHECK(explained.contains(core::net::sshFingerprintOf(keys->publicKey(index))));
    for (auto const index: { 0U, 2U })
        CHECK_FALSE(explained.contains(core::net::sshFingerprintOf(keys->publicKey(index))));
}

TEST_CASE("a key authenticator gives no proof where there is no channel binding",
          "[vthost][keysource][keyauth]")
{
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto const keys = std::make_shared<vthost::testing::SoftwareKeySource>(1);
    auto authenticator = vthost::client::KeyAuthenticator { keys };
    auto proof = std::optional<vthost::proto::KeyProof> {};
    loop.blockOn(
        [](vthost::client::KeyAuthenticator* a, std::optional<vthost::proto::KeyProof>* out) -> Task<void> {
            *out = co_await a->proofFor(vthost::proto::AuthChallenge {}, std::nullopt);
        }(&authenticator, &proof));
    CHECK_FALSE(proof);
    CHECK(keys->signCount() == 0);
}
