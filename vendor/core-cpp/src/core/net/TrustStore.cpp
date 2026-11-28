// SPDX-License-Identifier: Apache-2.0
#include <core/net/TrustStore.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/Offload.hpp>
#include <core/net/Tls.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core::net
{

namespace
{
    /// Every place a system keeps its anchors, bundles first. A row is a fact about a distribution,
    /// so a new one is a line here and nothing else.
    constexpr auto Locations = std::array {
        TrustBundleLocation { .path = "/etc/ssl/certs/ca-certificates.crt",
                              .distribution = "Debian, Ubuntu, Arch, Gentoo",
                              .kind = TrustBundleKind::Bundle },
        TrustBundleLocation { .path = "/etc/pki/tls/certs/ca-bundle.crt",
                              .distribution = "Fedora, RHEL",
                              .kind = TrustBundleKind::Bundle },
        TrustBundleLocation { .path = "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                              .distribution = "RHEL 7 and later",
                              .kind = TrustBundleKind::Bundle },
        TrustBundleLocation {
            .path = "/etc/ssl/ca-bundle.pem", .distribution = "openSUSE", .kind = TrustBundleKind::Bundle },
        TrustBundleLocation { .path = "/etc/ssl/cert.pem",
                              .distribution = "Alpine, FreeBSD with ca_root_nss, OpenBSD",
                              .kind = TrustBundleKind::Bundle },
        TrustBundleLocation { .path = "/usr/local/share/certs/ca-root-nss.crt",
                              .distribution = "FreeBSD's ca_root_nss package",
                              .kind = TrustBundleKind::Bundle },
        TrustBundleLocation { .path = "/etc/ssl/certs",
                              .distribution = "FreeBSD's certctl, and any hashed system directory",
                              .kind = TrustBundleKind::HashedDirectory },
    };

    /// The environment variables OpenSSL reads, in the order they decide.
    struct TrustVariable
    {
        std::string_view name;
        TrustBundleKind kind;
    };

    constexpr auto Variables = std::array {
        TrustVariable { .name = "SSL_CERT_FILE", .kind = TrustBundleKind::Bundle },
        TrustVariable { .name = "SSL_CERT_DIR", .kind = TrustBundleKind::HashedDirectory },
    };

    /// Whether @p name is what OpenSSL looks a certificate up by in a hashed directory:
    /// `<8 lower-case hex digits>.<decimal>`.
    [[nodiscard]] bool isHashedCertificateName(std::string_view name) noexcept
    {
        constexpr auto HashDigits = std::size_t { 8 };
        if (name.size() < HashDigits + 2 || name[HashDigits] != '.')
            return false;
        auto const isLowerHex = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        };
        auto const isDecimal = [](char c) {
            return c >= '0' && c <= '9';
        };
        return std::ranges::all_of(name.substr(0, HashDigits), isLowerHex)
               && std::ranges::all_of(name.substr(HashDigits + 1), isDecimal);
    }

    /// The store @c makePemTrustStore answers: its bundle, parsed on the caller's executor.
    class PemTrustStore final: public ITrustStore
    {
      public:
        explicit PemTrustStore(std::string pem) noexcept: _pem { std::move(pem) } {}

        [[nodiscard]] async::Task<std::expected<TrustAnchors, std::string>> load() override
        {
            co_return trustAnchorsFromPem(_pem);
        }

      private:
        std::string _pem;
    };

    /// The store @c makeFileTrustStore answers.
    class FileTrustStore final: public ITrustStore
    {
      public:
        FileTrustStore(platform::FileSystem const& files,
                       Environment const& environment,
                       async::IExecutor& home,
                       async::IExecutor& blocking) noexcept:
            _files { &files }, _environment { &environment }, _home { &home }, _blocking { &blocking }
        {
        }

        [[nodiscard]] async::Task<std::expected<TrustAnchors, std::string>> load() override
        {
            co_return co_await async::offload(_home, _blocking, [this] { return loadNow(); });
        }

      private:
        /// Decides the source and reads it, on the blocking executor.
        [[nodiscard]] std::expected<TrustAnchors, std::string> loadNow() const
        {
            for (auto const& variable: Variables)
            {
                auto value = _environment->get(variable.name);
                if (!value || value->empty())
                    continue;
                // Named means chosen: a file that is not there is an error, never a fallback.
                return read(*value, variable.kind, std::string { variable.name } + " names");
            }
            for (auto const& location: Locations)
            {
                auto const path = std::filesystem::path { location.path };
                auto const present = location.kind == TrustBundleKind::Bundle ? _files->isRegularFile(path)
                                                                              : _files->isDirectory(path);
                if (present)
                    return read(std::string { location.path },
                                location.kind,
                                "the trust store of " + std::string { location.distribution } + " at");
            }
            return std::unexpected(std::string {
                "no trust store: SSL_CERT_FILE and SSL_CERT_DIR are unset, and none of the known bundle "
                "locations exists" });
        }

        /// Reads one source, prefixing a failure with @p origin and the path.
        [[nodiscard]] std::expected<TrustAnchors, std::string> read(std::string const& path,
                                                                    TrustBundleKind kind,
                                                                    std::string const& origin) const
        {
            auto anchors = kind == TrustBundleKind::Bundle ? readBundle(path, MaxTrustBundleBytes)
                                                           : readHashedDirectory(path);
            if (!anchors)
                return std::unexpected(origin + " '" + path + "': " + anchors.error());
            return anchors;
        }

        /// The size of the regular file at @p path, refused above @p limit before anything is read.
        [[nodiscard]] std::expected<std::uintmax_t, std::string> boundedSize(
            std::filesystem::path const& path, std::uintmax_t limit) const
        {
            // A regular file and nothing else: a device or a FIFO named here would be read for ever.
            if (!_files->isRegularFile(path))
                return std::unexpected(std::string { "not a readable regular file" });
            auto size = _files->fileSize(path);
            if (!size)
                return std::unexpected(std::move(size.error()));
            if (*size > limit)
                return std::unexpected(
                    std::format("{} bytes, more than the {} a trust store reads", *size, limit));
            return size;
        }

        [[nodiscard]] std::expected<TrustAnchors, std::string> readBundle(std::filesystem::path const& path,
                                                                          std::uintmax_t limit) const
        {
            return boundedSize(path, limit)
                .and_then([&](std::uintmax_t /*size*/) { return _files->readFile(path); })
                .and_then([](std::string const& pem) { return trustAnchorsFromPem(pem); });
        }

        [[nodiscard]] std::expected<TrustAnchors, std::string> readHashedDirectory(
            std::filesystem::path const& path) const
        {
            auto entries = _files->listDirectory(path);
            if (!entries)
                return std::unexpected(std::move(entries.error()));
            auto names = std::vector<std::filesystem::path> {};
            for (auto const& entry: *entries)
                if (!entry.isDirectory && isHashedCertificateName(core::pathToUtf8(entry.path.filename())))
                    names.push_back(entry.path);
            // Every size before any read, so a directory over its budget is refused unread.
            auto total = std::uintmax_t { 0 };
            for (auto const& name: names)
            {
                auto const size = boundedSize(name, MaxTrustBundleBytes);
                if (!size)
                    return std::unexpected(core::pathToUtf8(name.filename()) + ": " + size.error());
                total += *size;
                if (total > MaxTrustDirectoryBytes)
                    return std::unexpected(
                        std::format("its hashed files hold more than the {} bytes a trust store reads",
                                    MaxTrustDirectoryBytes));
            }
            auto anchors = TrustAnchors {};
            for (auto const& name: names)
            {
                // Every hashed file, or none: one that does not read is a refusal, not a gap.
                auto one = readBundle(name, MaxTrustBundleBytes);
                if (!one)
                    return std::unexpected(core::pathToUtf8(name.filename()) + ": " + one.error());
                std::ranges::move(one->certificates, std::back_inserter(anchors.certificates));
            }
            if (anchors.certificates.empty())
                return std::unexpected(std::string { "no hashed certificate (<hash>.<n>) in the directory" });
            return anchors;
        }

        platform::FileSystem const* _files;
        Environment const* _environment;
        async::IExecutor* _home;
        async::IExecutor* _blocking;
    };
} // namespace

std::unique_ptr<ITrustStore> makePemTrustStore(std::string pem)
{
    return std::make_unique<PemTrustStore>(std::move(pem));
}

std::span<TrustBundleLocation const> trustBundleLocations() noexcept
{
    return Locations;
}

std::unique_ptr<ITrustStore> makeFileTrustStore(platform::FileSystem const& files,
                                                Environment const& environment,
                                                async::IExecutor& home,
                                                async::IExecutor& blocking)
{
    return std::make_unique<FileTrustStore>(files, environment, home, blocking);
}

} // namespace core::net
