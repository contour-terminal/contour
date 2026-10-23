// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <vtextension/BundledExtensions.hpp>

using vtextension::bundledExtensionDirectories;
using vtextension::BundledExtensionPlatform;
using vtextension::resolveBundledExtension;
using Paths = std::vector<std::filesystem::path>;

namespace
{

/// An environment holding only XDG_DATA_DIRS, or none at all.
[[nodiscard]] core::testing::FakeEnvironment xdgEnvironment(std::string value)
{
    return core::testing::FakeEnvironment { { { "XDG_DATA_DIRS", std::move(value) } } };
}

} // namespace

TEST_CASE("BundledExtensions.linuxSearchesEveryXdgDataDir", "[vtextension][extensions]")
{
    auto const directories = bundledExtensionDirectories(
        BundledExtensionPlatform::Linux, xdgEnvironment("/opt/share:/usr/share"), "/opt/contour/bin");
    CHECK(directories
          == Paths { "/opt/share/contour/extensions",
                     "/usr/share/contour/extensions",
                     "/opt/contour/share/contour/extensions" });
}

TEST_CASE("BundledExtensions.linuxFallsBackToTheXdgDefaultWhenUnsetOrEmpty", "[vtextension][extensions]")
{
    auto const expected = Paths { "/usr/local/share/contour/extensions",
                                  "/usr/share/contour/extensions",
                                  "/opt/contour/share/contour/extensions" };
    CHECK(bundledExtensionDirectories(
              BundledExtensionPlatform::Linux, core::testing::FakeEnvironment {}, "/opt/contour/bin")
          == expected);
    CHECK(bundledExtensionDirectories(BundledExtensionPlatform::Linux, xdgEnvironment(""), "/opt/contour/bin")
          == expected);
}

TEST_CASE("BundledExtensions.linuxIgnoresEmptyAndRelativeEntries", "[vtextension][extensions]")
{
    // The XDG specification leaves a relative entry undefined; reading one would make the answer depend on
    // the working directory.
    auto const directories = bundledExtensionDirectories(
        BundledExtensionPlatform::Linux, xdgEnvironment(":relative/dir::/usr/share:"), "/opt/contour/bin");
    CHECK(directories == Paths { "/usr/share/contour/extensions", "/opt/contour/share/contour/extensions" });
}

TEST_CASE("BundledExtensions.linuxSearchesBesideTheExecutableLast", "[vtextension][extensions]")
{
    // A prefix XDG_DATA_DIRS does not list must still find its own extensions -- but only after every
    // directory the user's environment names, so a system-wide install does not shadow an override.
    auto const lastIn = [](core::Environment const& environment) {
        auto const directories =
            bundledExtensionDirectories(BundledExtensionPlatform::Linux, environment, "/opt/contour/bin");
        REQUIRE_FALSE(directories.empty());
        return directories.back();
    };
    auto const beside = std::filesystem::path { "/opt/contour/share/contour/extensions" };
    CHECK(lastIn(xdgEnvironment("/opt/share:/usr/share")) == beside);
    CHECK(lastIn(xdgEnvironment("")) == beside);
    CHECK(lastIn(xdgEnvironment("relative/only")) == beside);
    CHECK(lastIn(core::testing::FakeEnvironment {}) == beside);

    // Found there, and only there, when the module is in no data dir.
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {
        { "/opt/contour/share/contour/extensions/git.wasm", "git" },
    };
    auto const directories = bundledExtensionDirectories(
        BundledExtensionPlatform::Linux, xdgEnvironment("/usr/share"), "/opt/contour/bin");
    CHECK(resolveBundledExtension("git", directories, fileSystem)
          == std::filesystem::path { "/opt/contour/share/contour/extensions/git.wasm" });
}

TEST_CASE("BundledExtensions.macosSearchesTheAppBundleResources", "[vtextension][extensions]")
{
    auto const directories = bundledExtensionDirectories(BundledExtensionPlatform::MacOS,
                                                         core::testing::FakeEnvironment {},
                                                         "/Applications/Contour.app/Contents/MacOS");
    REQUIRE_FALSE(directories.empty());
    CHECK(directories.front() == "/Applications/Contour.app/Contents/Resources/extensions");
}

TEST_CASE("BundledExtensions.macosWithoutABundleSearchesBesideTheExecutable", "[vtextension][extensions]")
{
    // A GUI-less build installs like any other Unix command-line tool: bin/contour, and the extensions in
    // share/contour/extensions beside it, e.g. under a Homebrew prefix.
    auto fileSystem = core::platform::testing::InMemoryFileSystem {};
    REQUIRE(fileSystem.writeFile("/opt/homebrew/share/contour/extensions/clock.wasm", "WASM").has_value());
    auto const directories = bundledExtensionDirectories(
        BundledExtensionPlatform::MacOS, core::testing::FakeEnvironment {}, "/opt/homebrew/bin");
    CHECK(resolveBundledExtension("clock", directories, fileSystem)
          == std::filesystem::path { "/opt/homebrew/share/contour/extensions/clock.wasm" });
}

TEST_CASE("BundledExtensions.windowsSearchesBesideTheExecutable", "[vtextension][extensions]")
{
    auto const directories = bundledExtensionDirectories(
        BundledExtensionPlatform::Windows, core::testing::FakeEnvironment {}, "C:/Program Files/Contour/bin");
    CHECK(directories == Paths { "C:/Program Files/Contour/share/contour/extensions" });
}

TEST_CASE("BundledExtensions.resolveTakesTheFirstDirectoryHoldingTheModule", "[vtextension][extensions]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {
        { "/second/extensions/git.wasm", "second" },
        { "/third/extensions/git.wasm", "third" },
        { "/third/extensions/plasma.wasm", "plasma" },
    };
    auto const directories = Paths { "/first/extensions", "/second/extensions", "/third/extensions" };

    CHECK(resolveBundledExtension("git", directories, fileSystem)
          == std::filesystem::path { "/second/extensions/git.wasm" });
    CHECK(resolveBundledExtension("plasma", directories, fileSystem)
          == std::filesystem::path { "/third/extensions/plasma.wasm" });
}

TEST_CASE("BundledExtensions.resolveFindsNothingForAnUnknownName", "[vtextension][extensions]")
{
    auto const fileSystem =
        core::platform::testing::InMemoryFileSystem { { "/share/extensions/git.wasm", "git" } };
    auto const directories = Paths { "/share/extensions" };

    CHECK_FALSE(resolveBundledExtension("nope", directories, fileSystem));
    CHECK_FALSE(resolveBundledExtension("git", Paths {}, fileSystem));
}

TEST_CASE("BundledExtensions.resolveSkipsADirectoryNamedLikeTheModule", "[vtextension][extensions]")
{
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {
        { .path = "/share/extensions/git.wasm", .isDirectory = true },
    };
    CHECK_FALSE(resolveBundledExtension("git", Paths { "/share/extensions" }, fileSystem));
}

TEST_CASE("BundledExtensions.resolveRefusesNamesThatEscapeTheDirectory", "[vtextension][extensions]")
{
    // A name is a YAML key anyone can write; it must never walk out of the extensions directory.
    auto const fileSystem = core::platform::testing::InMemoryFileSystem {
        { "/share/secret.wasm", "secret" },
        { "/share/extensions/a/b.wasm", "nested" },
    };
    auto const directories = Paths { "/share/extensions" };

    CHECK_FALSE(resolveBundledExtension("../secret", directories, fileSystem));
    CHECK_FALSE(resolveBundledExtension("a/b", directories, fileSystem));
    CHECK_FALSE(resolveBundledExtension("a\\b", directories, fileSystem));
    CHECK_FALSE(resolveBundledExtension("..", directories, fileSystem));
    CHECK_FALSE(resolveBundledExtension(".", directories, fileSystem));
    CHECK_FALSE(resolveBundledExtension("", directories, fileSystem));
}
