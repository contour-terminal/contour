// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

#include <core/Utf8Path.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/IoBackend.hpp>
#include <core/net/Sockets.hpp>
#include <core/testing/Environment.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonImage.hpp>
#include <vthost/DaemonSpawn.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/testing/FakeImageLeases.hpp>
#include <vthost/testing/StagingFileSystem.hpp>

#ifndef _WIN32
    #include <sys/file.h>
    #include <sys/stat.h>

    #include <ranges>
    #include <string_view>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

using vthost::DaemonPresence;
using vthost::testing::appImageStoreConfig;
using vthost::testing::FakeImageLeases;
using vthost::testing::FakeSymbolicLinks;
using vthost::testing::linuxPackageStoreConfig;
using vthost::testing::StagingFileSystem;

namespace
{
/// The AppImage the staging cases below stage.
constexpr auto AppImagePath = "/home/u/Contour.AppImage";

/// A DaemonBinarySource that counts how often it is asked, and hands out the installed binary.
class CountingBinarySource final: public vthost::DaemonBinarySource
{
  public:
    [[nodiscard]] vthost::StagedImage imageForSpawn() override
    {
        ++calls;
        return vthost::StagedImage { .executable = "contour-no-such-binary", .lease = nullptr };
    }

    std::size_t calls = 0;
};

/// A DaemonSpawner that starts nothing: it records each spawn, and answers each probe as told.
class FakeSpawner final: public vthost::DaemonSpawner
{
  public:
    [[nodiscard]] std::expected<void, std::string> spawn(std::span<std::string const> argv,
                                                         vtpty::ProcessPlacement&) override
    {
        spawned.emplace_back(argv.begin(), argv.end());
        if (spawnError)
            return std::unexpected(*spawnError);
        return {};
    }

    [[nodiscard]] DaemonPresence probe(fs::path const&) override
    {
        ++probes;
        return answer ? answer() : DaemonPresence::Absent;
    }

    /// What each probe answers; Absent when unset.
    std::function<DaemonPresence()> answer;

    /// What every spawn fails with, when set.
    std::optional<std::string> spawnError;

    std::vector<std::vector<std::string>> spawned;
    std::size_t probes = 0;
};

/// A ProcessImageReplacer that records what it was asked to run, and fails as an execv would.
class FakeReplacer final: public vthost::ProcessImageReplacer
{
  public:
    [[nodiscard]] std::error_code replaceWith(fs::path const& image,
                                              std::span<std::string const> argv) override
    {
        replacements.emplace_back(image, std::vector<std::string>(argv.begin(), argv.end()));
        if (onReplace)
            onReplace();
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }

    /// Runs inside replaceWith(), where the exec would happen.
    std::function<void()> onReplace;

    std::vector<std::pair<fs::path, std::vector<std::string>>> replacements;
};

/// A unix endpoint nothing listens on.
[[nodiscard]] vthost::AttachEndpoint unboundEndpoint()
{
    return vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = "/nonexistent/contour/control" } };
}
} // namespace

TEST_CASE("only an installed, staging POSIX daemon re-executes from its staged copy", "[vthost][relocate]")
{
    using enum vthost::Relocation;
    using vthost::HostPlatform;
    using vthost::ImagePlacement;
    using vthost::RelocationRequest;
    using vthost::StagingLayout;
    CHECK(vthost::relocationFor(StagingLayout::AppImage,
                                HostPlatform::Linux,
                                ImagePlacement::Installed,
                                RelocationRequest::Allowed)
          == ExecStaged);
    CHECK(vthost::relocationFor(StagingLayout::AppBundle,
                                HostPlatform::MacOS,
                                ImagePlacement::Installed,
                                RelocationRequest::Allowed)
          == ExecStaged);
    CHECK(
        vthost::relocationFor(
            StagingLayout::AppImage, HostPlatform::Linux, ImagePlacement::Staged, RelocationRequest::Allowed)
        == RunInPlace);
    CHECK(vthost::relocationFor(StagingLayout::AppBundle,
                                HostPlatform::MacOS,
                                ImagePlacement::Installed,
                                RelocationRequest::Refused)
          == RunInPlace);
    CHECK(vthost::relocationFor(StagingLayout::InstallTree,
                                HostPlatform::Windows,
                                ImagePlacement::Installed,
                                RelocationRequest::Allowed)
          == RunInPlace);
    CHECK(vthost::relocationFor(StagingLayout::InPlace,
                                HostPlatform::Linux,
                                ImagePlacement::Installed,
                                RelocationRequest::Allowed)
          == RunInPlace); // a packaged Linux install
}

TEST_CASE("an installed binary source hands out its binary with no lease", "[vthost][relocate]")
{
    auto source = vthost::InstalledDaemonBinary { "/usr/bin/contour" };
    auto const image = source.imageForSpawn();
    CHECK(image.executable == fs::path { "/usr/bin/contour" });
    CHECK(image.lease == nullptr);
}

TEST_CASE("a staged binary source stages lazily and falls back to the installed binary", "[vthost][relocate]")
{
    auto files = StagingFileSystem {};
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto source = vthost::StagedDaemonBinary { store, AppImagePath };
    CHECK_FALSE(files.exists("/state/bin"));     // nothing before a spawn is needed
    auto const missing = source.imageForSpawn(); // the source is missing: fall back
    CHECK(missing.executable == AppImagePath);
    CHECK(missing.lease == nullptr);
    std::ignore = files.writeFile(AppImagePath, "ELF");
    auto const staged = source.imageForSpawn();
    CHECK(store.contains(staged.executable));
    CHECK(staged.lease != nullptr);
}

TEST_CASE("a staged binary source of an in-place install hands out the installed binary",
          "[vthost][relocate]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile("/usr/bin/contour", "ELF");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { linuxPackageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto source = vthost::StagedDaemonBinary { store, "contour" };
    auto const image = source.imageForSpawn();
    CHECK(image.executable == fs::path { "contour" }); // as the caller spelled it, PATH lookup and all
    CHECK(image.lease == nullptr);
    CHECK_FALSE(files.exists("/state/bin"));
}

TEST_CASE("a staged binary source prunes the images nobody leases once its own is held", "[vthost][relocate]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, "ELF");
    std::ignore = files.writeFile("/state/bin/0.8.0-aaaaaaaa/Contour.AppImage", "OLD");
    std::ignore = files.writeFile("/state/bin/0.8.0-aaaaaaaa/.complete", "");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto source = vthost::StagedDaemonBinary { store, AppImagePath };

    // The prune runs while the new image is leased: were it not, a prune of another launcher racing
    // this one could remove it. The claim of each candidate is where that would show.
    auto heldDuringPrune = std::optional<std::size_t> {};
    leases.beforeClaim = [&](fs::path const&) {
        if (!heldDuringPrune)
            heldDuringPrune = leases.holds(store.leaseFileOf(store.imageDirectoryOf(
                fs::path { "/state/bin" }
                / vthost::imageDirectoryName("0.8.1", 3, StagingFileSystem::FixedWriteTime)
                / "Contour.AppImage")));
    };
    auto const staged = source.imageForSpawn();
    REQUIRE(staged.lease != nullptr);
    CHECK(heldDuringPrune == 1);
    CHECK_FALSE(files.exists("/state/bin/0.8.0-aaaaaaaa"));
    CHECK(files.exists(store.imageDirectoryOf(staged.executable) / ".complete"));
}

TEST_CASE("a launched image is the AppImage when there is one", "[vthost][relocate]")
{
    CHECK(vthost::launchedImage({ .executable = "/usr/bin/contour" }) == fs::path { "/usr/bin/contour" });
    CHECK(vthost::launchedImage({ .executable = "/tmp/.mount_x/usr/bin/contour", .appImage = AppImagePath })
          == fs::path { AppImagePath });
}

TEST_CASE("a daemon knows whether it runs from a staged image", "[vthost][relocate]")
{
    auto files = StagingFileSystem {};
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto const store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    using vthost::ImagePlacement;
    CHECK(vthost::imagePlacementOf(
              store, { .executable = "/tmp/.mount_x/usr/bin/contour", .appImage = AppImagePath })
          == ImagePlacement::Installed);
    // A staged AppImage mounts itself afresh: its executable lies under /tmp, its AppImage in the store.
    CHECK(vthost::imagePlacementOf(store,
                                   { .executable = "/tmp/.mount_y/usr/bin/contour",
                                     .appImage = "/state/bin/0.8.1-01234567/Contour.AppImage" })
          == ImagePlacement::Staged);
    CHECK(vthost::imagePlacementOf(store, { .executable = "/state/bin/0.8.1-01234567/bin/contour" })
          == ImagePlacement::Staged);
    // Re-executed from its image with the `$APPIMAGE` of the installed one still set.
    CHECK(vthost::imagePlacementOf(
              store, { .executable = "/state/bin/0.8.1-01234567/contour", .appImage = AppImagePath })
          == ImagePlacement::Staged);
}

TEST_CASE("this process's image store is derived from its environment", "[vthost][relocate]")
{
    auto const process = vthost::OwnProcess { .executable = "/tmp/.mount_x/usr/bin/contour",
                                              .sandbox = vtpty::SandboxState::Host,
                                              .releaseVersion = "0.8.1",
                                              .processId = 42 };
    auto const env = core::testing::FakeEnvironment { { { "XDG_STATE_HOME", "/home/u/.state" },
                                                        { "LOCALAPPDATA", "C:/Users/u/AppData/Local" },
                                                        { "APPIMAGE", AppImagePath },
                                                        { "HOME", "/home/u" } } };
    auto const config = vthost::ownImageStoreConfig(env, process);
#ifdef _WIN32
    CHECK(config.imagesRoot == fs::path { "C:/Users/u/AppData/Local/contour/bin" });
#else
    CHECK(config.imagesRoot == fs::path { "/home/u/.state/contour/bin" });
#endif
    CHECK(config.releaseVersion == "0.8.1");
    CHECK(config.processId == 42);
    CHECK(config.inputs.platform == vthost::NativeHostPlatform);
    CHECK(config.inputs.executable == process.executable);
    CHECK(config.inputs.appImage == fs::path { AppImagePath });
    CHECK(config.inputs.sandbox == vtpty::SandboxState::Host);
    CHECK(config.stateOrigin == vthost::StateDirectoryOrigin::Configured);

    // Neither a state directory of its own nor an AppImage: the shared temp directory runs nothing.
    auto const bare = vthost::ownImageStoreConfig(core::testing::FakeEnvironment {}, process);
    CHECK_FALSE(bare.inputs.appImage.has_value());
    CHECK(bare.stateOrigin == vthost::StateDirectoryOrigin::TemporaryFallback);

    auto const flatpak = vthost::ownImageStoreConfig(
        env,
        vthost::OwnProcess { .executable = "/app/bin/contour", .sandbox = vtpty::SandboxState::Flatpak });
    CHECK(flatpak.inputs.sandbox == vtpty::SandboxState::Flatpak);
}

TEST_CASE("a daemon re-executes from its staged image, keeping the lease across the exec",
          "[vthost][relocate]")
{
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, "ELF");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto replacer = FakeReplacer {};
    auto handOversAtExec = std::optional<std::size_t> {};
    auto holdsAtExec = std::optional<std::size_t> {};
    replacer.onReplace = [&] {
        handOversAtExec = leases.handOvers;
        holdsAtExec = leases.heldCount();
    };

    auto const failure = vthost::execFromStagedImage(
        store, leases, replacer, std::vector<std::string> { AppImagePath, "daemon", "--socket", "/run/s" });

    REQUIRE(replacer.replacements.size() == 1);
    auto const& [image, argv] = replacer.replacements.front();
    CHECK(store.contains(image));
    CHECK(argv == std::vector<std::string> { core::pathToUtf8(image), "daemon", "--socket", "/run/s" });
    CHECK(handOversAtExec == 1); // made inheritable right before the exec, and not before staging
    CHECK(holdsAtExec == 1);
    CHECK_FALSE(failure.empty());   // it returned, so it failed, and says why
    CHECK(leases.heldCount() == 0); // the failed hand-over's lease is not kept by a daemon running in place
}

TEST_CASE("a daemon whose image cannot be staged is not re-executed", "[vthost][relocate]")
{
    auto files = StagingFileSystem {}; // the AppImage is missing
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto replacer = FakeReplacer {};
    auto const failure = vthost::execFromStagedImage(
        store, leases, replacer, std::vector<std::string> { AppImagePath, "daemon" });
    CHECK(replacer.replacements.empty());
    CHECK_FALSE(failure.empty());
}

TEST_CASE("a daemon already re-executed from its image is not re-executed again", "[vthost][relocate]")
{
    // A re-executed process that still reads as installed -- its executable and `$APPIMAGE` both outside
    // the store -- would move again, and again. The lease it inherited for that very image says it moved.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, "ELF");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const image =
        fs::path { "/state/bin" } / vthost::imageDirectoryName("0.8.1", 3, StagingFileSystem::FixedWriteTime);
    leases.inherit(store.leaseFileOf(image));
    auto replacer = FakeReplacer {};

    auto const failure = vthost::execFromStagedImage(
        store, leases, replacer, std::vector<std::string> { AppImagePath, "daemon" });

    CHECK(replacer.replacements.empty());
    CHECK(failure.contains("already"));
    CHECK(leases.adoptions == 1);
    CHECK(leases.heldCount() == 0); // it runs in place, holding nothing of the image
    CHECK(leases.handOvers == 0);
}

TEST_CASE("a daemon running from a staged image leases it, adopting a lease handed across exec",
          "[vthost][relocate]")
{
    auto files = StagingFileSystem {};
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto const image = vthost::StagingInputs { .executable = "/tmp/.mount_y/usr/bin/contour",
                                               .appImage = "/state/bin/0.8.1-01234567/Contour.AppImage" };
    auto const leaseFile = fs::path { "/state/bin/0.8.1-01234567.lease" };

    SECTION("spawned by a launcher: a lease of its own")
    {
        auto lease = vthost::leaseRunningImage(store, leases, image);
        REQUIRE(lease.has_value());
        CHECK(leases.holds(leaseFile) == 1);
        lease->reset();
        CHECK(leases.holds(leaseFile) == 0);
    }
    SECTION("running from no image: no lease")
    {
        auto const lease = vthost::leaseRunningImage(store, leases, { .executable = "/usr/bin/contour" });
        REQUIRE_FALSE(lease.has_value());
        CHECK(lease.error().code == vthost::StagingErrorCode::LeaseFailed);
        CHECK(leases.heldCount() == 0);
    }
    SECTION("re-executed: the inherited lease, adopted rather than doubled")
    {
        leases.inherit(leaseFile);
        auto lease = vthost::leaseRunningImage(store, leases, image);
        REQUIRE(lease.has_value());
        CHECK(leases.holds(leaseFile) == 1);
        CHECK(leases.adoptions == 1);
        lease->reset();
        CHECK(leases.holds(leaseFile) == 0);
    }
}

TEST_CASE("a daemon leasing its running image prunes the free images and keeps its own", "[vthost][relocate]")
{
    // A daemon a service manager starts re-executes from its own image and never runs a launcher, so
    // its lease is the only prune it gets: without it every release's copy stayed behind.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile("/state/bin/0.8.0-aaaaaaaa/Contour.AppImage", "OLD");
    std::ignore = files.writeFile("/state/bin/0.8.0-aaaaaaaa/.complete", "");
    std::ignore = files.writeFile("/state/bin/0.8.1-01234567/Contour.AppImage", "ELF");
    std::ignore = files.writeFile("/state/bin/0.8.1-01234567/.complete", "");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    // The store's own release is another one, so the name alone would not spare the running image.
    auto store =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.9.0"), files, leases, links };
    auto const image = vthost::StagingInputs { .executable = "/tmp/.mount_y/usr/bin/contour",
                                               .appImage = "/state/bin/0.8.1-01234567/Contour.AppImage" };
    auto const leaseFile = fs::path { "/state/bin/0.8.1-01234567.lease" };

    SECTION("spawned by a launcher")
    {
        auto const lease = vthost::leaseRunningImage(store, leases, image);
        REQUIRE(lease.has_value());
        CHECK(leases.holds(leaseFile) == 1);
    }
    SECTION("re-executed onto it, as a service-started daemon is")
    {
        leases.inherit(leaseFile);
        auto const lease = vthost::leaseRunningImage(store, leases, image);
        REQUIRE(lease.has_value());
        CHECK(leases.adoptions == 1);
    }
    CHECK_FALSE(files.exists("/state/bin/0.8.0-aaaaaaaa"));
    CHECK(files.exists("/state/bin/0.8.1-01234567/Contour.AppImage"));
    CHECK(files.exists("/state/bin/0.8.1-01234567/.complete"));
}

TEST_CASE("ensureDaemon asks for a binary only when it must spawn", "[vthost][daemon][afunix]")
{
    auto const temp = core::testing::ScopedTempDir { "cd" };
    auto const controlSocket = temp.path() / "s" / "control";
    auto const source = core::net::makeDefaultBackend();
    auto loop = core::net::EventLoop { *source };
    auto listener = core::net::listenUnix(loop, core::pathToUtf8(vthost::nativeSocketPath(controlSocket)));
    if (!listener.has_value())
    {
        INFO("listenUnix failed: " << listener.error().toString());
        REQUIRE(listener.error().code == core::net::NetErrorCode::Unsupported);
        SKIP("AF_UNIX not supported on this platform");
    }

    auto binary = CountingBinarySource {};
    auto placement = vtpty::NoPlacement {};
    CHECK(
        vthost::ensureDaemon(vthost::AttachEndpoint { vthost::UnixEndpoint { .socketPath = controlSocket } },
                             binary,
                             placement,
                             {},
                             std::chrono::seconds { 0 })
        == EXIT_SUCCESS);
    CHECK(binary.calls == 0);
}

TEST_CASE("ensureDaemon spawns the image its binary source hands out", "[vthost][daemon]")
{
    auto binary = CountingBinarySource {};
    auto spawner = FakeSpawner {};
    auto placement = vtpty::NoPlacement {};
    std::ignore =
        vthost::ensureDaemon(unboundEndpoint(), binary, spawner, placement, {}, std::chrono::seconds { 0 });
    CHECK(binary.calls == 1);
    REQUIRE(spawner.spawned.size() == 1);
    CHECK(spawner.spawned.front().front() == "contour-no-such-binary");
}

TEST_CASE("ensureDaemon keeps the image leased until the daemon serves", "[vthost][daemon]")
{
    // Review Focus 4: between this launcher staging its image and the daemon it spawned taking its own
    // lease, a newer launcher's prune must find the image held.
    auto files = StagingFileSystem {};
    std::ignore = files.writeFile(AppImagePath, "ELF");
    auto leases = FakeImageLeases {};
    auto const links = FakeSymbolicLinks {};
    auto older =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.1"), files, leases, links };
    auto newer =
        vthost::DaemonImageStore { appImageStoreConfig("/state/bin", "0.8.2"), files, leases, links };
    auto binary = vthost::StagedDaemonBinary { older, AppImagePath };
    auto spawner = FakeSpawner {};
    auto placement = vtpty::NoPlacement {};

    auto spawnedImage = [&] {
        REQUIRE(spawner.spawned.size() == 1);
        return fs::path { spawner.spawned.front().front() };
    };
    auto const imageSurvives = [&] {
        return files.exists(older.imageDirectoryOf(spawnedImage()) / ".complete");
    };

    SECTION("a daemon that comes up")
    {
        auto prunedWhileStarting = std::optional<std::size_t> {};
        spawner.answer = [&] {
            if (spawner.spawned.empty())
                return DaemonPresence::Absent; // the probe before the spawn: no daemon yet
            if (!prunedWhileStarting)
            {
                prunedWhileStarting = newer.prune();
                return DaemonPresence::Absent; // not bound yet
            }
            return DaemonPresence::Serving;
        };
        CHECK(vthost::ensureDaemon(
                  unboundEndpoint(), binary, spawner, placement, {}, std::chrono::seconds { 5 })
              == EXIT_SUCCESS);
        CHECK(older.contains(spawnedImage()));
        CHECK(prunedWhileStarting == 0);
        CHECK(imageSurvives());
        CHECK(leases.heldCount() == 0); // released once the daemon serves: it holds its own by then
        CHECK(newer.prune() == 1);
    }
    SECTION("a daemon that never binds")
    {
        CHECK(vthost::ensureDaemon(
                  unboundEndpoint(), binary, spawner, placement, {}, std::chrono::seconds { 0 })
              == EXIT_FAILURE);
        CHECK(older.contains(spawnedImage()));
        CHECK(leases.heldCount() == 0);
    }
    SECTION("a spawn that fails")
    {
        spawner.spawnError = "no such file";
        CHECK(vthost::ensureDaemon(
                  unboundEndpoint(), binary, spawner, placement, {}, std::chrono::seconds { 5 })
              == EXIT_FAILURE);
        CHECK(leases.heldCount() == 0);
        CHECK(spawner.probes == 1); // no readiness poll after a failed spawn
    }
}

TEST_CASE("this process names its own executable", "[vthost][relocate]")
{
    auto const executable = vthost::currentExecutablePath();
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    REQUIRE(executable.has_value());
    CHECK(executable->is_absolute());
    CHECK(executable->filename().string().starts_with("vthost_test"));
#else
    CHECK((executable.has_value() || executable.error() == std::errc::not_supported));
#endif
}

#ifndef _WIN32
namespace
{
/// The descriptors inheritableDescriptorsOf() asks about: more than a test ever has open.
constexpr auto DescriptorsScanned = 1024;

/// @return How many of this process's descriptors are @p file and would be inherited across an exec.
[[nodiscard]] std::size_t inheritableDescriptorsOf(fs::path const& file)
{
    struct stat named {};
    REQUIRE(::stat(file.c_str(), &named) == 0);
    auto count = std::size_t { 0 };
    // Every descriptor asked in turn, not /dev/fd listed: FreeBSD's /dev/fd shows 0 to 2 only, unless
    // fdescfs is mounted on it.
    for (auto const fd: std::views::iota(0, DescriptorsScanned))
    {
        struct stat opened {};
        auto const flags = ::fcntl(fd, F_GETFD);
        if (flags != -1 && (flags & FD_CLOEXEC) == 0 && ::fstat(fd, &opened) == 0
            && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino)
            ++count;
    }
    return count;
}
} // namespace

TEST_CASE("a native lease is inherited across exec only once handed over", "[vthost][relocate][native-lease]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-lease" };
    auto const leaseFile = temp.path() / "img.lease";
    auto const leases = vthost::makeNativeImageLeases();
    auto lease = leases->hold(leaseFile);
    REQUIRE(lease.has_value());
    CHECK(inheritableDescriptorsOf(leaseFile) == 0); // no shell, opener or helper inherits it
    CHECK_FALSE((*lease)->keepAcrossExec());
    CHECK(inheritableDescriptorsOf(leaseFile) == 1);
}

TEST_CASE("a native lease handed across exec is adopted and stops being inherited",
          "[vthost][relocate][native-lease]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-lease" };
    auto const leaseFile = temp.path() / "img.lease";
    auto const leases = vthost::makeNativeImageLeases();
    CHECK(leases->adoptInherited(leaseFile) == nullptr); // there is nothing to adopt

    // What the process image before the exec left behind: an inheritable descriptor, locked shared.
    auto const inherited = ::open(leaseFile.c_str(), O_RDWR | O_CREAT, 0600);
    REQUIRE(inherited != -1);
    REQUIRE(::flock(inherited, LOCK_SH) == 0);
    REQUIRE(inheritableDescriptorsOf(leaseFile) == 1);

    auto adopted = leases->adoptInherited(leaseFile);
    REQUIRE(adopted != nullptr);
    CHECK(inheritableDescriptorsOf(leaseFile) == 0);
    CHECK(leases->probe(leaseFile) == vthost::LeaseState::Held);
    adopted.reset(); // and with it the descriptor
    CHECK(leases->probe(leaseFile) == vthost::LeaseState::Free);
}
#endif

TEST_CASE("a launcher without a state directory of its own spawns its installed binary", "[vthost][relocate]")
{
    // No state directory but the shared temp one, which nothing runs from: whatever this process is,
    // nothing is staged, and an AppImage is started as the AppImage, never from its mount.
    auto const files = StagingFileSystem {};
    auto const env = core::testing::FakeEnvironment { { { "APPIMAGE", AppImagePath } } };
    auto const plain =
        vthost::makeOwnDaemonBinary(core::testing::FakeEnvironment {}, files, "0.8.1", "contour");
    CHECK(plain->imageForSpawn().executable == fs::path { "contour" });
    auto const appImage = vthost::makeOwnDaemonBinary(env, files, "0.8.1", "/tmp/.mount_x/AppRun");
    auto const image = appImage->imageForSpawn();
    CHECK(image.executable == fs::path { AppImagePath });
    CHECK(image.lease == nullptr);
}
