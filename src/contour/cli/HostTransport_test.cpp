// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/HostTransport.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <memory>
#include <span>
#include <string>

#include <vthost/Askpass.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/SshTransport.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/Transport.hpp>
#include <vthost/testing/CountingRandom.hpp>

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
        core::net::EventLoop& /*loop*/,
        std::span<std::string const> /*argv*/,
        vthost::SpawnEnvironment const& /*environment*/) override
    {
        return std::unexpected { vthost::StdioSpawnError { .kind = vthost::SpawnFailure::SystemError,
                                                           .message = "not in this test" } };
    }
};

[[nodiscard]] std::shared_ptr<vthost::IStdioSpawner> nullSpawner()
{
    return std::make_shared<NullStdioSpawner>();
}

/// Where a tls host's daemon is pinned: in memory.
struct Pins
{
    core::platform::testing::InMemoryFileSystem files;
    vthost::testing::CountingRandom random {};
    vthost::KnownDaemons known { files, "/pins/known_daemons", random };

    [[nodiscard]] contour::cli::TlsTrust tls() { return contour::cli::TlsTrust { .knownDaemons = &known }; }
};

[[nodiscard]] HostEntry tlsStudio()
{
    return HostEntry { .url =
                           HostUrl { .scheme = HostScheme::Tls, .user = {}, .host = "studio", .port = 7297 },
                       .tokenFile = "/home/me/.config/contour/studio.token",
                       .tlsCa = "/home/me/.config/contour/studio-ca.pem",
                       .identityFile = {},
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
                    .identityFile = {},
                    .remoteCommand = "/opt/contour/bin/contour daemon --stdio",
                    .profile = {} };

    auto const transport = makeHostTransport(
        "home", host, fileSystem, nullSpawner(), contour::cli::SshPrompting {}, contour::cli::TlsTrust {});
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "ssh://me@homepc:2222");
    // Longer than the transport's own bound, so that bound speaks first.
    CHECK(transport->attach.connect == vthost::SshConnectTimeout + contour::cli::SshAttachMargin);
    CHECK(transport->attach.connect > vthost::SshConnectTimeout);
    CHECK(transport->label == "home");
}

namespace
{
/// Opens no session: only its presence matters to the budget.
class NullAskpassBroker final: public vthost::IAskpassBroker
{
  public:
    [[nodiscard]] std::expected<std::unique_ptr<vthost::IAskpassSession>, std::string> open(
        vthost::SshTarget const& /*target*/) override
    {
        return std::unexpected { std::string { "not in this test" } };
    }
};
} // namespace

TEST_CASE("an ssh host with a prompt broker has no outer attach timeout", "[contour][hosts]")
{
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto const host = HostEntry {
        .url = HostUrl { .scheme = HostScheme::Ssh, .user = "me", .host = "homepc", .port = std::nullopt },
        .tokenFile = {},
        .tlsCa = {},
        .identityFile = {},
        .remoteCommand = {},
        .profile = {}
    };

    auto const transport =
        makeHostTransport("home",
                          host,
                          fileSystem,
                          nullSpawner(),
                          contour::cli::SshPrompting { .broker = std::make_shared<NullAskpassBroker>(),
                                                       .probe = std::make_shared<vthost::SshVersionProbe>() },
                          contour::cli::TlsTrust {});
    REQUIRE(transport.has_value());
    // The transport's own deadline pauses for prompts and is the only clock: no outer timeout to speak of.
    CHECK_FALSE(transport->attach.connect.has_value());
    // What follows the preamble stays bound.
    CHECK(transport->attach.handshake == vthost::SshConnectTimeout);
}

TEST_CASE("a tls host reads its token and CA", "[contour][hosts]")
{
    auto pins = Pins {};
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "secret\r\n");
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "-----BEGIN CERTIFICATE-----\n");

    auto const transport = makeHostTransport(
        "studio", tlsStudio(), fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "tls://studio:7297");
    CHECK(transport->attach.connect == std::chrono::seconds(5));
    CHECK(transport->label == "studio");
}

TEST_CASE("a tls host whose token file is missing is refused", "[contour][hosts]")
{
    auto pins = Pins {};
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "-----BEGIN CERTIFICATE-----\n");

    auto const transport = makeHostTransport(
        "studio", tlsStudio(), fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error().contains("/home/me/.config/contour/studio.token"));
}

TEST_CASE("a tls host whose token file is blank is refused", "[contour][hosts]")
{
    auto pins = Pins {};
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "\n");
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "-----BEGIN CERTIFICATE-----\n");

    auto const transport = makeHostTransport(
        "studio", tlsStudio(), fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error() == "token file '/home/me/.config/contour/studio.token' is empty");
}

TEST_CASE("a tls host whose CA file is empty is refused", "[contour][hosts]")
{
    auto pins = Pins {};
    // An empty CA would silently mean trust on first use.
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "secret\n");
    fileSystem.addFile("/home/me/.config/contour/studio-ca.pem", "");

    auto const transport = makeHostTransport(
        "studio", tlsStudio(), fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error() == "cannot read tls_ca file '/home/me/.config/contour/studio-ca.pem'");
}

TEST_CASE("a tls host without a CA trusts on first use", "[contour][hosts]")
{
    auto pins = Pins {};
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    fileSystem.addFile("/home/me/.config/contour/studio.token", "secret\n");
    auto host = tlsStudio();
    host.tlsCa.clear();

    auto const transport = makeHostTransport(
        "studio", host, fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "tls://studio:7297");
}

namespace
{
/// Asks nothing, answers yes: all a test of a host's budget needs of a pin prompt.
class AcceptingPinPrompt final: public vthost::IPinPrompt
{
  public:
    core::async::Task<vthost::PinDecision> confirm(std::string /*hostPort*/,
                                                   std::string /*fingerprint*/) override
    {
        co_return vthost::PinDecision::Trust;
    }
};
} // namespace

TEST_CASE("a tls host that can ask about a new daemon has no clock through the question", "[contour][hosts]")
{
    auto pins = Pins {};
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto host = tlsStudio();
    host.tokenFile.clear();
    host.tlsCa.clear();

    SECTION("with someone to ask, the connect is not bound from outside")
    {
        auto tls = pins.tls();
        tls.pinPrompt = std::make_shared<AcceptingPinPrompt>();
        auto const transport =
            makeHostTransport("studio", host, fileSystem, nullSpawner(), contour::cli::SshPrompting {}, tls);
        REQUIRE(transport.has_value());
        CHECK_FALSE(transport->attach.connect.has_value());
        // What follows the connection stays bound, from the moment the transport hands it out.
        CHECK(transport->attach.handshake == contour::cli::TlsHostConnectTimeout);
    }
    SECTION("with no one to ask, the whole attach is bound")
    {
        auto const transport = makeHostTransport(
            "studio", host, fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
        REQUIRE(transport.has_value());
        CHECK(transport->attach.connect == contour::cli::TlsHostConnectTimeout);
    }
}

TEST_CASE("a tls host needs no token: a key signs in", "[contour][hosts]")
{
    auto pins = Pins {};
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto host = tlsStudio();
    host.tokenFile.clear();
    host.tlsCa.clear();
    host.identityFile = "/home/me/.ssh/id_work";

    auto const transport = makeHostTransport(
        "studio", host, fileSystem, nullSpawner(), contour::cli::SshPrompting {}, pins.tls());
    REQUIRE(transport.has_value());
    CHECK(transport->factory->describe() == "tls://studio:7297");
}

TEST_CASE("a tls host is refused where there is no file to pin daemons in", "[contour][hosts]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {};
    auto host = tlsStudio();
    host.tokenFile.clear();
    host.tlsCa.clear();

    auto const transport = makeHostTransport(
        "studio", host, fileSystem, nullSpawner(), contour::cli::SshPrompting {}, contour::cli::TlsTrust {});
    REQUIRE_FALSE(transport.has_value());
    CHECK(transport.error().contains("pinned daemons"));
}
