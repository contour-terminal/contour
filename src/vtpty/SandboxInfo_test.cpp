// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for reading what a sandbox permits out of its own description of itself.
//
// The point of parsing /.flatpak-info rather than inferring from a failed call is that the answer
// is then a FACT: a Flatpak without --share=network breaks DNS resolution and connect() alike, and
// no errno tells that apart from a misspelt host name. @see issue #2075.

#include <vtpty/SandboxInfo.hpp>
#include <vtpty/SshFailureMessage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using vtpty::describeSshConnectFailure;
using vtpty::NetworkAccess;
using vtpty::parseFlatpakInfo;
using vtpty::SandboxState;
using vtpty::SshConnectStage;

namespace
{

// Shaped as flatpak_context_save_metadata() writes it: a key-file whose `shared` is a
// semicolon-separated list of flatpak_context_shares[], i.e. "network" and "ipc" and nothing else.
constexpr auto WithoutNetwork = std::string_view { R"([Application]
name=org.contourterminal.Contour
runtime=runtime/org.kde.Platform/x86_64/6.9

[Instance]
instance-id=3819017542
arch=x86_64

[Context]
shared=ipc;
sockets=wayland;fallback-x11;pulseaudio;
devices=dri;
)" };

constexpr auto WithNetwork = std::string_view { R"([Application]
name=org.contourterminal.Contour

[Context]
shared=network;ipc;
sockets=wayland;
)" };

} // namespace

TEST_CASE("parseFlatpakInfo reads a sandbox that was granted no network", "[vtpty][sandbox]")
{
    auto const info = parseFlatpakInfo(WithoutNetwork);

    CHECK(info.state == SandboxState::Flatpak);
    CHECK(info.network == NetworkAccess::Denied);
    CHECK(info.applicationId == "org.contourterminal.Contour");
}

TEST_CASE("parseFlatpakInfo reads a sandbox that was granted network", "[vtpty][sandbox]")
{
    auto const info = parseFlatpakInfo(WithNetwork);

    CHECK(info.network == NetworkAccess::Permitted);
    CHECK(info.applicationId == "org.contourterminal.Contour");
}

TEST_CASE("parseFlatpakInfo reads `shared` only under [Context]", "[vtpty][sandbox]")
{
    // A `shared=network` sitting in another group must not be read as the context's. The groups are
    // what give the keys their meaning, so a parser that ignored them would be reading noise.
    auto const info = parseFlatpakInfo(R"([Application]
name=org.contourterminal.Contour
shared=network;

[Context]
shared=ipc;
)");

    CHECK(info.network == NetworkAccess::Denied);
}

TEST_CASE("parseFlatpakInfo matches a share whole, not as a substring", "[vtpty][sandbox]")
{
    // "network" must not be found inside a longer share name a future flatpak might define.
    auto const info = parseFlatpakInfo(R"([Context]
shared=networking;
)");

    CHECK(info.network == NetworkAccess::Denied);
}

TEST_CASE("parseFlatpakInfo survives a file saying nothing useful", "[vtpty][sandbox]")
{
    // Being sandboxed is what possessing the file MEANS, so the state stands even when every key
    // this cares about is missing -- and the absent `shared` is itself the denial.
    auto const info = parseFlatpakInfo("");

    CHECK(info.state == SandboxState::Flatpak);
    CHECK(info.network == NetworkAccess::Denied);
    CHECK(info.applicationId.empty());
}

TEST_CASE("parseFlatpakInfo tolerates comments, blank lines and stray whitespace", "[vtpty][sandbox]")
{
    auto const info =
        parseFlatpakInfo("# written by flatpak\n\n  [Context]  \n  shared = ipc ; network ; \n");

    CHECK(info.network == NetworkAccess::Permitted);
}

TEST_CASE("describeSshConnectFailure names the sandbox rather than the symptom", "[vtpty][sandbox]")
{
    // The whole point: with no network permission BOTH stages fail, resolution first, and the
    // platform's own text ("Name or service not known") sends the user looking at their hostname.
    auto const resolve = describeSshConnectFailure(
        SshConnectStage::Resolve, NetworkAccess::Denied, "example.com", "Name or service not known");
    auto const connect = describeSshConnectFailure(
        SshConnectStage::Connect, NetworkAccess::Denied, "10.0.0.1:22", "Network is unreachable");

    CHECK(resolve.contains("no network access"));
    CHECK(resolve.contains("example.com"));
    CHECK(!resolve.contains("Name or service not known")); // replaced, not appended
    CHECK(connect.contains("no network access"));
    CHECK(connect.contains("10.0.0.1:22"));
}

TEST_CASE("describeSshConnectFailure reports the platform's own reason when network is permitted",
          "[vtpty][sandbox]")
{
    auto const resolve = describeSshConnectFailure(SshConnectStage::Resolve,
                                                   NetworkAccess::Permitted,
                                                   "nosuchhost.invalid",
                                                   "Name or service not known");
    auto const connect = describeSshConnectFailure(
        SshConnectStage::Connect, NetworkAccess::Permitted, "10.0.0.1:22", "Connection refused");

    CHECK(resolve == "Failed to resolve host nosuchhost.invalid. Name or service not known");
    CHECK(connect == "Failed to connect to 10.0.0.1:22. Connection refused");
    CHECK(!resolve.contains("sandbox"));
}

TEST_CASE("describeSshConnectFailure does not trail off when the platform said nothing", "[vtpty][sandbox]")
{
    CHECK(describeSshConnectFailure(SshConnectStage::Connect, NetworkAccess::Permitted, "host:22", "")
          == "Failed to connect to host:22.");
}
