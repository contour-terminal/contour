// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/HostTransport.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/FileSystem.hpp>

#include <format>
#include <utility>

#include <vthost/Daemon.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/Token.hpp>
#include <vthost/Transport.hpp>

namespace contour::cli
{

std::expected<std::string, std::string> readPinnedCa(core::platform::FileSystem const& fileSystem,
                                                     std::filesystem::path const& path,
                                                     std::string_view origin)
{
    auto pem = fileSystem.readFile(path).value_or(std::string {});
    if (pem.empty())
        return std::unexpected { std::format("cannot read {} file '{}'", origin, core::pathToUtf8(path)) };
    return pem;
}

namespace
{
    /// @return The ssh transport to @p host.
    [[nodiscard]] HostTransport sshHostTransport(std::string label,
                                                 config::HostEntry const& host,
                                                 std::shared_ptr<vthost::IStdioSpawner> spawner)
    {
        auto target =
            vthost::SshTarget { .user = host.url.user, .host = host.url.host, .port = host.url.port };
        if (!host.remoteCommand.empty())
            target.remoteCommand = host.remoteCommand;
        return HostTransport {
            .factory = std::make_shared<vthost::SshTransportFactory>(std::move(target), std::move(spawner)),
            .label = std::move(label),
            .connectTimeout = sshAttachBudget(vthost::SshConnectTimeout),
        };
    }

    /// @return The TLS transport to @p host, its token read and its CA, if it names one, too.
    [[nodiscard]] std::expected<HostTransport, std::string> tlsHostTransport(
        std::string label, config::HostEntry const& host, core::platform::FileSystem const& fileSystem)
    {
        // A TCP daemon always requires a token.
        if (host.tokenFile.empty())
            return std::unexpected { std::format("{}: a tls:// host needs token_file", label) };
        auto token =
            fileSystem.readFile(host.tokenFile)
                .transform_error([&](std::string const&) {
                    return std::format("cannot open token file '{}'", core::pathToUtf8(host.tokenFile));
                })
                .and_then([&](std::string content) {
                    return vthost::tokenFromFileContent(std::move(content), host.tokenFile);
                });
        if (!token)
            return std::unexpected { token.error() };
        auto endpoint = vthost::TcpEndpoint {
            .host = host.url.host, .port = host.url.port.value_or(0), .token = *std::move(token), .caPem = {}
        };
        if (!host.tlsCa.empty())
        {
            auto pem = readPinnedCa(fileSystem, host.tlsCa, "tls_ca");
            if (!pem)
                return std::unexpected { pem.error() };
            endpoint.caPem = *std::move(pem);
        }
        return HostTransport {
            .factory = std::make_shared<vthost::TlsTransportFactory>(std::move(endpoint)),
            .label = std::move(label),
            .connectTimeout = TlsHostConnectTimeout,
        };
    }
} // namespace

std::expected<HostTransport, std::string> makeHostTransport(std::string label,
                                                            config::HostEntry const& host,
                                                            core::platform::FileSystem const& fileSystem,
                                                            std::shared_ptr<vthost::IStdioSpawner> spawner)
{
    switch (host.url.scheme)
    {
        case config::HostScheme::Ssh: return sshHostTransport(std::move(label), host, std::move(spawner));
        case config::HostScheme::Tls: return tlsHostTransport(std::move(label), host, fileSystem);
    }
    return std::unexpected { std::format("{}: the host's scheme is not supported", label) };
}

} // namespace contour::cli
