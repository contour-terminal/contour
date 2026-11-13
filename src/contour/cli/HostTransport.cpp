// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/HostTransport.hpp>

#include <core/Utf8Path.hpp>
#include <core/platform/FileSystem.hpp>

#include <format>
#include <utility>

#include <vthost/Askpass.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/PromptPause.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/Token.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/KeySource.hpp>

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
                                                 std::shared_ptr<vthost::IStdioSpawner> spawner,
                                                 SshPrompting prompting)
    {
        auto target =
            vthost::SshTarget { .user = host.url.user, .host = host.url.host, .port = host.url.port };
        if (!host.remoteCommand.empty())
            target.remoteCommand = host.remoteCommand;
        auto const prompts = prompting.broker ? SshPrompts::Askpass : SshPrompts::Batch;
        return HostTransport {
            .factory = std::make_shared<vthost::SshTransportFactory>(std::move(target),
                                                                     std::move(spawner),
                                                                     std::move(prompting.broker),
                                                                     vthost::SshConnectTimeout,
                                                                     std::move(prompting.probe)),
            .label = std::move(label),
            .attach = sshAttachBudget(vthost::SshConnectTimeout, prompts),
        };
    }

    /// @return The TLS transport to @p host: its token read if it names one, its CA if it names one, and
    ///         the SSH key that signs in.
    [[nodiscard]] std::expected<HostTransport, std::string> tlsHostTransport(
        std::string label,
        config::HostEntry const& host,
        core::platform::FileSystem const& fileSystem,
        std::shared_ptr<vthost::IStdioSpawner> const& spawner,
        std::shared_ptr<vthost::IAskpassBroker> const& broker,
        TlsTrust tls)
    {
        if (tls.knownDaemons == nullptr)
            return std::unexpected { std::format("{}: no file to keep pinned daemons in is available",
                                                 label) };
        auto endpoint = vthost::TcpEndpoint { .host = host.url.host,
                                              .port = host.url.port.value_or(0),
                                              .token = {},
                                              .caPem = {},
                                              .expectedFingerprint = std::move(tls.daemonFingerprint) };
        // A token is optional: a daemon that authorizes keys is signed in to by one.
        if (!host.tokenFile.empty())
        {
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
            endpoint.token = *std::move(token);
        }
        if (!host.tlsCa.empty())
        {
            auto pem = readPinnedCa(fileSystem, host.tlsCa, "tls_ca");
            if (!pem)
                return std::unexpected { pem.error() };
            endpoint.caPem = *std::move(pem);
        }
        // A passphrase asked for an identity_file is asked while the handshake runs: the time the user takes
        // over it is not counted against the attach, so that the daemon's pre-authentication deadline is the
        // bound the user has, as the documentation says.
        auto const pause = std::make_shared<vthost::PromptPause>();
        auto keySources = vthost::client::makeClientKeySourceProvider(
            tls.sshAgentEndpoint,
            spawner,
            broker ? std::make_shared<vthost::PausingAskpassBroker>(broker, pause)
                   : std::shared_ptr<vthost::IAskpassBroker> {},
            fileSystem,
            host.identityFile,
            vthost::SshTarget { .user = {}, .host = host.url.host, .port = host.url.port });
        // A daemon met for the first time is asked about, and no clock may run through the question: the
        // transport bounds its dialling itself, and the handshake's bound starts once it hands out the
        // connection.
        auto const attach = (tls.pinPrompt ? vthost::AttachBudget::promptsOpen(TlsHostConnectTimeout)
                                           : vthost::AttachBudget { TlsHostConnectTimeout })
                                .pausedBy(pause);
        return HostTransport {
            .factory = std::make_shared<vthost::TlsTransportFactory>(
                std::move(endpoint), *tls.knownDaemons, std::move(tls.pinPrompt), std::move(keySources)),
            .label = std::move(label),
            .attach = attach,
        };
    }
} // namespace

std::expected<HostTransport, std::string> makeHostTransport(std::string label,
                                                            config::HostEntry const& host,
                                                            core::platform::FileSystem const& fileSystem,
                                                            std::shared_ptr<vthost::IStdioSpawner> spawner,
                                                            SshPrompting prompting,
                                                            TlsTrust tls)
{
    switch (host.url.scheme)
    {
        case config::HostScheme::Ssh:
            return sshHostTransport(std::move(label), host, std::move(spawner), std::move(prompting));
        case config::HostScheme::Tls:
            return tlsHostTransport(
                std::move(label), host, fileSystem, spawner, prompting.broker, std::move(tls));
    }
    return std::unexpected { std::format("{}: the host's scheme is not supported", label) };
}

} // namespace contour::cli
