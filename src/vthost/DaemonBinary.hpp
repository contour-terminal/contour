// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which executable a daemon is started from, and the re-execution that moves a daemon onto its
/// version-specific image (@see DaemonImage.hpp) before it serves.

#include <vtpty/SandboxInfo.hpp>

#include <core/Environment.hpp>
#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/DaemonImage.hpp>
#include <vthost/StatePath.hpp>

namespace vthost
{

/// What a launcher starts a daemon from.
class DaemonBinarySource
{
  public:
    virtual ~DaemonBinarySource() = default;

    /// Called only once a daemon must be spawned, because staging may copy a whole install.
    /// @return The executable to spawn, and the lease that keeps it from being pruned until the
    ///         spawned daemon holds its own; whoever spawns keeps the StagedImage alive until then.
    [[nodiscard]] virtual StagedImage imageForSpawn() = 0;
};

/// The installed binary, as it is: nothing staged, nothing leased.
class InstalledDaemonBinary final: public DaemonBinarySource
{
  public:
    /// @param executable The binary, as the caller spells it (a bare name is looked up on PATH).
    explicit InstalledDaemonBinary(std::filesystem::path executable): _executable { std::move(executable) } {}

    [[nodiscard]] StagedImage imageForSpawn() override
    {
        return StagedImage { .executable = _executable, .lease = nullptr };
    }

  private:
    std::filesystem::path _executable;
};

/// This version's staged image, staged when first asked for.
///
/// Each call stages (or finds staged) the image, then prunes the images nobody leases; the prune runs
/// while the new image's lease is already held. A layout that runs in place, or a staging failure,
/// hands out the installed binary with no lease; a failure is logged.
class StagedDaemonBinary final: public DaemonBinarySource
{
  public:
    /// @param store The store to stage into and prune (not owned; outlives this).
    /// @param installed What to start when nothing is staged: the binary as the caller spells it, or
    ///        the AppImage it runs from.
    StagedDaemonBinary(DaemonImageStore& store, std::filesystem::path installed);

    [[nodiscard]] StagedImage imageForSpawn() override;

  private:
    DaemonImageStore& _store;
    std::filesystem::path _installed;
};

/// Whether a daemon about to serve moves onto its staged image first.
enum class Relocation : uint8_t
{
    RunInPlace = 0,
    ExecStaged, ///< Replaces the process image with the staged copy, keeping the process id.
};

/// Where a daemon's executable lies.
enum class ImagePlacement : uint8_t
{
    Installed = 0, ///< Where it was installed (or built).
    Staged,        ///< In a version-specific image of the store.
};

/// Whether the user lets a daemon move onto its staged image (`contour daemon --in-place` refuses).
enum class RelocationRequest : uint8_t
{
    Allowed = 0,
    Refused,
};

/// Decides whether a daemon re-executes from its staged image before it serves.
///
/// Only an installed daemon with something to stage moves, and never on Windows, which cannot replace
/// a process image: there a daemon is moved by being spawned from its image in the first place.
/// @param layout What an image of the executable holds (DaemonImageStore::layout()).
/// @param platform The platform the daemon runs on.
/// @param placement Where the daemon's executable lies.
/// @param request Whether the user allows the move.
/// @return ExecStaged iff @p layout is not InPlace, @p platform not Windows, @p placement Installed and
///         @p request Allowed.
[[nodiscard]] constexpr Relocation relocationFor(StagingLayout layout,
                                                 HostPlatform platform,
                                                 ImagePlacement placement,
                                                 RelocationRequest request) noexcept
{
    auto const moves = layout != StagingLayout::InPlace && platform != HostPlatform::Windows
                       && placement == ImagePlacement::Installed && request == RelocationRequest::Allowed;
    return moves ? Relocation::ExecStaged : Relocation::RunInPlace;
}

/// @param inputs What staging knows of an executable.
/// @return The file it was launched from: the AppImage, when it runs from one, else the executable.
[[nodiscard]] std::filesystem::path launchedImage(StagingInputs const& inputs);

/// @param store The store this process's images live in.
/// @param inputs What staging knows of this process's executable.
/// @return The one of its AppImage and its executable that lies in @p store, if either does. Both are
///         asked, so a process re-executed from its image is recognized whatever `$APPIMAGE` it kept.
[[nodiscard]] std::optional<std::filesystem::path> stagedImageOf(DaemonImageStore const& store,
                                                                 StagingInputs const& inputs);

/// @param store The store this process's images live in.
/// @param inputs What staging knows of this process's executable.
/// @return Staged when stagedImageOf() finds it in @p store.
[[nodiscard]] ImagePlacement imagePlacementOf(DaemonImageStore const& store, StagingInputs const& inputs);

/// What this process is, as its image store asks it.
struct OwnProcess
{
    /// The running executable, absolute (currentExecutablePath()).
    std::filesystem::path executable {};

    /// The application sandbox it runs in (vtpty::currentSandbox()).
    vtpty::SandboxState sandbox = vtpty::SandboxState::Host;

    /// The release it reports, which names its image.
    std::string releaseVersion {};

    /// Its process id, which names its partial copy.
    std::uint64_t processId = 0;
};

/// The configuration of the store this process stages its own executable into.
/// @param env Where `$APPIMAGE` and the state directory's variables are read from.
/// @param process What this process is.
/// @return Images under `daemonStateDirectory(env) / "bin"`, with the directory's origin, so that a
///         temporary fallback stages nothing.
[[nodiscard]] DaemonImageStoreConfig ownImageStoreConfig(core::Environment const& env, OwnProcess process);

/// Replaces this process's image with another program, keeping its process id.
///
/// An interface because a successful call never returns: re-execution is tested over a fake.
class ProcessImageReplacer
{
  public:
    virtual ~ProcessImageReplacer() = default;

    /// @param image The program to run.
    /// @param argv Its argument vector, the name it runs under first.
    /// @return Why it could not be replaced; on success this does not return.
    [[nodiscard]] virtual std::error_code replaceWith(std::filesystem::path const& image,
                                                      std::span<std::string const> argv) = 0;
};

/// @return The operating system's replacer: `execv` on POSIX; on Windows one that fails with
///         `errc::not_supported`.
[[nodiscard]] std::unique_ptr<ProcessImageReplacer> makeNativeProcessImageReplacer();

/// Stages this executable and re-executes this process from the staged copy.
///
/// The image's lease is made to survive the exec right before it, so the image stays leased from
/// staging through the re-executed daemon's whole life (@see leaseRunningImage). Returns only on
/// failure, having dropped that lease: the caller then runs in place.
///
/// Refused when this process already is such a re-execution, which the lease it inherited for the very
/// image it would move onto tells: a process that still reads as installed after moving (one whose
/// executable and `$APPIMAGE` both lie outside the store) would otherwise replace itself forever.
/// The handed-over lease is the marker rather than an environment variable, because it needs no
/// environment written and dies with the process that holds it.
/// @param store The store to stage into.
/// @param leases The leases that guard it, asked for a lease this process inherited.
/// @param replacer What replaces the process image.
/// @param argv This process's command line; argv[0] is replaced by the staged executable.
/// @return Why the re-execution did not happen.
[[nodiscard]] std::string execFromStagedImage(DaemonImageStore& store,
                                              ImageLeases& leases,
                                              ProcessImageReplacer& replacer,
                                              std::vector<std::string> argv);

/// Leases the staged image a daemon runs from, for as long as the daemon runs, then prunes every image
/// of @p store nobody leases.
///
/// Taken before the daemon binds its listener, so "serving" implies "leased", and a launcher's lease
/// can end once the daemon serves. A lease handed across a re-execution is adopted rather than doubled.
/// The prune is what keeps a daemon started by a service manager from leaving every release's copy
/// behind: no launcher, and so no StagedDaemonBinary, ever runs for it.
/// @param store The store the image lies in, and the one pruned.
/// @param leases The leases that guard it.
/// @param inputs What staging knows of the daemon's executable.
/// @return The held lease, or why it could not be taken: LeaseFailed, also when the daemon runs from
///         no image of @p store.
[[nodiscard]] std::expected<std::unique_ptr<ImageLease>, StagingError> leaseRunningImage(
    DaemonImageStore& store, ImageLeases& leases, StagingInputs const& inputs);

/// @return The running executable's absolute path (`/proc/self/exe`, `_NSGetExecutablePath`,
///         `GetModuleFileNameW`); `errc::not_supported` elsewhere.
[[nodiscard]] std::expected<std::filesystem::path, std::error_code> currentExecutablePath();

/// @return This process's id.
[[nodiscard]] std::uint64_t currentProcessId() noexcept;

/// This process's own image store, with the operating system's leases and links beneath it.
class NativeDaemonImages
{
  public:
    /// @param config What to stage, and where.
    /// @param fs The file system the images live on (not owned; outlives this).
    NativeDaemonImages(DaemonImageStoreConfig config, core::platform::FileSystem const& fs);

    NativeDaemonImages(NativeDaemonImages const&) = delete;
    NativeDaemonImages& operator=(NativeDaemonImages const&) = delete;
    NativeDaemonImages(NativeDaemonImages&&) = delete;
    NativeDaemonImages& operator=(NativeDaemonImages&&) = delete;
    ~NativeDaemonImages() = default;

    /// The store of the running executable, by ownImageStoreConfig().
    ///
    /// Paths are made canonical first, so that an executable the operating system names by its real
    /// path is found under an images root spelled through a symbolic link.
    /// @param env Where `$APPIMAGE` and the state directory are read from.
    /// @param fs The file system the images live on (not owned; outlives the result).
    /// @param releaseVersion The release this executable reports.
    /// @return The store, or why the running executable is unknown.
    [[nodiscard]] static std::expected<std::unique_ptr<NativeDaemonImages>, std::error_code> ofThisProcess(
        core::Environment const& env, core::platform::FileSystem const& fs, std::string releaseVersion);

    /// @return What staging knows of the executable.
    [[nodiscard]] StagingInputs const& inputs() const noexcept { return _inputs; }

    [[nodiscard]] DaemonImageStore& store() noexcept { return _store; }
    [[nodiscard]] ImageLeases& leases() noexcept { return *_leases; }

  private:
    StagingInputs _inputs;
    std::unique_ptr<ImageLeases> _leases;
    std::unique_ptr<SymbolicLinks> _links;
    DaemonImageStore _store;
};

/// The binary a launcher spawns its daemon from: this version's staged image of the running
/// executable (a StagedDaemonBinary over NativeDaemonImages::ofThisProcess()).
///
/// What is started when nothing is staged is the AppImage this process runs from, if it does --
/// its own executable lies in a mount that ends with this process -- else @p argv0.
/// @param env Where `$APPIMAGE` and the state directory are read from.
/// @param fs The file system the images live on (not owned; outlives the result).
/// @param releaseVersion The release this executable reports.
/// @param argv0 The binary as this process was started (a bare name is looked up on PATH).
/// @return The source; one handing out the installed binary when the running executable is unknown.
[[nodiscard]] std::unique_ptr<DaemonBinarySource> makeOwnDaemonBinary(core::Environment const& env,
                                                                      core::platform::FileSystem const& fs,
                                                                      std::string releaseVersion,
                                                                      std::filesystem::path argv0);

} // namespace vthost
