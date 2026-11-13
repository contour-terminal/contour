// SPDX-License-Identifier: Apache-2.0
#include <vthost/client/KeySource.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/Tls.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

#include <vthost/KeyAuth.hpp>
#include <vthost/Logging.hpp>

namespace vthost::client
{

namespace
{
    /// How often a child whose output ended is asked whether it exited.
    constexpr auto ExitPoll = std::chrono::milliseconds { 10 };

    /// @param identityFile A private key file.
    /// @return The `.pub` file beside it.
    [[nodiscard]] std::filesystem::path publicKeyPathOf(std::filesystem::path const& identityFile)
    {
        auto path = identityFile;
        path += ".pub";
        return path;
    }

    class ClientKeySource final: public IClientKeySource
    {
      public:
        ClientKeySource(core::net::EventLoop& loop,
                        std::shared_ptr<core::net::ISshAgent> agent,
                        std::shared_ptr<IStdioSpawner> spawner,
                        std::shared_ptr<IAskpassBroker> broker,
                        core::platform::FileSystem const& fileSystem,
                        std::filesystem::path identityFile,
                        SshTarget askpassTarget):
            _loop(loop),
            _agent(std::move(agent)),
            _spawner(std::move(spawner)),
            _broker(std::move(broker)),
            _fileSystem(fileSystem),
            _identityFile(std::move(identityFile)),
            _askpassTarget(std::move(askpassTarget))
        {
        }

        core::async::Task<std::expected<std::vector<core::net::SshPublicKey>, std::string>> candidates()
            override
        {
            if (!_identityFile.empty())
                co_return namedKey().transform([](auto key) { return std::vector { std::move(key) }; });
            if (!_agent)
                co_return std::unexpected(std::string { "no SSH agent is running" });
            auto identities = co_await _agent->identities();
            if (!identities)
                co_return std::unexpected(std::string { "the SSH agent could not be asked for its keys" });
            co_return std::move(*identities);
        }

        core::async::Task<std::expected<std::string, std::string>> sign(
            core::net::SshPublicKey const& key, std::span<std::byte const> message) override
        {
            // Copied, as a coroutine must not hold the caller's references across a suspension.
            return signMessage(key, std::vector<std::byte> { message.begin(), message.end() });
        }

      private:
        /// @return The key the identity file's `.pub` holds.
        [[nodiscard]] std::expected<core::net::SshPublicKey, std::string> namedKey() const
        {
            auto const path = publicKeyPathOf(_identityFile);
            auto const content = _fileSystem.readFile(path);
            if (!content)
                return std::unexpected(
                    std::format("cannot read {}: {}", core::pathToUtf8(path), content.error()));
            auto const line = std::string_view { *content }.substr(0, content->find('\n'));
            auto key = core::net::parseSshPublicKey(line);
            if (!key)
                return std::unexpected(
                    std::format("{} holds no supported SSH public key", core::pathToUtf8(path)));
            return *std::move(key);
        }

        /// Signs @p message with @p key: through the agent, else by `ssh-keygen` for the named key file.
        [[nodiscard]] core::async::Task<std::expected<std::string, std::string>> signMessage(
            core::net::SshPublicKey key, std::vector<std::byte> message)
        {
            if (_agent)
            {
                auto const signedData = core::net::sshsigSignedData(KeyAuthNamespace, message);
                auto signature = co_await _agent->sign(key, signedData);
                if (signature)
                    co_return core::net::assembleSshsig(key, KeyAuthNamespace, *signature);
                clientLog()("key auth: the agent did not sign with {}", core::net::sshFingerprintOf(key));
            }
            // Only a key the user named has a file to fall back to: an agent's other keys do not.
            if (_identityFile.empty())
                co_return std::unexpected(std::string { "the SSH agent did not sign" });
            co_return co_await signWithKeygen(std::move(message));
        }

        /// Runs `ssh-keygen -Y sign` over the identity file with @p message on its stdin.
        [[nodiscard]] core::async::Task<std::expected<std::string, std::string>> signWithKeygen(
            std::vector<std::byte> message)
        {
            auto session = std::unique_ptr<IAskpassSession> {};
            if (_broker)
                if (auto const serving = co_await _broker->whenServing(); serving)
                    if (auto opened = _broker->open(_askpassTarget))
                        session = *std::move(opened);
            auto const argv = std::vector<std::string> { "ssh-keygen",
                                                         "-Y",
                                                         "sign",
                                                         "-n",
                                                         std::string { KeyAuthNamespace },
                                                         "-f",
                                                         core::pathToUtf8(_identityFile) };
            auto spawned =
                _spawner->spawn(_loop, argv, session ? session->environment() : SpawnEnvironment {});
            if (!spawned)
                co_return std::unexpected(
                    std::format("ssh-keygen could not be run: {}", spawned.error().message));
            auto& socket = *spawned->transport;
            if (auto const written = co_await socket.write(std::span<std::byte const> { message }); !written)
                co_return std::unexpected(std::string { "ssh-keygen did not take the message" });
            std::ignore = co_await socket.shutdownWrite();
            auto output = std::string {};
            auto buffer = std::vector<std::byte>(4096);
            while (true)
            {
                auto const count = co_await socket.read(buffer);
                if (!count || *count == 0)
                    break;
                output.append(reinterpret_cast<char const*>(buffer.data()), *count);
            }
            // The child's output ended, so it is exiting; its status is polled without blocking the loop, and
            // only the attempt's own cancellation (the handshake deadline, a detach) bounds the wait, so that
            // a slow machine never costs a good signature.
            auto exitCode = spawned->process->exitCode(std::chrono::milliseconds { 0 });
            while (!exitCode)
            {
                co_await _loop.delay(ExitPoll);
                exitCode = spawned->process->exitCode(std::chrono::milliseconds { 0 });
            }
            if (*exitCode != 0 || output.empty())
            {
                auto const cancelled = session && session->wasCancelled();
                co_return std::unexpected(
                    cancelled ? std::string { "the passphrase prompt was cancelled" }
                              : std::format("ssh-keygen did not sign: {}", spawned->process->stderrTail()));
            }
            co_return output;
        }

        core::net::EventLoop& _loop;
        std::shared_ptr<core::net::ISshAgent> _agent;
        std::shared_ptr<IStdioSpawner> _spawner;
        std::shared_ptr<IAskpassBroker> _broker;
        core::platform::FileSystem const& _fileSystem;
        std::filesystem::path _identityFile;
        SshTarget _askpassTarget;
    };
} // namespace

std::shared_ptr<IClientKeySource> makeClientKeySource(core::net::EventLoop& loop,
                                                      std::shared_ptr<core::net::ISshAgent> agent,
                                                      std::shared_ptr<IStdioSpawner> spawner,
                                                      std::shared_ptr<IAskpassBroker> broker,
                                                      core::platform::FileSystem const& fileSystem,
                                                      std::filesystem::path identityFile,
                                                      SshTarget askpassTarget)
{
    return std::make_shared<ClientKeySource>(loop,
                                             std::move(agent),
                                             std::move(spawner),
                                             std::move(broker),
                                             fileSystem,
                                             std::move(identityFile),
                                             std::move(askpassTarget));
}

ClientKeySourceProvider makeClientKeySourceProvider(std::string const& agentEndpoint,
                                                    std::shared_ptr<IStdioSpawner> const& spawner,
                                                    std::shared_ptr<IAskpassBroker> const& broker,
                                                    core::platform::FileSystem const& fileSystem,
                                                    std::filesystem::path const& identityFile,
                                                    SshTarget const& askpassTarget)
{
    return [=, &fileSystem](core::net::EventLoop& loop) {
        auto const& endpoint = agentEndpoint;
        auto agent = endpoint.empty()
                         ? std::shared_ptr<core::net::ISshAgent> {}
                         : std::shared_ptr<core::net::ISshAgent> { core::net::makeSshAgent(loop, endpoint) };
        return makeClientKeySource(
            loop, std::move(agent), spawner, broker, fileSystem, identityFile, askpassTarget);
    };
}

KeyAuthenticator::KeyAuthenticator(std::shared_ptr<IClientKeySource> source): _source(std::move(source))
{
}

std::optional<std::array<std::byte, 32>> channelBindingOf(core::net::ISocket const& connection)
{
    auto const* const session = core::net::tlsSessionOf(connection);
    if (session == nullptr)
        return std::nullopt;
    auto binding = session->channelBinding();
    if (!binding)
        return std::nullopt;
    return *binding;
}

core::async::Task<std::optional<proto::KeyProof>> KeyAuthenticator::proofFor(
    proto::AuthChallenge challenge, std::optional<std::array<std::byte, 32>> channelBinding)
{
    ++_challenges;
    if (!_candidates)
    {
        auto found = co_await _source->candidates();
        if (!found)
            _whyNone = std::move(found.error());
        _candidates = found ? std::move(*found) : std::vector<core::net::SshPublicKey> {};
    }
    // Only a TLS session has a channel binding to bind the proof to.
    if (!channelBinding)
        co_return std::nullopt;
    auto const message = keyAuthMessage(challenge.nonce, *channelBinding);

    // A key that cannot sign (the agent refused it, the user cancelled its passphrase) is passed over: the
    // challenge is answered by the next one that can. Only the proofs sent count toward the daemon's cap, and
    // only their keys were offered: a key that never reached the daemon is neither.
    while (_next < _candidates->size() && _offered.size() < MaxKeyAuthAttempts)
    {
        auto const& key = (*_candidates)[_next++];
        auto signature = co_await _source->sign(key, message);
        if (signature)
        {
            _offered.push_back(core::net::sshFingerprintOf(key));
            co_return proto::KeyProof { .publicKey = key.blob, .signature = std::move(*signature) };
        }
        clientLog()("key auth: {} could not sign: {}", core::net::sshFingerprintOf(key), signature.error());
        _whyNone = std::move(signature.error());
    }
    co_return std::nullopt;
}

std::string KeyAuthenticator::explainRefusal(std::string refusal) const
{
    if (_challenges == 0)
        return refusal;
    auto const hint = std::string_view { "set identity_file on this host to choose the key" };
    if (_offered.empty())
        return std::format("{}; no SSH key was offered ({}): {}",
                           refusal,
                           _whyNone.empty() ? std::string { "the agent holds none" } : _whyNone,
                           hint);
    auto fingerprints = std::string {};
    for (auto const& fingerprint: _offered)
        fingerprints += (fingerprints.empty() ? "" : ", ") + fingerprint;
    return std::format(
        "{}; keys offered: {}. If none is authorized on the daemon, {}", refusal, fingerprints, hint);
}

} // namespace vthost::client
