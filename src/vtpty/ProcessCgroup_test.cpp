// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessCgroup.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

namespace
{

constexpr auto Unit = std::string_view { "contour-session-10-20-1.scope" };

struct CgroupRow
{
    std::string_view name;
    std::string_view cgroupFile;
    bool inUnit;
};

constexpr auto CgroupRows = std::array {
    CgroupRow { .name = "unified hierarchy, in the unit",
                .cgroupFile = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                              "contour-session-10-20-1.scope\n",
                .inUnit = true },
    CgroupRow { .name = "without the trailing newline",
                .cgroupFile = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                              "contour-session-10-20-1.scope",
                .inUnit = true },
    CgroupRow { .name = "still in Contour's own unit",
                .cgroupFile = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                              "app-org.contourterminal.Contour@abc.service\n",
                .inUnit = false },
    CgroupRow { .name = "another session's scope that ends in the same name",
                .cgroupFile = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/"
                              "contour-session-10-20-11.scope\n",
                .inUnit = false },
    CgroupRow { .name = "a unit whose name ends in this one's",
                .cgroupFile = "0::/app.slice/xcontour-session-10-20-1.scope\n",
                .inUnit = false },
    CgroupRow { .name = "beneath the unit, not in it",
                .cgroupFile = "0::/app.slice/contour-session-10-20-1.scope/payload\n",
                .inUnit = false },
    CgroupRow { .name = "legacy hierarchy, systemd's own",
                .cgroupFile = "12:memory:/user.slice\n"
                              "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/app.slice/"
                              "contour-session-10-20-1.scope\n",
                .inUnit = true },
    CgroupRow { .name = "no file", .cgroupFile = "", .inUnit = false },
    CgroupRow {
        .name = "a line without a path", .cgroupFile = "contour-session-10-20-1.scope\n", .inUnit = false },
};

} // namespace

TEST_CASE("ProcessCgroup.isInUnit", "[placement]")
{
    for (auto const& row: CgroupRows)
    {
        INFO(row.name);
        CHECK(vtpty::isInUnit(row.cgroupFile, Unit) == row.inUnit);
    }
}

TEST_CASE("ProcessCgroup.isInUnit: no unit is in no cgroup", "[placement]")
{
    CHECK_FALSE(vtpty::isInUnit("0::/\n", ""));
}
