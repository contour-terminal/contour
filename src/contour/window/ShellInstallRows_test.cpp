// SPDX-License-Identifier: Apache-2.0
#include <contour/window/ShellInstallRows.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

using namespace contour::window;
using namespace contour::config;
using contour::platform::RcInspection;
using contour::platform::ShellRcError;

namespace
{
constexpr auto Zsh = ShellRows[std::to_underlying(ShellKind::Zsh) - 1];

[[nodiscard]] std::filesystem::path home()
{
    return "/home/me";
}

[[nodiscard]] RcInspection inspected(RcState state, std::string partialLine = {})
{
    return RcInspection { .file = home() / ".zshrc", .state = state, .partialLine = std::move(partialLine) };
}
} // namespace

static_assert(ShellStatusRows.size() == std::to_underlying(RcState::Partial) + 1U);

TEST_CASE("shellInstall.statusRowsAreInEnumeratorOrder", "[shellintegration][settings]")
{
    for (auto const index: std::views::iota(std::size_t { 0 }, ShellStatusRows.size()))
        CHECK(std::to_underlying(ShellStatusRows[index].state) == index);
}

TEST_CASE("shellInstall.eachStateIsSaidAndOnlyAnAbsentOneIsOffered", "[shellintegration][settings]")
{
    struct Expected
    {
        RcState state;
        std::string_view status;
        InstallOffer install;
    };
    for (auto const& expected: std::array {
             Expected { RcState::Unknown, "Unknown", InstallOffer::No },
             Expected { RcState::Absent, "Not installed", InstallOffer::Yes },
             Expected { RcState::Installed, "Installed in ~/.zshrc", InstallOffer::No },
             Expected { RcState::Partial,
                        "Cut short in ~/.zshrc: delete this line, then install again",
                        InstallOffer::No },
         })
    {
        CAPTURE(std::to_underlying(expected.state));
        auto const row = shellInstallRowOf(
            Zsh, inspected(expected.state), home(), HostPlatform::Other, BinaryReach::Reachable);
        CHECK(row.shell == ShellKind::Zsh);
        CHECK(row.name == "zsh");
        CHECK(row.status == expected.status);
        CHECK(row.install == expected.install);
    }
}

TEST_CASE("shellInstall.aCutShortInstallNamesTheLineToDelete", "[shellintegration][settings]")
{
    auto const cut = std::string { R"(if [ "$TERM_PROGRAM" = contour ]; then eval "$('/usr/b)" };
    CHECK(shellInstallRowOf(
              Zsh, inspected(RcState::Partial, cut), home(), HostPlatform::Other, BinaryReach::Reachable)
              .deleteLine
          == cut);
    // Nothing else carries a line to delete.
    CHECK(shellInstallRowOf(
              Zsh, inspected(RcState::Absent), home(), HostPlatform::Other, BinaryReach::Reachable)
              .deleteLine.empty());
    CHECK(shellInstallRowOf(
              Zsh, inspected(RcState::Installed), home(), HostPlatform::Other, BinaryReach::Reachable)
              .deleteLine.empty());
}

TEST_CASE("shellInstall.aFileOutsideHomeIsShownWhole", "[shellintegration][settings]")
{
    auto const row =
        shellInstallRowOf(Zsh,
                          RcInspection { .file = "/etc/zsh/.zshrc", .state = RcState::Installed },
                          home(),
                          HostPlatform::Other,
                          BinaryReach::Reachable);
    CHECK(row.status == "Installed in /etc/zsh/.zshrc");
}

TEST_CASE("shellInstall.everyRefusalIsSaidAndNeverOffered", "[shellintegration][settings]")
{
    constexpr auto Last = std::to_underlying(ShellRcError::AppendUnterminated);
    for (auto const code: std::views::iota(0, Last + 1))
    {
        auto const error = static_cast<ShellRcError>(code);
        CAPTURE(code);
        auto const row = shellInstallRowOf(
            Zsh, std::unexpected(error), home(), HostPlatform::Other, BinaryReach::Reachable);
        CHECK(row.status == "Unknown: " + std::string { contour::platform::describe(error) });
        CHECK(row.install == InstallOffer::No);
        CHECK(row.rc.empty());
    }
}

TEST_CASE("shellInstall.aBlockReplacesTheStatusOfAnInstallableRowOnly", "[shellintegration][settings]")
{
    struct Case
    {
        ShellRow shell;
        HostPlatform host;
        BinaryReach binary;
        ProfileScripts scripts;
        InstallBlock block;
    };
    constexpr auto Pwsh = ShellRows[std::to_underlying(ShellKind::Pwsh) - 1];
    for (auto const& c: std::array {
             Case { Zsh,
                    HostPlatform::Windows,
                    BinaryReach::Reachable,
                    ProfileScripts::Run,
                    InstallBlock::NotOnWindows },
             Case { Zsh,
                    HostPlatform::Other,
                    BinaryReach::Unreachable,
                    ProfileScripts::Run,
                    InstallBlock::BinaryUnreachable },
             Case { Pwsh,
                    HostPlatform::Windows,
                    BinaryReach::Reachable,
                    ProfileScripts::BlockedByGroupPolicy,
                    InstallBlock::GroupPolicy },
             Case { Pwsh,
                    HostPlatform::Windows,
                    BinaryReach::Reachable,
                    ProfileScripts::SignedOnly,
                    InstallBlock::SignedScriptsOnly },
         })
    {
        CAPTURE(std::to_underlying(c.block));
        auto absentFile = inspected(RcState::Absent);
        absentFile.scripts = c.scripts;
        auto const absent = shellInstallRowOf(c.shell, absentFile, home(), c.host, c.binary);
        CHECK(absent.status == InstallBlockRows[std::to_underlying(c.block)].text);
        CHECK(absent.install == InstallOffer::No);
        // A file that is already done says so, whatever blocks a new install.
        auto installedFile = inspected(RcState::Installed);
        installedFile.scripts = c.scripts;
        auto const installed = shellInstallRowOf(c.shell, installedFile, home(), c.host, c.binary);
        CHECK(installed.status == "Installed in ~/.zshrc");
    }
}

TEST_CASE("shellInstall.aPolicyTheUserCanFixStillOffersTheInstall", "[shellintegration][settings]")
{
    constexpr auto Pwsh = ShellRows[std::to_underlying(ShellKind::Pwsh) - 1];
    auto file = inspected(RcState::Absent);
    file.scripts = ProfileScripts::NeedUserPolicy;
    auto const row = shellInstallRowOf(Pwsh, file, home(), HostPlatform::Windows, BinaryReach::Reachable);
    CHECK(row.status == "Not installed");
    CHECK(row.install == InstallOffer::Yes);
}

TEST_CASE("ShellInstallRows.blocked", "[shellintegration][settings]")
{
    constexpr auto Bash = ShellRows[std::to_underlying(ShellKind::Bash) - 1];
    auto const row = blockedShellInstallRowOf(Bash, InstallBlock::NotOnWindows);
    CHECK(row.shell == ShellKind::Bash);
    CHECK(row.name == "bash");
    CHECK(row.status == "Not available on Windows");
    CHECK(row.install == InstallOffer::No);
    CHECK(row.rc.empty());
    CHECK(row.deleteLine.empty());
}

TEST_CASE("ShellInstallRows.displayName", "[shellintegration][settings]")
{
    constexpr auto Pwsh = ShellRows[std::to_underlying(ShellKind::Pwsh) - 1];
    constexpr auto WindowsPowerShell = ShellRows[std::to_underlying(ShellKind::WindowsPowerShell) - 1];
    CHECK(shellInstallRowOf(
              Pwsh, inspected(RcState::Absent), home(), HostPlatform::Windows, BinaryReach::Reachable)
              .name
          == "PowerShell");
    CHECK(blockedShellInstallRowOf(WindowsPowerShell, InstallBlock::WindowsOnly).name
          == "Windows PowerShell");
}
