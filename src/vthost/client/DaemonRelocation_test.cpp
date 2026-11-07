// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

#include <vthost/client/DaemonRelocation.hpp>

namespace
{
using vthost::HostPlatform;
using vthost::RelocationRequest;
using vthost::client::DaemonStatus;
using vthost::client::RelocationVerdict;
using vthost::client::relocationVerdictFor;

constexpr auto Bin = "C:/Program Files/Contour/bin";

[[nodiscard]] DaemonStatus statusOf(std::filesystem::path executable,
                                    std::optional<RelocationRequest> request)
{
    auto status = DaemonStatus {};
    status.executablePath = std::move(executable);
    status.relocation = request;
    return status;
}
} // namespace

TEST_CASE("a daemon of the installation that allows it is moved", "[relocation]")
{
    CHECK(
        relocationVerdictFor(statusOf("C:/Program Files/Contour/bin/contour.exe", RelocationRequest::Allowed),
                             Bin,
                             HostPlatform::Windows)
        == RelocationVerdict::Move);
}

TEST_CASE("a daemon of another installation is left", "[relocation]")
{
    CHECK(relocationVerdictFor(
              statusOf("C:/Users/u/AppData/Local/contour/bin/contour.exe", RelocationRequest::Allowed),
              Bin,
              HostPlatform::Windows)
          == RelocationVerdict::OtherInstallation);
}

TEST_CASE("a daemon started with --in-place is left", "[relocation]")
{
    CHECK(
        relocationVerdictFor(statusOf("C:/Program Files/Contour/bin/contour.exe", RelocationRequest::Refused),
                             Bin,
                             HostPlatform::Windows)
        == RelocationVerdict::InPlaceRequested);
}

TEST_CASE("a daemon that does not state a request is left", "[relocation]")
{
    CHECK(relocationVerdictFor(
              statusOf("C:/Program Files/Contour/bin/contour.exe", std::nullopt), Bin, HostPlatform::Windows)
          == RelocationVerdict::PredatesRelocation);
}

TEST_CASE("another installation is told before the request is looked at", "[relocation]")
{
    CHECK(relocationVerdictFor(
              statusOf("/opt/other/bin/contour", RelocationRequest::Refused), "/usr/bin", HostPlatform::Linux)
          == RelocationVerdict::OtherInstallation);
}

TEST_CASE("a Windows directory is the same whatever its case", "[relocation]")
{
    auto const status = statusOf("c:/program files/contour/BIN/contour.exe", RelocationRequest::Allowed);
    CHECK(relocationVerdictFor(status, Bin, HostPlatform::Windows) == RelocationVerdict::Move);
    CHECK(relocationVerdictFor(status, Bin, HostPlatform::Linux) == RelocationVerdict::OtherInstallation);
}

TEST_CASE("a trailing separator and dot segments do not make another directory", "[relocation]")
{
    auto const status = statusOf("/usr/bin/contour", RelocationRequest::Allowed);
    CHECK(relocationVerdictFor(status, "/usr/bin/", HostPlatform::Linux) == RelocationVerdict::Move);
    CHECK(relocationVerdictFor(status, "/usr/./lib/../bin", HostPlatform::Linux) == RelocationVerdict::Move);
}

TEST_CASE("only control sockets are kept among a socket directory's entries", "[relocation]")
{
    auto const entries = std::vector<std::filesystem::path> {
        "/run/contour/default",     "/run/contour/default-native", "/run/contour/default-tmux",
        "/run/contour/default.log", "/run/contour/work",
    };
    auto const sockets = vthost::client::controlSocketsAmong(entries);
    CHECK(sockets == std::vector<std::filesystem::path> { "/run/contour/default", "/run/contour/work" });
}

TEST_CASE("a label that merely ends in a sibling suffix is a control socket", "[relocation]")
{
    auto const entries = std::vector<std::filesystem::path> {
        "/run/contour/my-native",
        "/run/contour/work",
        "/run/contour/work-tmux",
        "/run/contour/work.log",
    };
    CHECK(vthost::client::controlSocketsAmong(entries)
          == std::vector<std::filesystem::path> { "/run/contour/my-native", "/run/contour/work" });
}

TEST_CASE("every verdict but Move has a reason", "[relocation]")
{
    using vthost::client::reasonOf;
    CHECK(reasonOf(RelocationVerdict::Move).empty());
    CHECK(reasonOf(RelocationVerdict::OtherInstallation)
          == "it does not run from this installation's folder");
    CHECK(reasonOf(RelocationVerdict::InPlaceRequested) == "it was started with --in-place");
    CHECK(reasonOf(RelocationVerdict::PredatesRelocation) == "its release cannot be moved");
}

TEST_CASE("a Windows path spelt with backslashes and dot segments is compared normalized", "[relocation]")
{
    auto const status = statusOf(R"(C:\Program Files\Contour\.\bin\contour.exe)", RelocationRequest::Allowed);
    CHECK(relocationVerdictFor(status, "C:/Program Files/Contour/bin", HostPlatform::Windows)
          == RelocationVerdict::Move);
    CHECK(relocationVerdictFor(status, R"(C:\Program Files\Other\..\Contour\bin)", HostPlatform::Windows)
          == RelocationVerdict::Move);
}
