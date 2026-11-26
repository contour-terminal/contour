// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <vtextension/wasm/remote/process/HelperLocation.hpp>

using std::filesystem::path;
using vtextension::wasm::remote::process::ExtensionHostName;
using vtextension::wasm::remote::process::HelperLocationError;
using vtextension::wasm::remote::process::locateExtensionHost;

namespace
{

/// One lookup: where the program runs, which paths are executables, and what is found.
struct LocationCase
{
    std::string name;
    path self;
    std::set<path> executables;
    std::expected<path, HelperLocationError> found;
};

/// @return The cases of this platform's layout.
[[nodiscard]] std::vector<LocationCase> locationCases()
{
    auto const none = std::unexpected(HelperLocationError::NotFound);
#ifdef _WIN32
    auto const installed = path { "C:\\Contour\\bin\\contour-extension-host.exe" };
    return {
        { .name = "beside",
          .self = "C:\\Contour\\bin\\contour.exe",
          .executables = { installed },
          .found = installed },
        { .name = "none", .self = "C:\\Contour\\bin\\contour.exe", .executables = {}, .found = none },
    };
#elifdef __APPLE__
    auto const helpers = path { "/Applications/Contour.app/Contents/Helpers" } / ExtensionHostName;
    auto const beside = path { "/Applications/Contour.app/Contents/MacOS" } / ExtensionHostName;
    auto const self = path { "/Applications/Contour.app/Contents/MacOS/contour" };
    return {
        { .name = "bundle", .self = self, .executables = { helpers, beside }, .found = helpers },
        { .name = "beside", .self = self, .executables = { beside }, .found = beside },
        { .name = "none", .self = self, .executables = {}, .found = none },
    };
#else
    auto const libexec =
        (path { "/usr/bin" } / CONTOUR_EXTENSION_HOST_RELATIVE_DIR / ExtensionHostName).lexically_normal();
    auto const beside = path { "/usr/bin" } / ExtensionHostName;
    return {
        { .name = "libexec",
          .self = "/usr/bin/contour",
          .executables = { libexec, beside },
          .found = libexec },
        { .name = "beside", .self = "/usr/bin/contour", .executables = { beside }, .found = beside },
        { .name = "none", .self = "/usr/bin/contour", .executables = {}, .found = none },
    };
#endif
}

} // namespace

TEST_CASE("HelperLocation.candidatesInOrder", "[vtextension]")
{
    for (auto const& row: locationCases())
    {
        INFO(row.name);
        auto const isExecutable = [&row](path const& candidate) {
            return row.executables.contains(candidate.lexically_normal());
        };
        CHECK(locateExtensionHost(row.self, isExecutable) == row.found);
    }
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("HelperLocation.libexecIsNotBesideTheBinaries", "[vtextension]")
{
    // The relative directory the install computed leads out of bin/, into the private executables' directory.
    CHECK(path { CONTOUR_EXTENSION_HOST_RELATIVE_DIR }.filename() == "contour");
    CHECK(path { CONTOUR_EXTENSION_HOST_RELATIVE_DIR }.is_relative());
}
#endif
