// SPDX-License-Identifier: Apache-2.0
#include <core/platform/NativeFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <vthost/DaemonImage.hpp>
#include <vthost/testing/FakeImageLeases.hpp>
#include <vthost/testing/StagingFileSystem.hpp>

namespace fs = std::filesystem;

using vthost::HostPlatform;
using vthost::LeaseState;
using vthost::StagingLayout;
using vthost::testing::appImageStoreConfig;
using vthost::testing::FakeImageLeases;
using vthost::testing::FakeSymbolicLinks;
using vthost::testing::linuxPackageStoreConfig;
using vthost::testing::StagingFileSystem;

namespace
{
/// The AppImage every staging case below stages, and what it holds.
constexpr auto AppImagePath = std::string_view { "/home/u/Contour.AppImage" };
constexpr auto AppImageContent = std::string_view { "ELF" };

/// @return The name the store gives an image of AppImageContent for @p release.
std::string appImageName(std::string_view release)
{
    return vthost::imageDirectoryName(release, AppImageContent.size(), StagingFileSystem::FixedWriteTime);
}

/// @return Whether anything directly under @p root is a partial copy.
bool holdsPartial(StagingFileSystem const& files, fs::path const& root)
{
    auto const children = files.listDirectory(root);
    REQUIRE(children.has_value());
    return std::ranges::any_of(
        *children, [](auto const& entry) { return entry.path.filename().string().contains(".partial-"); });
}
} // namespace

TEST_CASE("each platform stages what its executable needs beside it", "[vthost][image]")
{
    CHECK(vthost::stagingPlan({ .platform = HostPlatform::Linux, .executable = "/usr/bin/contour" }).layout
          == StagingLayout::InPlace); // a packaged Linux install runs in place
    CHECK(
        vthost::stagingPlan({ .platform = HostPlatform::OtherPosix, .executable = "/usr/local/bin/contour" })
            .entries
        == std::vector<fs::path> { "contour" });
    // Written with '/', so the case decomposes the same on a POSIX host as on Windows.
    auto const win = vthost::stagingPlan(
        { .platform = HostPlatform::Windows, .executable = "C:/Program Files/Contour/bin/contour.exe" });
    CHECK(win.layout == StagingLayout::InstallTree);
    CHECK(win.sourceRoot == fs::path { "C:/Program Files/Contour" });
    CHECK(win.entries == std::vector<fs::path> { "bin", fs::path { "share" } / "contour" });
    CHECK(win.executable == fs::path { "bin" } / "contour.exe");
#ifdef _WIN32
    CHECK(vthost::stagingPlan({ .platform = HostPlatform::Windows,
                                .executable = R"(C:\Program Files\Contour\bin\contour.exe)" })
              .executable
          == fs::path { "bin" } / "contour.exe");
#endif
    auto const mac =
        vthost::stagingPlan({ .platform = HostPlatform::MacOS,
                              .executable = "/Applications/Contour.app/Contents/MacOS/contour" });
    CHECK(mac.layout == StagingLayout::AppBundle);
    CHECK(mac.sourceRoot == fs::path { "/Applications" });
    CHECK(mac.entries == std::vector<fs::path> { "Contour.app" });
    CHECK(mac.executable == fs::path { "Contour.app/Contents/MacOS/contour" });
    CHECK(mac.symlinks == vthost::SymlinkPolicy::Preserve); // the bundle's signature seals its links
    CHECK(win.symlinks == vthost::SymlinkPolicy::Follow);
    CHECK(vthost::stagingPlan({ .platform = HostPlatform::Linux,
                                .executable = "/app/bin/contour",
                                .sandbox = vtpty::SandboxState::Flatpak })
              .layout
          == StagingLayout::InPlace);
    auto const appImage = vthost::stagingPlan({ .platform = HostPlatform::Linux,
                                                .executable = "/tmp/.mount_x/usr/bin/contour",
                                                .appImage = "/home/u/Contour.AppImage" });
    CHECK(appImage.layout == StagingLayout::AppImage);
    CHECK(appImage.sourceRoot == fs::path { "/home/u" });
    CHECK(appImage.entries == std::vector<fs::path> { "Contour.AppImage" });
    CHECK(appImage.executable == fs::path { "Contour.AppImage" });
}

TEST_CASE("a macOS executable outside an .app is staged on its own", "[vthost][image]")
{
    auto const plan =
        vthost::stagingPlan({ .platform = HostPlatform::MacOS, .executable = "/opt/bin/contour" });
    CHECK(plan.layout == StagingLayout::ExecutableOnly);
    CHECK(plan.sourceRoot == fs::path { "/opt/bin" });
    CHECK(plan.executable == fs::path { "contour" });
    // A directory named MacOS is not a bundle unless it sits in an .app's Contents.
    CHECK(
        vthost::stagingPlan({ .platform = HostPlatform::MacOS, .executable = "/opt/Contents/MacOS/contour" })
            .layout
        == StagingLayout::ExecutableOnly);
}

TEST_CASE("an image directory name is file-system safe and changes with the binary", "[vthost][image]")
{
    auto const t = fs::file_time_type {};
    auto const a = vthost::imageDirectoryName("0.8.1-master+abc/def", 100, t);
    CHECK(a.starts_with("0.8.1-master_abc_def-"));
    CHECK(a.size() == std::string_view { "0.8.1-master_abc_def-" }.size() + 8);
    CHECK(a != vthost::imageDirectoryName("0.8.1-master+abc/def", 101, t));
    CHECK(a != vthost::imageDirectoryName("0.8.1-master+abc/def", 100, t + std::chrono::seconds { 1 }));
    CHECK(a == vthost::imageDirectoryName("0.8.1-master+abc/def", 100, t));
    auto const hex = std::string_view { a }.substr(a.size() - 8);
    CHECK(std::ranges::all_of(hex,
                              [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }));
}

TEST_CASE("a staging error names its cause", "[vthost][image]")
{
    auto const error =
        vthost::StagingError { .code = vthost::StagingErrorCode::PublishFailed, .context = "x" };
    CHECK(error.toString().contains("x"));
    CHECK(
        error.toString()
        != vthost::StagingError { .code = vthost::StagingErrorCode::CopyFailed, .context = "x" }.toString());
}

TEST_CASE("staging copies once and reuses a complete image", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const first = store.stage();
    REQUIRE(first.has_value());
    CHECK(files.readFile(first->executable) == AppImageContent);
    CHECK(store.contains(first->executable));
    CHECK(first->lease != nullptr);
    CHECK(first->executable == fs::path { "/state/bin" } / appImageName("0.8.1") / "Contour.AppImage");
    CHECK(files.exists(store.imageDirectoryOf(first->executable) / ".complete"));
    CHECK(files.isExecutableFile(first->executable)); // owner-execute restored on the copy
    CHECK(leases.holds(store.leaseFileOf(store.imageDirectoryOf(first->executable))) == 1);
    CHECK_FALSE(holdsPartial(files, "/state/bin"));

    // A complete image is never re-copied: a change to the copy survives a second stage.
    std::ignore = files.writeFile(first->executable, "COPY");
    auto const second = store.stage();
    REQUIRE(second.has_value());
    CHECK(second->executable == first->executable);
    CHECK(files.readFile(second->executable) == "COPY");
}

TEST_CASE("a packaged Linux install is not staged and holds no lease", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile("/usr/bin/contour", "ELF");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { linuxPackageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    CHECK(staged->executable == "/usr/bin/contour");
    CHECK(staged->lease == nullptr);
    CHECK_FALSE(files.exists("/state/bin"));
}

TEST_CASE("an install tree is staged with whichever of its entries exist", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile("/opt/Contour/bin/contour.exe", "MZ");
    std::ignore = files.writeFile("/opt/Contour/bin/Qt6Core.dll", "DLL");
    std::ignore = files.writeFile("/opt/Contour/bin/plugins/platforms/qwindows.dll", "PLUGIN");
    // share/contour is missing: skipped, not an error.
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store = vthost::DaemonImageStore { vthost::DaemonImageStoreConfig {
                                                .imagesRoot = "/state/bin",
                                                .releaseVersion = "0.8.1",
                                                .inputs = { .platform = HostPlatform::Windows,
                                                            .executable = "/opt/Contour/bin/contour.exe" },
                                                .processId = 9 },
                                            files,
                                            leases,
                                            links };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    auto const image = store.imageDirectoryOf(staged->executable);
    CHECK(staged->executable == image / "bin" / "contour.exe");
    CHECK(files.readFile(image / "bin" / "Qt6Core.dll") == "DLL");
    CHECK(files.readFile(image / "bin" / "plugins" / "platforms" / "qwindows.dll") == "PLUGIN");
    CHECK_FALSE(files.exists(image / "share"));
}

namespace
{
/// The store configuration staging @p executable, a macOS bundle's, into /state/bin.
vthost::DaemonImageStoreConfig bundleStoreConfig(fs::path imagesRoot, fs::path executable)
{
    return vthost::DaemonImageStoreConfig {
        .imagesRoot = std::move(imagesRoot),
        .releaseVersion = "0.8.1",
        .inputs = { .platform = HostPlatform::MacOS, .executable = std::move(executable) },
        .processId = 7,
    };
}
} // namespace

TEST_CASE("an app bundle is staged with its symbolic links recreated", "[vthost][image]")
{
    // A framework's Versions/Current, and the links through it, are what its code signature seals:
    // a copy that flattens them can fail validation.
    auto files = StagingFileSystem {};
    auto const bundle = fs::path { "/Applications/Contour.app" };
    auto const framework = bundle / "Contents" / "Frameworks" / "QtCore.framework";
    std::ignore = files.writeFile(bundle / "Contents" / "MacOS" / "contour", "MACHO");
    std::ignore = files.writeFile(framework / "Versions" / "A" / "QtCore", "DYLIB");
    files.files.addSymlink(framework / "Versions" / "Current", "A");
    files.files.addSymlink(framework / "QtCore", "Versions/Current/QtCore");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store = vthost::DaemonImageStore {
        bundleStoreConfig("/state/bin", bundle / "Contents" / "MacOS" / "contour"), files, leases, links
    };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    auto const image = store.imageDirectoryOf(staged->executable);
    auto const stagedFramework = image / "Contour.app" / "Contents" / "Frameworks" / "QtCore.framework";
    CHECK(staged->executable == image / "Contour.app" / "Contents" / "MacOS" / "contour");
    CHECK(files.readFile(stagedFramework / "Versions" / "A" / "QtCore") == "DYLIB");
    // Recreated in the partial copy, which is then renamed into place.
    auto partialFramework = image;
    partialFramework += ".partial-7";
    partialFramework /= stagedFramework.lexically_relative(image);
    CHECK(std::ranges::contains(links.copies,
                                FakeSymbolicLinks::Copy { .link = framework / "Versions" / "Current",
                                                          .to = partialFramework / "Versions" / "Current" }));
    CHECK(std::ranges::contains(
        links.copies,
        FakeSymbolicLinks::Copy { .link = framework / "QtCore", .to = partialFramework / "QtCore" }));
    CHECK(links.copies.size() == 2);
    // Not flattened: nothing but the recreated link was put where a link was.
    CHECK_FALSE(files.exists(stagedFramework / "Versions" / "Current"));
    CHECK_FALSE(files.exists(stagedFramework / "QtCore"));
}

TEST_CASE("an unreadable executable fails staging and copies nothing", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE_FALSE(staged.has_value());
    CHECK(staged.error().code == vthost::StagingErrorCode::SourceUnreadable);
    CHECK_FALSE(files.exists("/state/bin"));
}

TEST_CASE("a lease that cannot be taken fails staging before anything is copied", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    leases.holdError =
        vthost::StagingError { .code = vthost::StagingErrorCode::LeaseFailed, .context = "denied" };
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE_FALSE(staged.has_value());
    CHECK(staged.error().code == vthost::StagingErrorCode::LeaseFailed);
    CHECK_FALSE(files.exists(fs::path { "/state/bin" } / appImageName("0.8.1")));
}

TEST_CASE("a staged image stays leased while its StagedImage lives", "[vthost][image]")
{
    // Review Focus 4, the spawn window. Launcher A (0.8.1) stages and keeps its StagedImage, as
    // ensureDaemon does until the spawned daemon serves. Launcher B (0.8.2, same imagesRoot) stages
    // and prunes: 0.8.1's directory survives. A drops its StagedImage; B prunes again: removed.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto storeA =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto storeB =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.2"), files, leases, links };

    auto stagedA = std::optional { storeA.stage() };
    REQUIRE(stagedA->has_value());
    auto const imageA = storeA.imageDirectoryOf((*stagedA)->executable);
    auto const stagedB = storeB.stage();
    REQUIRE(stagedB.has_value());

    CHECK(storeB.prune() == 0);
    CHECK(files.exists(imageA / ".complete"));

    stagedA.reset();
    CHECK(storeB.prune() == 1);
    CHECK_FALSE(files.exists(imageA));
    CHECK_FALSE(files.exists(storeA.leaseFileOf(imageA)));
    CHECK(files.exists(storeB.imageDirectoryOf(stagedB->executable) / ".complete"));
}

TEST_CASE("the lease is taken before the completion marker is read", "[vthost][image]")
{
    // A prune from a newer launcher races the check: it runs inside hold(), before the hold is
    // recorded, and removes the complete image. stage() must then find no .complete and re-stage
    // rather than return a path into the directory that was just removed.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto newer =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.2"), files, leases, links };
    REQUIRE(store.stage().has_value()); // complete, and no longer leased

    auto pruned = std::size_t { 0 };
    leases.beforeHold = [&](fs::path const&) {
        pruned = newer.prune();
    };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    CHECK(pruned == 1);
    CHECK(files.readFile(staged->executable) == AppImageContent);
    CHECK(files.exists(store.imageDirectoryOf(staged->executable) / ".complete"));
}

TEST_CASE("a target without its completion marker is never handed out", "[vthost][image]")
{
    // Review Focus 3: a launcher died mid-publish, or something else left the directory behind.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto const target = fs::path { "/state/bin" } / appImageName("0.8.1");
    std::ignore = files.writeFile(target / "Contour.AppImage", "EL"); // truncated, no .complete
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    CHECK(staged->executable == target / "Contour.AppImage");
    CHECK(files.readFile(staged->executable) == AppImageContent);
    CHECK(files.exists(target / ".complete"));
}

TEST_CASE("a losing concurrent stager uses the winner's image", "[vthost][image]")
{
    // Review Focus 3: the other launcher publishes its complete copy between this one's check and
    // its rename, so the rename fails onto the winner's directory.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto const target = fs::path { "/state/bin" } / appImageName("0.8.1");
    files.beforeRename = [&](fs::path const&, fs::path const& to) {
        if (to != target || files.exists(target))
            return;
        std::ignore = files.writeFile(target / "Contour.AppImage", "WINNER");
        std::ignore = files.writeFile(target / ".complete", "");
    };
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    CHECK(staged->executable == target / "Contour.AppImage");
    CHECK(files.readFile(staged->executable) == "WINNER");
    CHECK(staged->lease != nullptr);
    CHECK_FALSE(holdsPartial(files, "/state/bin"));
}

TEST_CASE("clearing an incomplete target never removes a launcher's complete image", "[vthost][image]")
{
    // Review Focus 3, with an incomplete directory in the way of two launchers. This one finds it
    // incomplete; before it moves it aside, the other clears it and publishes its complete image
    // there. That image is leased and handed out: it must survive, and be the one used.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto const target = fs::path { "/state/bin" } / appImageName("0.8.1");
    std::ignore = files.writeFile(target / "Contour.AppImage", "EL"); // truncated, no .complete
    // The window is between this launcher's check and its disposing of the target, by removal or by
    // a move: the other launcher's publish lands at whichever comes first.
    auto otherPublished = false;
    auto const otherPublishes = [&](fs::path const& path) {
        if (path != target || otherPublished)
            return;
        otherPublished = true;
        std::ignore = files.files.removeAll(target);
        std::ignore = files.writeFile(target / "Contour.AppImage", "WINNER");
        std::ignore = files.writeFile(target / ".complete", "");
    };
    files.beforeRemoveAll = otherPublishes;
    files.beforeRename = [&](fs::path const& from, fs::path const&) {
        otherPublishes(from);
    };
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE(otherPublished);
    REQUIRE(staged.has_value());
    CHECK(staged->executable == target / "Contour.AppImage");
    CHECK(files.readFile(target / "Contour.AppImage") == "WINNER");
    CHECK(files.exists(target / ".complete"));
    CHECK_FALSE(holdsPartial(files, "/state/bin"));
}

TEST_CASE("a publish that fails with no winner reports it and releases the lease", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto const target = fs::path { "/state/bin" } / appImageName("0.8.1");
    // Something that is not an image keeps occupying the target's name, however often it is cleared.
    files.beforeRename = [&](fs::path const&, fs::path const& to) {
        if (to == target)
            std::ignore = files.writeFile(target / "junk", "");
    };
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const staged = store.stage();
    REQUIRE_FALSE(staged.has_value());
    CHECK(staged.error().code == vthost::StagingErrorCode::PublishFailed);
    CHECK(leases.holds(store.leaseFileOf(target)) == 0);
    CHECK_FALSE(holdsPartial(files, "/state/bin"));
}

TEST_CASE("prune removes free images and keeps held and current ones", "[vthost][image]")
{
    // Review Focus 4.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const root = fs::path { "/state/bin" };
    std::ignore = files.writeFile(root / "0.8.0-aaaaaaaa" / "contour", "OLD");
    std::ignore = files.writeFile(root / "0.8.0-aaaaaaaa" / ".complete", "");
    std::ignore = files.writeFile(root / "0.8.0-aaaaaaaa.lease", "");
    std::ignore = files.writeFile(root / "0.7.9-bbbbbbbb" / "contour", "HELD");
    std::ignore = files.writeFile(root / "0.7.9-bbbbbbbb.lease", "");
    std::ignore = files.writeFile(root / "0.8.1-cccccccc.partial-77" / "contour", "HALF");
    auto const held = leases.hold(root / "0.7.9-bbbbbbbb.lease");
    REQUIRE(held.has_value());
    auto const current = store.stage();
    REQUIRE(current.has_value());
    auto const currentImage = store.imageDirectoryOf(current->executable);

    CHECK(store.prune() == 2);
    CHECK_FALSE(files.exists(root / "0.8.0-aaaaaaaa"));
    CHECK_FALSE(files.exists(root / "0.8.0-aaaaaaaa.lease"));
    CHECK_FALSE(files.exists(root / "0.8.1-cccccccc.partial-77"));
    CHECK(files.exists(root / "0.7.9-bbbbbbbb" / "contour"));
    CHECK(files.exists(root / "0.7.9-bbbbbbbb.lease"));
    CHECK(files.exists(currentImage / ".complete"));
}

TEST_CASE("prune keeps the current image even when nothing leases it", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const image = [&] {
        auto const staged = store.stage();
        REQUIRE(staged.has_value());
        return store.imageDirectoryOf(staged->executable);
    }();
    CHECK(store.prune() == 0);
    CHECK(files.exists(image / ".complete"));
}

TEST_CASE("a lease taken after prune listed an image keeps it", "[vthost][image]")
{
    // Review Focus 4, the race a probe-then-remove prune loses: an older launcher leases its image
    // after the newer launcher's prune has listed it. The removal is decided by the claim, taken
    // where the removal happens, so the image survives and the launcher's path stays valid.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto older =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto newer =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.2"), files, leases, links };
    REQUIRE(older.stage().has_value()); // complete, and no longer leased
    REQUIRE(newer.stage().has_value());

    auto lateLauncher = std::optional<std::expected<vthost::StagedImage, vthost::StagingError>> {};
    leases.beforeClaim = [&](fs::path const&) {
        if (!lateLauncher)
            lateLauncher = older.stage();
    };
    CHECK(newer.prune() == 0);
    REQUIRE(lateLauncher.has_value());
    REQUIRE(lateLauncher->has_value());
    CHECK(files.readFile((*lateLauncher)->executable) == AppImageContent);
    CHECK(files.exists(older.imageDirectoryOf((*lateLauncher)->executable) / ".complete"));

    leases.beforeClaim = nullptr;
    lateLauncher.reset();
    CHECK(newer.prune() == 1);
}

TEST_CASE("a store in a fallback state directory runs in place and prunes nothing", "[vthost][image]")
{
    // Other local users can pre-create <temp>/contour-state-<user>: nothing is executed out of it.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, AppImageContent);
    std::ignore = files.writeFile("/tmp/state/bin/0.8.0-aaaaaaaa/contour", "PLANTED");
    std::ignore = files.writeFile("/tmp/state/bin/0.8.0-aaaaaaaa/.complete", "");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto config = appImageStoreConfig("/tmp/state/bin", "0.8.1");
    config.stateOrigin = vthost::StateDirectoryOrigin::TemporaryFallback;
    auto store = vthost::DaemonImageStore { std::move(config), files, leases, links };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    CHECK(staged->executable == fs::path { "/tmp/.mount_x/usr/bin/contour" });
    CHECK(staged->lease == nullptr);
    CHECK_FALSE(files.exists(fs::path { "/tmp/state/bin" } / appImageName("0.8.1")));
    CHECK(store.prune() == 0);
    CHECK(files.exists("/tmp/state/bin/0.8.0-aaaaaaaa/contour"));
}

TEST_CASE("prune on a store that never staged removes nothing and fails nothing", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    CHECK(store.prune() == 0);
}

TEST_CASE("the store knows what lies under it", "[vthost][image]")
{
    auto files = StagingFileSystem {};
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto const store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    CHECK(store.contains("/state/bin/0.8.1-01234567/Contour.AppImage"));
    CHECK_FALSE(store.contains("/state/binary/contour"));
    CHECK_FALSE(store.contains("/state/bin/../other/contour"));
    CHECK_FALSE(store.contains("/usr/bin/contour"));
    CHECK(store.imageDirectoryOf("/state/bin/0.8.1-01234567/bin/contour.exe")
          == fs::path { "/state/bin/0.8.1-01234567" });
    CHECK(store.imageDirectoryOf("/usr/bin/contour").empty());
    CHECK(store.leaseFileOf("/state/bin/0.8.1-01234567") == fs::path { "/state/bin/0.8.1-01234567.lease" });
}

// Portable: what follows runs against each platform's own lease backend, flock or share modes.
TEST_CASE("a native lease survives its image's publish and ends with its holder",
          "[vthost][image][native-lease]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-lease" };
    auto const leaseFile = temp.path() / "img.lease";
    fs::create_directory(temp.path() / "img.partial-1");
    std::ofstream { temp.path() / "img.partial-1" / "contour" } << "ELF";

    auto const holder = vthost::makeNativeImageLeases();
    auto const prober = vthost::makeNativeImageLeases();
    CHECK(prober->probe(leaseFile) == LeaseState::Free); // no lease file yet
    auto lease = holder->hold(leaseFile);
    REQUIRE(lease.has_value());
    CHECK(prober->probe(leaseFile) == LeaseState::Held);

    fs::rename(temp.path() / "img.partial-1", temp.path() / "img");
    CHECK(prober->probe(leaseFile) == LeaseState::Held);

    lease->reset();
    CHECK(prober->probe(leaseFile) == LeaseState::Free);
}

TEST_CASE("native leases are shared among holders", "[vthost][image][native-lease]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-lease" };
    auto const leaseFile = temp.path() / "img.lease";
    auto const leases = vthost::makeNativeImageLeases();
    auto first = leases->hold(leaseFile);
    auto second = leases->hold(leaseFile);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    first->reset();
    CHECK(leases->probe(leaseFile) == LeaseState::Held);
    second->reset();
    CHECK(leases->probe(leaseFile) == LeaseState::Free);
}

TEST_CASE("a native claim is refused while a lease is held", "[vthost][image][native-lease]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-lease" };
    auto const leaseFile = temp.path() / "img.lease";
    auto const leases = vthost::makeNativeImageLeases();
    auto lease = leases->hold(leaseFile);
    REQUIRE(lease.has_value());
    CHECK(leases->claim(leaseFile) == nullptr);
    lease->reset();
    CHECK(leases->claim(leaseFile) != nullptr);
}

// POSIX only: a thread blocked in flock(), symbolic links (which Windows creates only with a
// privilege), and the execute bit the real-file-system case checks.
#ifndef _WIN32
TEST_CASE("a native hold waiting out a prune leases the file that is there afterwards",
          "[vthost][image][native-lease][threading]")
{
    // The prune claims the lease, removes the image and unlinks the lease file, then lets go. A hold
    // that blocked meanwhile wakes up locking the unlinked inode; it must start again on the file at
    // the path, or the next prune finds that file free and removes an image in use.
    auto const temp = core::testing::ScopedTempDir { "contour-lease" };
    auto const leaseFile = temp.path() / "img.lease";
    auto const leases = vthost::makeNativeImageLeases();
    auto claim = leases->claim(leaseFile);
    REQUIRE(claim != nullptr);

    auto held = std::expected<std::unique_ptr<vthost::ImageLease>, vthost::StagingError> {};
    auto holder = std::thread { [&] { held = leases->hold(leaseFile); } };
    // Gives the holder time to open the old lease file and block on it. Were it later, it would
    // open the new file directly: the case would still pass, only without exercising the retry.
    std::this_thread::sleep_for(std::chrono::milliseconds { 200 });
    fs::remove(leaseFile);
    claim.reset();
    holder.join();

    REQUIRE(held.has_value());
    CHECK(fs::exists(leaseFile));
    CHECK(leases->probe(leaseFile) == LeaseState::Held);
    CHECK(leases->claim(leaseFile) == nullptr);
    held->reset();
    CHECK(leases->probe(leaseFile) == LeaseState::Free);
}

TEST_CASE("an app bundle's symbolic links survive staging on the real file system",
          "[vthost][image][native-lease]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-bundle" };
    auto const bundle = temp.path() / "Contour.app";
    auto const framework = bundle / "Contents" / "Frameworks" / "QtCore.framework";
    fs::create_directories(bundle / "Contents" / "MacOS");
    fs::create_directories(framework / "Versions" / "A");
    std::ofstream { bundle / "Contents" / "MacOS" / "contour" } << "MACHO";
    std::ofstream { framework / "Versions" / "A" / "QtCore" } << "DYLIB";
    fs::create_directory_symlink("A", framework / "Versions" / "Current");
    fs::create_symlink("Versions/Current/QtCore", framework / "QtCore");

    auto const files = core::platform::NativeFileSystem {};
    auto const leases = vthost::makeNativeImageLeases();
    auto const links = vthost::makeNativeSymbolicLinks();
    auto store = vthost::DaemonImageStore { bundleStoreConfig(temp.path() / "bin",
                                                              bundle / "Contents" / "MacOS" / "contour"),
                                            files,
                                            *leases,
                                            *links };
    auto const staged = store.stage();
    REQUIRE(staged.has_value());
    auto const stagedFramework = store.imageDirectoryOf(staged->executable) / "Contour.app" / "Contents"
                                 / "Frameworks" / "QtCore.framework";
    CHECK(fs::is_symlink(stagedFramework / "Versions" / "Current"));
    CHECK(fs::read_symlink(stagedFramework / "Versions" / "Current") == fs::path { "A" });
    CHECK(fs::is_symlink(stagedFramework / "QtCore"));
    CHECK(fs::read_symlink(stagedFramework / "QtCore") == fs::path { "Versions/Current/QtCore" });
    CHECK(files.readFile(stagedFramework / "QtCore") == "DYLIB"); // resolves inside the copy
    CHECK(files.isExecutableFile(staged->executable));
}

TEST_CASE("an image is staged, reused and pruned on the real file system", "[vthost][image][native-lease]")
{
    // The in-memory cases model rename(2) and the leases; this one runs them for real.
    auto const temp = core::testing::ScopedTempDir { "contour-image" };
    auto const appImage = temp.path() / "Contour.AppImage";
    std::ofstream { appImage } << AppImageContent;
    auto const files = core::platform::NativeFileSystem {};
    auto const leases = vthost::makeNativeImageLeases();
    auto const links = vthost::makeNativeSymbolicLinks();
    auto const configFor = [&](std::string release) {
        return vthost::DaemonImageStoreConfig { .imagesRoot = temp.path() / "bin",
                                                .releaseVersion = std::move(release),
                                                .inputs = { .platform = HostPlatform::Linux,
                                                            .executable = "/tmp/.mount_x/usr/bin/contour",
                                                            .appImage = appImage },
                                                .processId = 1 };
    };
    auto older = vthost::DaemonImageStore { configFor("0.8.1"), files, *leases, *links };
    auto newer = vthost::DaemonImageStore { configFor("0.8.2"), files, *leases, *links };

    auto staged = std::optional { older.stage() };
    REQUIRE(staged->has_value());
    auto const executable = (*staged)->executable;
    auto const image = older.imageDirectoryOf(executable);
    CHECK(files.readFile(executable) == AppImageContent);
    CHECK(files.isExecutableFile(executable));
    auto again = std::optional { older.stage() };
    REQUIRE(again->has_value());
    CHECK((*again)->executable == executable);

    REQUIRE(newer.stage().has_value());
    CHECK(newer.prune() == 0); // two leases of 0.8.1 held
    staged.reset();
    CHECK(newer.prune() == 0); // one still held
    again.reset();
    CHECK(newer.prune() == 1);
    CHECK_FALSE(fs::exists(image));
    CHECK_FALSE(fs::exists(older.leaseFileOf(image)));
}
#endif
