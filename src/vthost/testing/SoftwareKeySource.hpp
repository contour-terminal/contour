// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A key source for tests that holds software security keys: it offers their public keys, in order, and signs
/// with the one asked for, as an agent would. Shared by the suites that drive a client against a daemon.

#include <core/net/testing/SoftwareSecurityKey.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <vthost/KeyAuth.hpp>
#include <vthost/client/KeySource.hpp>

namespace vthost::testing
{

/// An IClientKeySource over software keys.
class SoftwareKeySource final: public client::IClientKeySource
{
  public:
    /// Makes @p count Ed25519 security keys.
    /// @param count How many.
    explicit SoftwareKeySource(std::size_t count)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, count))
        {
            auto key = core::net::testing::SoftwareSecurityKey::generate(core::net::SshKeyType::SkEd25519);
            auto const line = (*key)->publicKeyLine(std::format("key{}", index));
            _publicKeys.push_back(*core::net::parseSshPublicKey(line));
            _lines.push_back(line);
            _keys.push_back(std::move(*key));
        }
    }

    /// @return The i'th key's public half.
    [[nodiscard]] core::net::SshPublicKey const& publicKey(std::size_t index) const
    {
        return _publicKeys.at(index);
    }

    /// @return The i'th key as an `authorized_keys` line.
    [[nodiscard]] std::string const& authorizedLine(std::size_t index) const { return _lines.at(index); }

    /// @return How many signatures were asked for.
    [[nodiscard]] std::size_t signCount() const noexcept { return _signCount; }

    [[nodiscard]] core::async::Task<std::expected<std::vector<core::net::SshPublicKey>, std::string>>
    candidates() override
    {
        co_return _publicKeys;
    }

    [[nodiscard]] core::async::Task<std::expected<std::string, std::string>> sign(
        core::net::SshPublicKey const& key, std::span<std::byte const> message) override
    {
        ++_signCount;
        return answer(signNow(key, message));
    }

  private:
    /// @return What @p key signs @p message as, at once.
    [[nodiscard]] std::expected<std::string, std::string> signNow(core::net::SshPublicKey const& key,
                                                                  std::span<std::byte const> message) const
    {
        auto const found = std::ranges::find(_publicKeys, key.blob, &core::net::SshPublicKey::blob);
        if (found == _publicKeys.end())
            return std::unexpected(std::string { "no such key" });
        auto const signedData = core::net::sshsigSignedData(KeyAuthNamespace, message);
        auto const index = static_cast<std::size_t>(found - _publicKeys.begin());
        auto const agentSignature =
            _keys[index]->sign(signedData, core::net::testing::UserPresence::Present, 1);
        if (!agentSignature)
            return std::unexpected(agentSignature.error());
        return core::net::assembleSshsig(key, KeyAuthNamespace, *agentSignature);
    }

    /// @return A task that has already finished with @p result.
    [[nodiscard]] static core::async::Task<std::expected<std::string, std::string>> answer(
        std::expected<std::string, std::string> result)
    {
        co_return result;
    }

    std::vector<std::unique_ptr<core::net::testing::SoftwareSecurityKey>> _keys;
    std::vector<core::net::SshPublicKey> _publicKeys;
    std::vector<std::string> _lines;
    std::size_t _signCount = 0;
};

} // namespace vthost::testing
