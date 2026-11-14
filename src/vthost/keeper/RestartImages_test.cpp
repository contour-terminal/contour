// SPDX-License-Identifier: Apache-2.0
#include <core/platform/NativeFileSystem.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <vthost/DaemonImage.hpp>
#include <vthost/keeper/RestartImages.hpp>

namespace fs = std::filesystem;

namespace
{
/// An AppImage's image store on the real file system, as the given release stages it.
struct Store
{
    Store(fs::path const& root, fs::path const& appImage, std::string release):
        store { vthost::DaemonImageStoreConfig { .imagesRoot = root / "bin",
                                                 .releaseVersion = std::move(release),
                                                 .inputs = { .platform = vthost::HostPlatform::Linux,
                                                             .executable = "/tmp/.mount_x/usr/bin/contour",
                                                             .appImage = appImage },
                                                 .processId = 1 },
                files,
                *leases,
                *links }
    {
    }

    core::platform::NativeFileSystem files;
    std::unique_ptr<vthost::ImageLeases> leases = vthost::makeNativeImageLeases();
    std::unique_ptr<vthost::SymbolicLinks> links = vthost::makeNativeSymbolicLinks();
    vthost::DaemonImageStore store;
};
} // namespace

TEST_CASE("the restart image is not pruned while the keeper holds it", "[keeper][image]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-restart-image" };
    auto const appImage = temp.path() / "Contour.AppImage";
    std::ofstream { appImage } << "ELF";

    // The keeper's own release, staged; it holds that lease itself for as long as it keeps.
    auto keeperStore = Store { temp.path(), appImage, "0.8.1" };
    auto committed = std::optional { keeperStore.store.stage() };
    REQUIRE(committed->has_value());
    auto const committedExecutable = (*committed)->executable;

    auto images =
        std::optional<vthost::keeper::StoreRestartImages> { std::in_place, keeperStore.store, fs::path {} };
    CHECK(images->pick(committedExecutable) == committedExecutable); // nothing newer yet

    // A newer release is staged by a client of it, which lets its lease go once its daemon serves.
    auto newerStore = Store { temp.path(), appImage, "0.9.0" };
    auto const newerExecutable = [&] {
        auto const staged = newerStore.store.stage();
        REQUIRE(staged.has_value());
        return staged->executable;
    }();
    auto const newerImage = newerStore.store.imageDirectoryOf(newerExecutable);

    CHECK(images->pick(committedExecutable) == newerExecutable);
    // A prune by yet another release removes what nobody holds, and not the image the keeper restarts from.
    auto pruner = Store { temp.path(), appImage, "1.0.0" };
    CHECK(pruner.store.prune() == 0);
    CHECK(fs::exists(newerExecutable));

    // Picked again, it stays leased throughout.
    CHECK(images->pick(committedExecutable) == newerExecutable);
    CHECK(pruner.store.prune() == 0);
    CHECK(fs::exists(newerExecutable));

    // Once the keeper holds neither, both go.
    committed.reset();
    images.reset();
    CHECK(pruner.store.prune() == 2);
    CHECK_FALSE(fs::exists(newerImage));
}

TEST_CASE("a pick never waits for a prune's claim on the newer image", "[keeper][image]")
{
    using namespace std::chrono_literals;
    auto const temp = core::testing::ScopedTempDir { "contour-restart-image-busy" };
    auto const appImage = temp.path() / "Contour.AppImage";
    std::ofstream { appImage } << "ELF";

    auto keeperStore = Store { temp.path(), appImage, "0.8.1" };
    auto const committed = keeperStore.store.stage();
    REQUIRE(committed.has_value());
    auto newerStore = Store { temp.path(), appImage, "0.9.0" };
    auto const newerExecutable = [&] {
        auto const staged = newerStore.store.stage();
        REQUIRE(staged.has_value());
        return staged->executable;
    }();

    // A prune holds the newer image's lease exclusively, as it does while it removes the image.
    auto pruner = Store { temp.path(), appImage, "1.0.0" };
    auto claim = std::optional { pruner.leases->claim(
        newerStore.store.leaseFileOf(newerStore.store.imageDirectoryOf(newerExecutable))) };
    REQUIRE(*claim != nullptr);

    auto images = vthost::keeper::StoreRestartImages { keeperStore.store, fs::path {} };
    auto picked = std::async(std::launch::async, [&] { return images.pick(committed->executable); });
    auto const answered = picked.wait_for(30s) == std::future_status::ready;
    claim.reset(); // lets a pick that blocked carry on, so the test ends either way
    REQUIRE(answered);
    CHECK(picked.get() == committed->executable); // busy: skipped, the committed image restarts
}

TEST_CASE("the image a daemon execs is held for its restart, and nothing else is", "[keeper][image]")
{
    auto const temp = core::testing::ScopedTempDir { "contour-exec-image" };
    auto const appImage = temp.path() / "Contour.AppImage";
    std::ofstream { appImage } << "ELF";

    auto keeperStore = Store { temp.path(), appImage, "0.8.1" };
    auto newerStore = Store { temp.path(), appImage, "0.9.0" };
    auto const newerExecutable = [&] {
        auto const staged = newerStore.store.stage();
        REQUIRE(staged.has_value());
        return staged->executable;
    }();
    auto images = vthost::keeper::StoreRestartImages { keeperStore.store, fs::path {} };

    auto held = std::optional { images.hold(newerExecutable) };
    REQUIRE(held->has_value());
    // Held, the image survives a prune by yet another release; let go, it does not.
    auto pruner = Store { temp.path(), appImage, "1.0.0" };
    CHECK(pruner.store.prune() == 0);
    CHECK(fs::exists(newerExecutable));
    held.reset();
    CHECK(pruner.store.prune() == 1);

    CHECK(images.hold(appImage) == std::unexpected { vthost::keeper::RestartImageRefusal::NotAnImage });
    CHECK(images.hold("/bin/sh") == std::unexpected { vthost::keeper::RestartImageRefusal::NotAnImage });
}

TEST_CASE("an install that runs in place holds its installed executable alone", "[keeper][image]")
{
    auto files = core::platform::NativeFileSystem {};
    auto leases = vthost::makeNativeImageLeases();
    auto links = vthost::makeNativeSymbolicLinks();
    auto const temp = core::testing::ScopedTempDir { "contour-exec-in-place" };
    auto store = vthost::DaemonImageStore { vthost::DaemonImageStoreConfig {
                                                .imagesRoot = temp.path() / "bin",
                                                .releaseVersion = "0.8.1",
                                                .inputs = { .platform = vthost::HostPlatform::Linux,
                                                            .executable = "/usr/bin/contour",
                                                            .appImage = std::nullopt },
                                                .processId = 1 },
                                            files,
                                            *leases,
                                            *links };
    REQUIRE(store.layout() == vthost::StagingLayout::InPlace);
    auto images = vthost::keeper::StoreRestartImages { store, "/usr/bin/contour" };
    CHECK(images.hold("/usr/bin/contour").has_value());
    CHECK(images.hold("/usr/bin/../bin/contour").has_value());
    CHECK(images.hold("/tmp/contour") == std::unexpected { vthost::keeper::RestartImageRefusal::NotAnImage });
}
