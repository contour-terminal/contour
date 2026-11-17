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
/// The home directory the cases resolve `~` against.
[[nodiscard]] std::filesystem::path home()
{
    return "/home/u";
}
} // namespace

TEST_CASE("TabIcon.autoShowsTheRunningProgramsInitialElseTheRowsOwn", "[contour][tabicon]")
{
    CHECK(tabIconOf("auto", "/usr/bin/nvim", home())
          == TabIcon { .kind = TabIconKind::Monogram, .initial = "N", .image = {} });
    CHECK(tabIconOf("auto", "", home()) == TabIcon {});
    // An unset key is auto.
    CHECK(tabIconOf("", "htop", home()).kind == TabIconKind::Monogram);
}

TEST_CASE("TabIcon.noneHidesItAndAnythingElseIsAnImage", "[contour][tabicon]")
{
    CHECK(tabIconOf("none", "nvim", home())
          == TabIcon { .kind = TabIconKind::Hidden, .initial = {}, .image = {} });
    auto const image = tabIconOf("/icons/pwsh.png", "", home());
    CHECK(image.kind == TabIconKind::Image);
    CHECK(image.image == std::filesystem::path { "/icons/pwsh.png" });
    // A leading ~ is the home directory, as for every other path in the configuration.
    CHECK(tabIconOf("~/icons/a.svg", "", home()).image == home() / "icons" / "a.svg");
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
