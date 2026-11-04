// SPDX-License-Identifier: Apache-2.0
#include <contour/config/ShellKind.hpp>

#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string_view>

using namespace contour::config;

TEST_CASE("ShellKind.shellKindOf.WindowsNames", "[shells]")
{
    CHECK(shellKindOf("pwsh") == ShellKind::Pwsh);
    CHECK(shellKindOf("pwsh.exe") == ShellKind::Pwsh);
    CHECK(shellKindOf(R"(C:\Program Files\PowerShell\7\pwsh.exe)") == ShellKind::Pwsh);
    CHECK(shellKindOf("POWERSHELL.EXE") == ShellKind::WindowsPowerShell);
    CHECK(shellKindOf(R"(C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe)")
          == ShellKind::WindowsPowerShell);
    CHECK(shellKindOf("/usr/bin/pwsh") == ShellKind::Pwsh);
    CHECK(shellKindOf("-zsh") == ShellKind::Zsh);
    CHECK(shellKindOf("cmd.exe") == ShellKind::Unknown);
    CHECK(shellKindOf("pwsh.exe.bak") == ShellKind::Unknown);
}

TEST_CASE("ShellKind.rows.inEnumeratorOrder", "[shells]")
{
    auto names = std::set<std::string_view> {};
    for (auto const [index, row]: core::views::enumerate(ShellRows))
    {
        CHECK(static_cast<std::size_t>(row.kind) == static_cast<std::size_t>(index) + 1);
        CHECK(shellRow(row.kind) == &row);
        names.insert(row.name);
    }
    CHECK(names.size() == ShellRows.size());
}

TEST_CASE("ShellKind.rcLocationOn", "[shells]")
{
    auto const& pwsh = *shellRow(ShellKind::Pwsh);
    auto const windows = rcLocationOn(pwsh, HostPlatform::Windows);
    CHECK(windows.base == RcBase::Documents);
    CHECK(windows.file == "PowerShell/Microsoft.PowerShell_profile.ps1");
    for (auto const host: { HostPlatform::Other, HostPlatform::KdePlasma, HostPlatform::MacOS })
    {
        auto const location = rcLocationOn(pwsh, host);
        CHECK(location.base == RcBase::XdgConfig);
        CHECK(location.file == "powershell/Microsoft.PowerShell_profile.ps1");
    }
    CHECK(rcLocationOn(*shellRow(ShellKind::Bash), HostPlatform::MacOS).file == ".bash_profile");
    CHECK(rcLocationOn(*shellRow(ShellKind::Bash), HostPlatform::Other).file == ".bashrc");
}

TEST_CASE("ShellKind.hosts", "[shells]")
{
    auto const& bash = *shellRow(ShellKind::Bash);
    auto const& pwsh = *shellRow(ShellKind::Pwsh);
    auto const& legacy = *shellRow(ShellKind::WindowsPowerShell);
    for (auto const host:
         { HostPlatform::Other, HostPlatform::Windows, HostPlatform::MacOS, HostPlatform::KdePlasma })
        CHECK(isListedOn(bash, host));
    CHECK_FALSE(isListedOn(legacy, HostPlatform::Other));
    CHECK_FALSE(isListedOn(legacy, HostPlatform::MacOS));
    CHECK(isListedOn(legacy, HostPlatform::Windows));

    CHECK_FALSE(isInstallableOn(bash, HostPlatform::Windows));
    CHECK(isInstallableOn(bash, HostPlatform::MacOS));
    CHECK(isInstallableOn(pwsh, HostPlatform::Windows));
    CHECK(isInstallableOn(pwsh, HostPlatform::MacOS));
    CHECK(isInstallableOn(pwsh, HostPlatform::Other));
    CHECK(isInstallableOn(legacy, HostPlatform::Windows));
    CHECK_FALSE(isInstallableOn(legacy, HostPlatform::Other));
    CHECK_FALSE(isInstallableOn(legacy, HostPlatform::MacOS));
}
