// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <utility>

#include <vthost/client/ReleaseDrift.hpp>

TEST_CASE("releases compare by their numeric prefix", "[vthost][drift]")
{
    using enum vthost::client::DriftKind;
    CHECK(vthost::client::compareReleases("0.8.2", "0.8.1") == DaemonOlder);
    CHECK(vthost::client::compareReleases("0.8.1", "0.10.0") == DaemonNewer);
    CHECK(vthost::client::compareReleases("0.8.1", "0.8.1") == None);
    CHECK(vthost::client::compareReleases("0.8.1", "0.8.1-dev") == Differs);
    CHECK(vthost::client::compareReleases("0.8", "0.8.0") == Differs);
    CHECK(vthost::client::compareReleases("0.8.1", "nightly") == Differs);
    CHECK(vthost::client::compareReleases("0.8.1", "") == DaemonUnknown);
}

TEST_CASE("release numbers are the leading dot-separated run", "[vthost][drift]")
{
    CHECK(vthost::client::parseReleaseNumbers("0.8.1-master+abc") == std::vector<std::uint32_t> { 0, 8, 1 });
    CHECK(vthost::client::parseReleaseNumbers("12") == std::vector<std::uint32_t> { 12 });
    CHECK(vthost::client::parseReleaseNumbers("1.") == std::vector<std::uint32_t> { 1 });
    CHECK_FALSE(vthost::client::parseReleaseNumbers("nightly").has_value());
    CHECK_FALSE(vthost::client::parseReleaseNumbers("").has_value());
}

TEST_CASE("the drift notice uses the spec's wording", "[vthost][drift]")
{
    using vthost::client::LayoutAfterRestart;
    auto const n = vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Lost);
    CHECK(n.text == "daemon 0.8.1 is older than this client");
    CHECK(n.confirmation.contains("ends every shell"));
    CHECK(vthost::client::assessDrift("0.8.1", "0.8.1", LayoutAfterRestart::Lost).text.empty());
    CHECK(vthost::client::assessDrift("0.8.1", "0.9.0", LayoutAfterRestart::Lost).text
          == "daemon 0.9.0 is newer than this client");
    CHECK(vthost::client::assessDrift("0.8.1", "0.8.1-dev", LayoutAfterRestart::Lost).text
          == "daemon 0.8.1-dev differs from this client (0.8.1)");
    CHECK(vthost::client::assessDrift("0.8.1", "", LayoutAfterRestart::Lost).text
          == "daemon of an earlier release than this client (0.8.1)");
    auto const restored = vthost::client::assessDrift("0.8.2", "0.8.1", LayoutAfterRestart::Restored);
    CHECK(
        restored.confirmation
        == "Restarting the daemon on 0.8.2 ends the programs running in it. Your windows, tabs, directories "
           "and scrollback come back.");
}

TEST_CASE("before phase 6 a restart does not keep the layout", "[vthost][drift]")
{
    CHECK(vthost::client::layoutSurvivesRestart(vthost::proto::Negotiated {})
          == vthost::client::LayoutAfterRestart::Lost);
}

TEST_CASE("layoutSurvivesRestart follows the restore capability", "[vthost][client]")
{
    using vthost::proto::Capability;
    using vthost::proto::CapabilitySet;
    auto const offered = [](CapabilitySet daemon, CapabilitySet shared = {}) {
        return vthost::client::layoutSurvivesRestart(vthost::proto::Negotiated {
            .version = 4, .peerCapabilities = std::move(daemon), .shared = std::move(shared) });
    };
    CHECK(offered(CapabilitySet::of({ Capability::DaemonControl, Capability::WorkspaceRestore }))
          == vthost::client::LayoutAfterRestart::Restored);
    CHECK(offered(CapabilitySet::of({ Capability::DaemonControl }))
          == vthost::client::LayoutAfterRestart::Lost);
    // What the daemon offers decides, whatever the client offered.
    CHECK(offered(CapabilitySet::of({ Capability::DaemonControl }),
                  CapabilitySet::of({ Capability::WorkspaceRestore }))
          == vthost::client::LayoutAfterRestart::Lost);
}

TEST_CASE("the restart action has its spec label", "[vthost][drift]")
{
    CHECK(vthost::client::RestartActionLabel == "Restart daemon on the new version");
}
