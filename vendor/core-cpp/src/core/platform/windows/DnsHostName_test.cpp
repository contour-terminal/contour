// SPDX-License-Identifier: Apache-2.0
//
// Windows gives a machine two names. The NetBIOS name is upper-cased and cut to 15 characters; the
// DNS host name is the one POSIX `gethostname()` answers, the one Qt's `QHostInfo::localHostName()`
// answers (`QSysInfo::machineHostName()` asks `GetComputerNameExW` for `ComputerNameDnsHostname`),
// and the one a shell writes into an OSC 7 `file://host/path` URL. hostName() answered the NetBIOS
// name, so a consumer comparing it with any of those called a directory on its own machine remote.
// POSIX has one name and no such case; this file is the Windows half.
#include <core/WideString.hpp>
#include <core/platform/SystemInfo.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <tuple>

#include <windows.h>

namespace
{

/// @return The name Windows keeps for @p format, in UTF-8, or empty where it keeps none.
[[nodiscard]] std::string computerName(COMPUTER_NAME_FORMAT format)
{
    auto size = DWORD { 0 };
    std::ignore = ::GetComputerNameExW(format, nullptr, &size);
    auto name = std::wstring(size, L'\0');
    if (!::GetComputerNameExW(format, name.data(), &size))
        return {};
    return core::toUtf8({ name.data(), size });
}

} // namespace

TEST_CASE("hostName answers the DNS host name rather than the NetBIOS name", "[platform][windows]")
{
    auto const dnsHostName = computerName(ComputerNameDnsHostname);
    REQUIRE_FALSE(dnsHostName.empty());
    CHECK(core::platform::hostName() == dnsHostName);
}

TEST_CASE("hostName is not the NetBIOS name where this machine spells the two apart", "[platform][windows]")
{
    // The case above cannot tell the two names apart on a machine that spells them alike -- an
    // upper-case name of at most 15 characters -- so it says so here instead of passing.
    auto const dnsHostName = computerName(ComputerNameDnsHostname);
    auto const netBiosName = computerName(ComputerNameNetBIOS);
    if (dnsHostName == netBiosName)
        SKIP("this machine's DNS host name and NetBIOS name are both " + netBiosName);
    CHECK(core::platform::hostName() != netBiosName);
}
