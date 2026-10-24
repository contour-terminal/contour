// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/HostTransport.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <span>
#include <string>

#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/Transport.hpp>

using contour::cli::makeHostTransport;
using contour::config::HostEntry;
using contour::config::HostScheme;
using contour::config::HostUrl;

namespace
{
/// Starts nothing: mapping a host onto its transport must not spawn.
class NullStdioSpawner final: public vthost::IStdioSpawner
{
  public:
    [[nodiscard]] std::expected<vthost::SpawnedStdioChild, vthost::StdioSpawnError> spawn(
        core::net::EventLoop& /*loop*/, std::span<std::string const> /*argv*/) override
    {
        return std::unexpected { vthost::StdioSpawnError { .kind = vthost::SpawnFailure::SystemError,
                                                           .message = "not in this test" } };
    }
};

[[nodiscard]] std::shared_ptr<vthost::IStdioSpawner> nullSpawner()
{
    return std::make_shared<NullStdioSpawner>();
}

[[nodiscard]] HostEntry tlsStudio()
{
    return HostEntry { .url =
                           HostUrl { .scheme = HostScheme::Tls, .user = {}, .host = "studio", .port = 7297 },
                       .tokenFile = "/home/me/.config/contour/studio.token",
                       .tlsCa = "/home/me/.config/contour/studio-ca.pem",
                       .remoteCommand = {},
                       .profile = {} };
}
} // namespace

TEST_CASE("an ssh host maps onto the ssh transport", "[contour][hosts]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto const host =
        HostEntry { .url =
                        HostUrl { .scheme = HostScheme::Ssh, .user = "me", .host = "homepc", .port = 2222 },
                    .tokenFile = {},
                    .tlsCa = {},
                    .remoteCommand = "/opt/contour/bin/contour daemon --stdio",
                    .profile = {} };

    auto const transport = makeHostTransport("home", host, fileSystem, nullSpawner());
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "ssh://me@homepc:2222");
    // ssh verifies the host key: a control client may send this host its token.
    CHECK(transport->factory->peerTrust() == vthost::PeerTrust::Verified);
    // Longer than the transport's own bound, so that bound speaks first.
    CHECK(transport->connectTimeout == vthost::SshConnectTimeout + contour::cli::SshAttachMargin);
    CHECK(transport->connectTimeout > vthost::SshConnectTimeout);
    CHECK(transport->label == "home");
}

TEST_CASE("a tls host reads its token and CA", "[contour][hosts]")
{
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "secret\r\n");
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "-----BEGIN CERTIFICATE-----\n");

    auto const transport = makeHostTransport("studio", tlsStudio(), fileSystem, nullSpawner());
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "tls://studio:7297");
    // The CA was read and handed on: the daemon's certificate is checked against it.
    CHECK(transport->factory->peerTrust() == vthost::PeerTrust::Verified);
    CHECK(transport->connectTimeout == std::chrono::seconds(5));
    CHECK(transport->label == "studio");
}

TEST_CASE("a tls host whose token file is missing is refused", "[contour][hosts]")
{
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "-----BEGIN CERTIFICATE-----\n");

    auto const transport = makeHostTransport("studio", tlsStudio(), fileSystem, nullSpawner());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error().contains("/home/me/.config/contour/studio.token"));
}

TEST_CASE("a tls host whose token file is blank is refused", "[contour][hosts]")
{
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "\n");
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "-----BEGIN CERTIFICATE-----\n");

    auto const transport = makeHostTransport("studio", tlsStudio(), fileSystem, nullSpawner());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error() == "token file '/home/me/.config/contour/studio.token' is empty");
}

TEST_CASE("a tls host whose CA file is empty is refused", "[contour][hosts]")
{
    // An empty CA would silently mean trust on first use.
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "secret\n");
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "");

    auto const transport = makeHostTransport("studio", tlsStudio(), fileSystem, nullSpawner());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error() == "cannot read tls_ca file '/home/me/.config/contour/studio-ca.pem'");
}

TEST_CASE("a tls host without a CA trusts on first use", "[contour][hosts]")
{
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "secret\n");
    auto host = tlsStudio();
    host.tlsCa.clear();

    auto const transport = makeHostTransport("studio", host, fileSystem, nullSpawner());
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "tls://studio:7297");
    CHECK(transport->factory->peerTrust() == vthost::PeerTrust::Unverified);
}
