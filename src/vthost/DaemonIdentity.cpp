// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonIdentity.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/net/Tls.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>

#include <vthost/SessionSnapshotStore.hpp>

namespace vthost
{

namespace
{
    constexpr auto PrivateKeyMarker = std::string_view { "PRIVATE KEY-----" };
    constexpr auto BeginMarker = std::string_view { "-----BEGIN " };
    constexpr auto CertificateMarker = std::string_view { "-----BEGIN CERTIFICATE-----" };

    /// @param text A file: certificate(s), then a private key, in PEM.
    /// @return The two parts; nullopt unless it holds a certificate followed by a key.
    [[nodiscard]] std::optional<handover::TlsIdentity> splitIdentity(std::string_view text)
    {
        auto const marker = text.find(PrivateKeyMarker);
        if (marker == std::string_view::npos)
            return std::nullopt;
        auto const keyBegin = text.rfind(BeginMarker, marker);
        if (keyBegin == std::string_view::npos || !text.substr(0, keyBegin).contains(CertificateMarker))
            return std::nullopt;
        return handover::TlsIdentity { .certPem = std::string { text.substr(0, keyBegin) },
                                       .keyPem = std::string { text.substr(keyBegin) } };
    }

    [[nodiscard]] std::expected<void, std::string> makeIdentityFile(
        core::platform::FileSystem const& fileSystem,
        IFilePrivacy const& privacy,
        std::filesystem::path const& file,
        handover::TlsIdentity const& identity)
    {
        auto const directory = file.parent_path();
        auto temporary = file;
        temporary += ".tmp";
        // A crash may have left one, and createNewFile() refuses an existing file.
        std::ignore = fileSystem.remove(temporary);
        return fileSystem.createDirectories(directory)
            .and_then([&] { return privacy.restrictToOwner(directory, PrivateEntry::Directory); })
            .and_then([&] { return fileSystem.createNewFile(temporary); })
            // Restricted while still empty, so the key never exists within anyone else's reach.
            .and_then([&] { return privacy.restrictToOwner(temporary, PrivateEntry::File); })
            .and_then([&] { return fileSystem.writeFile(temporary, identity.certPem + identity.keyPem); })
            .and_then([&] { return fileSystem.syncFile(temporary); })
            .and_then([&] { return fileSystem.replaceFile(temporary, file); })
            .transform_error([&](std::string detail) {
                std::ignore = fileSystem.remove(temporary);
                return std::format(
                    "cannot keep the daemon's TLS identity in '{}': {}", core::pathToUtf8(file), detail);
            });
    }
} // namespace

std::optional<std::filesystem::path> daemonIdentityPath(std::filesystem::path const& stateDirectory,
                                                        std::string_view label)
{
    if (!isFileNameLabel(label))
        return std::nullopt;
    return stateDirectory / "tls" / std::format("{}.pem", label);
}

std::expected<handover::TlsIdentity, std::string> loadOrCreateDaemonIdentity(
    core::platform::FileSystem const& fileSystem,
    IFilePrivacy const& privacy,
    std::filesystem::path const& file)
{
    if (fileSystem.exists(file))
    {
        // Its owner and mode are checked on the file read: whoever else could have written it could pass for
        // this daemon, and whoever could read it holds its key.
        auto const text = privacy.readPrivateFile(file);
        if (!text)
            return std::unexpected(std::format(
                "cannot read the daemon's TLS identity '{}': {}", core::pathToUtf8(file), text.error()));
        auto identity = splitIdentity(*text);
        if (!identity || !core::net::certificateFingerprint(identity->certPem))
            return std::unexpected(std::format("'{}' holds no usable certificate and private key; move it "
                                               "away to have the daemon make a new identity (clients that "
                                               "pinned the old one will be refused)",
                                               core::pathToUtf8(file)));
        return *std::move(identity);
    }

    auto material = core::net::generateSelfSignedCertificate({ .commonName = "contour-daemon" });
    if (!material)
        return std::unexpected(
            std::format("cannot make a TLS certificate for the daemon: {}", material.error()));
    auto identity = handover::TlsIdentity { .certPem = std::move(material->certPem),
                                            .keyPem = std::move(material->keyPem) };
    return makeIdentityFile(fileSystem, privacy, file, identity).transform([&] { return identity; });
}

std::expected<std::string, std::string> daemonFingerprint(handover::TlsIdentity const& identity)
{
    auto const hex = core::net::certificateFingerprint(identity.certPem);
    if (!hex)
        return std::unexpected(hex.error());
    auto const raw = core::fromHexString(std::string_view { *hex });
    constexpr auto DigestSize = std::tuple_size_v<std::array<std::byte, 32>>;
    if (!raw || raw->size() != DigestSize)
        return std::unexpected(std::string { "the certificate's digest is not a SHA-256 one" });
    auto digest = std::array<std::byte, DigestSize> {};
    std::ranges::transform(*raw, digest.begin(), [](char c) { return static_cast<std::byte>(c); });
    return core::net::sshStyleFingerprint(digest);
}

} // namespace vthost
