// SPDX-License-Identifier: Apache-2.0
//
// The embedded shell-integration scripts. What matters here is that the bytes the binary carries
// are the bytes shell-integration/ holds -- the scripts stopped travelling through Qt's resource
// system so that a GUI-less build needs no Qt, and a generator that quietly mangled them would be
// found only by a user whose shell broke.

#include <contour/ShellIntegration.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{

/// Reads a file as raw bytes with CRLF folded to LF, matching what the generator embeds.
/// @param path File to read.
/// @return Its contents, or an empty string if it could not be opened.
[[nodiscard]] std::string readNormalized(std::filesystem::path const& path)
{
    auto in = std::ifstream { path, std::ios::binary };
    if (!in)
        return {};

    auto text = std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::erase(text, '\r');
    return text;
}

} // namespace

TEST_CASE("shellIntegrationScript serves every shell the build compiled in", "[shell-integration]")
{
    auto const shells = contour::supportedShells();
    REQUIRE(!shells.empty());

    for (auto const& row: shells)
    {
        INFO("shell: " << row.name);
        auto const script = contour::shellIntegrationScript(row.name);
        REQUIRE(script.has_value());
        CHECK(!script->empty());
        CHECK(*script == row.script);

        // A shell script with CRLF in it is a broken shell script: the CR ends up part of every
        // command. The generator folds them, and a Windows checkout is exactly where it matters.
        CHECK(!script->contains('\r'));
    }
}

TEST_CASE("shellIntegrationScript rejects a shell it carries no script for", "[shell-integration]")
{
    auto const script = contour::shellIntegrationScript("nosuchshell");

    REQUIRE(!script.has_value());
    CHECK(script.error() == contour::ShellIntegrationError::UnsupportedShell);

    SECTION("and does not match a prefix of a supported one")
    {
        CHECK(!contour::shellIntegrationScript("ba").has_value());
        CHECK(!contour::shellIntegrationScript("bashx").has_value());
        CHECK(!contour::shellIntegrationScript("").has_value());
    }
}

TEST_CASE("the embedded scripts are the ones in the source tree", "[shell-integration]")
{
    auto const shellIntegrationDir =
        std::filesystem::path { CONTOUR_PROJECT_SOURCE_DIR } / "src" / "contour" / "shell-integration";

    for (auto const& row: contour::supportedShells())
    {
        INFO("shell: " << row.name);
        auto const onDisk =
            readNormalized(shellIntegrationDir / ("shell-integration." + std::string { row.name }));
        REQUIRE(!onDisk.empty());
        CHECK(row.script == onDisk);
    }
}

TEST_CASE("supportedShellsText names exactly the compiled-in shells", "[shell-integration]")
{
    auto const text = contour::supportedShellsText();
    auto const shells = contour::supportedShells();

    for (auto const& row: shells)
    {
        INFO("shell: " << row.name);
        CHECK(text.contains(row.name));
    }

    // ...and names nothing else: a joined list has one separator fewer than it has entries.
    CHECK(std::ranges::count(text, ',') == static_cast<std::ptrdiff_t>(shells.size()) - 1);
}
