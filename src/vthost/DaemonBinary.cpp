// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonBinary.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace vthost
{

namespace fs = std::filesystem;

namespace
{
    /// A StagedDaemonBinary together with the store it stages into.
    class OwnDaemonBinary final: public DaemonBinarySource
    {
      public:
        OwnDaemonBinary(std::unique_ptr<NativeDaemonImages> images, fs::path installed):
            _images { std::move(images) }, _staged { _images->store(), std::move(installed) }
        {
        }

        [[nodiscard]] StagedImage imageForSpawn() override { return _staged.imageForSpawn(); }

      private:
        std::unique_ptr<NativeDaemonImages> _images;
        StagedDaemonBinary _staged;
    };

    /// @return The AppImage this process runs from, as `$APPIMAGE` names it; nullopt when it names none.
    [[nodiscard]] std::optional<fs::path> appImageOf(core::Environment const& env)
    {
        return env.get("APPIMAGE").and_then([](std::string const& value) -> std::optional<fs::path> {
            if (value.empty())
                return std::nullopt;
            return core::pathFromUtf8(value).value_or(fs::path {});
        });
    }
} // namespace

StagedDaemonBinary::StagedDaemonBinary(DaemonImageStore& store, fs::path installed):
    _store { store }, _installed { std::move(installed) }
{
}

StagedImage StagedDaemonBinary::imageForSpawn()
{
    if (_store.layout() == StagingLayout::InPlace)
        return StagedImage { .executable = _installed, .lease = nullptr };
    auto staged = _store.stage();
    if (!staged)
    {
        errorLog()("cannot stage the daemon's image ({}); starting {} instead",
                   staged.error().toString(),
                   core::pathToUtf8(_installed));
        return StagedImage { .executable = _installed, .lease = nullptr };
    }
    // Under the new image's lease, so a prune racing this one, from a launcher of any release, finds
    // it held.
    std::ignore = _store.prune();
    return std::move(*staged);
}

fs::path launchedImage(StagingInputs const& inputs)
{
    return inputs.appImage.value_or(inputs.executable);
}

std::optional<fs::path> stagedImageOf(DaemonImageStore const& store, StagingInputs const& inputs)
{
    if (inputs.appImage && store.contains(*inputs.appImage))
        return inputs.appImage;
    if (store.contains(inputs.executable))
        return inputs.executable;
    return std::nullopt;
}

ImagePlacement imagePlacementOf(DaemonImageStore const& store, StagingInputs const& inputs)
{
    return stagedImageOf(store, inputs) ? ImagePlacement::Staged : ImagePlacement::Installed;
}

DaemonImageStoreConfig ownImageStoreConfig(core::Environment const& env, OwnProcess process)
{
    auto appImage = appImageOf(env);
    return DaemonImageStoreConfig { .imagesRoot = daemonStateDirectory(env) / "bin",
                                    .releaseVersion = std::move(process.releaseVersion),
                                    .inputs = { .platform = NativeHostPlatform,
                                                .executable = std::move(process.executable),
                                                .appImage = std::move(appImage),
                                                .sandbox = process.sandbox },
                                    .processId = process.processId,
                                    .stateOrigin = stateDirectoryOrigin(env) };
}

std::string execFromStagedImage(DaemonImageStore& store,
                                ImageLeases& leases,
                                ProcessImageReplacer& replacer,
                                std::vector<std::string> argv)
{
    auto staged = store.stage();
    if (!staged)
        return staged.error().toString();
    if (!staged->lease)
        return "nothing is staged for this install";
    // Adopted, and dropped with this frame: the move it marks has happened, and this process runs in
    // place, holding none of it.
    if (leases.adoptInherited(store.leaseFileOf(store.imageDirectoryOf(staged->executable))))
        return std::format("this process already was re-executed from {}, and still reads as installed",
                           core::pathToUtf8(staged->executable));
    if (auto const handedOver = staged->lease->keepAcrossExec(); handedOver)
        return std::format("cannot hand the image's lease over: {}", handedOver.message());
    if (argv.empty())
        argv.emplace_back();
    argv.front() = core::pathToUtf8(staged->executable);
    auto const failed = replacer.replaceWith(staged->executable, argv);
    return std::format("cannot run {}: {}", core::pathToUtf8(staged->executable), failed.message());
}

std::expected<std::unique_ptr<ImageLease>, StagingError> leaseRunningImage(DaemonImageStore& store,
                                                                           ImageLeases& leases,
                                                                           StagingInputs const& inputs)
{
    auto const image = stagedImageOf(store, inputs);
    if (!image)
        return std::unexpected(StagingError {
            .code = StagingErrorCode::LeaseFailed,
            .context = std::format("{} runs from no staged image", core::pathToUtf8(inputs.executable)) });
    auto const leaseFile = store.leaseFileOf(store.imageDirectoryOf(*image));
    auto lease = [&]() -> std::expected<std::unique_ptr<ImageLease>, StagingError> {
        if (auto inherited = leases.adoptInherited(leaseFile))
            return inherited;
        return leases.hold(leaseFile);
    }();
    // Under the lease just taken, so this image is safe. A daemon a service manager starts runs no
    // launcher before it, and none after while the service exists, so this is the only prune it gets.
    if (lease)
        std::ignore = store.prune();
    return lease;
}

NativeDaemonImages::NativeDaemonImages(DaemonImageStoreConfig config, core::platform::FileSystem const& fs):
    _inputs { config.inputs },
    _leases { makeNativeImageLeases() },
    _links { makeNativeSymbolicLinks() },
    _store { std::move(config), fs, *_leases, *_links }
{
}

std::expected<std::unique_ptr<NativeDaemonImages>, std::error_code> NativeDaemonImages::ofThisProcess(
    core::Environment const& env, core::platform::FileSystem const& fs, std::string releaseVersion)
{
    return currentExecutablePath().transform([&](fs::path executable) {
        auto config = ownImageStoreConfig(env,
                                          OwnProcess { .executable = std::move(executable),
                                                       .sandbox = vtpty::currentSandbox().state,
                                                       .releaseVersion = std::move(releaseVersion),
                                                       .processId = currentProcessId() });
        config.imagesRoot = fs.weaklyCanonical(config.imagesRoot);
        config.inputs.appImage = config.inputs.appImage.transform(
            [&](fs::path const& appImage) { return fs.weaklyCanonical(appImage); });
        return std::make_unique<NativeDaemonImages>(std::move(config), fs);
    });
}

std::unique_ptr<DaemonBinarySource> makeOwnDaemonBinary(core::Environment const& env,
                                                        core::platform::FileSystem const& fs,
                                                        std::string releaseVersion,
                                                        fs::path argv0)
{
    auto images = NativeDaemonImages::ofThisProcess(env, fs, std::move(releaseVersion));
    if (!images)
    {
        errorLog()("cannot locate the running executable ({}); the daemon runs from {}",
                   images.error().message(),
                   core::pathToUtf8(argv0));
        // An AppImage is still started as the AppImage, never from its mount, which goes with this process.
        return std::make_unique<InstalledDaemonBinary>(appImageOf(env).value_or(std::move(argv0)));
    }
    auto installed = (*images)->inputs().appImage.value_or(std::move(argv0));
    return std::make_unique<OwnDaemonBinary>(std::move(*images), std::move(installed));
}

} // namespace vthost
