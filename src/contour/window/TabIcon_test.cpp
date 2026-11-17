// SPDX-License-Identifier: Apache-2.0
#include <contour/window/TabIcon.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

using contour::window::programInitial;
using contour::window::TabIcon;
using contour::window::TabIconKind;
using contour::window::tabIconOf;

namespace
{
std::filesystem::path const Home { "/home/u" };
} // namespace

TEST_CASE("TabIcon.autoShowsTheRunningProgramsInitialElseTheRowsOwn", "[contour][tabicon]")
{
    CHECK(tabIconOf("auto", "/usr/bin/nvim", Home)
          == TabIcon { .kind = TabIconKind::Monogram, .initial = "N", .image = {} });
    CHECK(tabIconOf("auto", "", Home) == TabIcon {});
    // An unset key is auto.
    CHECK(tabIconOf("", "htop", Home).kind == TabIconKind::Monogram);
}

TEST_CASE("TabIcon.noneHidesItAndAnythingElseIsAnImage", "[contour][tabicon]")
{
    CHECK(tabIconOf("none", "nvim", Home)
          == TabIcon { .kind = TabIconKind::Hidden, .initial = {}, .image = {} });
    auto const image = tabIconOf("/icons/pwsh.png", "", Home);
    CHECK(image.kind == TabIconKind::Image);
    CHECK(image.image == std::filesystem::path { "/icons/pwsh.png" });
    // A leading ~ is the home directory, as for every other path in the configuration.
    CHECK(tabIconOf("~/icons/a.svg", "", Home).image == Home / "icons" / "a.svg");
}

TEST_CASE("TabIcon.theInitialIsTheFirstLetterOrDigitOfTheBaseName", "[contour][tabicon]")
{
    CHECK(programInitial("python3.exe") == "P");
    CHECK(programInitial("C:\\Tools\\7z.exe") == "7");
    CHECK(programInitial("-zsh") == "Z");
    CHECK(programInitial("_x") == "X");
    CHECK(programInitial("").empty());
    CHECK(programInitial("...").empty());
}
