// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The version-specific daemon image store: where a daemon runs from when running from the installed
/// binary would let a package upgrade pull it from under the daemon.
///
/// An image is `<imagesRoot>/<release>-<fingerprint>/`, published by renaming a complete copy into
/// place and leased through the sibling file `<imagesRoot>/<release>-<fingerprint>.lease`. Whoever
/// runs from an image, or is about to, holds its lease; prune() removes only images nobody holds.

#include <vtpty/SandboxInfo.hpp>

#include <core/platform/FileSystem.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <vthost/StatePath.hpp>

namespace vthost
{

/// What a launcher knows about the executable it would start a daemon from.
struct StagingInputs
{
    /// Whose conventions apply.
    HostPlatform platform = NativeHostPlatform;

    /// The running executable's absolute path.
    std::filesystem::path executable {};

    /// The AppImage this process was started from (`$APPIMAGE`), if it was.
    std::optional<std::filesystem::path> appImage {};

    /// The application sandbox this process runs in.
    vtpty::SandboxState sandbox = vtpty::SandboxState::Host;
};

/// What an image holds, and so what staging copies.
enum class StagingLayout : uint8_t
{
    InPlace = 0,    ///< Nothing: the daemon runs from the installed executable.
    ExecutableOnly, ///< The executable alone.
    InstallTree,    ///< The install prefix's `bin` and `share/contour` (Windows).
    AppBundle,      ///< The whole `.app` (macOS).
    AppImage,       ///< The AppImage file, which is its own install tree.
};

/// What staging does with a symbolic link inside a copied tree.
enum class SymlinkPolicy : uint8_t
{
    Follow = 0, ///< Copy what the link names, as a file or a tree of its own.
    Preserve,   ///< Recreate the link: what a macOS bundle's code signature seals.
};

/// What to copy into an image, decided from StagingInputs alone.
struct StagingPlan
{
    /// What the image holds.
    StagingLayout layout = StagingLayout::InPlace;

    /// The directory the entries are relative to. Empty for InPlace.
    std::filesystem::path sourceRoot {};

    /// The files and directories to copy, relative to sourceRoot. An entry that does not exist is
    /// skipped. Empty for InPlace.
    std::vector<std::filesystem::path> entries {};

    /// The executable, relative to sourceRoot and so to the image. Empty for InPlace.
    std::filesystem::path executable {};

    /// What a symbolic link inside a copied tree becomes.
    SymlinkPolicy symlinks = SymlinkPolicy::Follow;
};

/// Decides what an image of @p inputs holds.
///
/// One table, first match wins:
///  - a Flatpak runs in place: flatpak already deploys each version side by side and immutable;
///  - an AppImage is copied whole, because its `/tmp/.mount_*` disappears with the process that
///    mounted it;
///  - any other Linux install runs in place: a running ELF keeps its unlinked inode, and dpkg, rpm
///    and flatpak replace files by rename, so an upgrade cannot pull the binary from under it;
///  - an executable in a macOS `.app` copies the `.app`, keeping `Contents/Resources` resolvable;
///  - a Windows executable copies the install tree's `bin` and `share/contour`, keeping
///    `<exe dir>/../share/contour/extensions` resolvable, since MSI and winget replace files in place;
///  - anything else copies the executable alone.
/// @param inputs What the launcher knows about its executable.
/// @return What to copy.
[[nodiscard]] StagingPlan stagingPlan(StagingInputs const& inputs);

/// The name of an image of an executable: `<sanitized release>-<8 lowercase hex digits>`.
///
/// Sanitizing maps every byte outside `[A-Za-z0-9._-]` to `_`. The hex is FNV-1a 32 over the
/// executable's size and modification time, so two development builds of one release get distinct
/// images.
/// @param releaseVersion The release the executable reports.
/// @param executableSize The executable's size in bytes.
/// @param executableTime The executable's last modification time.
/// @return The image directory's name.
[[nodiscard]] std::string imageDirectoryName(std::string_view releaseVersion,
                                             std::uintmax_t executableSize,
                                             std::filesystem::file_time_type executableTime);

/// Why staging an image failed.
enum class StagingErrorCode : uint8_t
{
    SourceUnreadable = 0, ///< The executable to stage could not be inspected.
    CopyFailed,           ///< Copying into the partial image failed.
    PublishFailed,        ///< Renaming the partial image into place failed, and no one else published it.
    LeaseFailed,          ///< The image's lease could not be taken.
};

/// A staging failure, and what it failed on.
struct StagingError
{
    StagingErrorCode code = StagingErrorCode::SourceUnreadable;

    /// The path and the operating system's reason.
    std::string context {};

    /// @return A one-line description, for a log or the user.
    [[nodiscard]] std::string toString() const;
};

/// Whether anyone holds an image's lease.
enum class LeaseState : uint8_t
{
    Free = 0,
    Held,
};

/// A held lease on an image: it keeps the image from being pruned until destroyed.
class ImageLease
{
  public:
    virtual ~ImageLease() = default;

    /// Lets the lease outlive this process image: it is handed to the program this process is about to
    /// replace itself with (execv), which adopts it (@see ImageLeases::adoptInherited). Called only right
    /// before that replacement, because until then every program this process starts would inherit it.
    /// @return Nothing on success; `errc::not_supported` where a lease cannot cross an exec (Windows, and
    ///         a lease that is no operating-system lock).
    [[nodiscard]] virtual std::error_code keepAcrossExec()
    {
        return std::make_error_code(std::errc::not_supported);
    }
};

/// Takes and inspects image leases.
///
/// An interface, because a lease is an operating-system lock: the store is tested over
/// testing::FakeImageLeases, and makeNativeImageLeases() is the production one.
class ImageLeases
{
  public:
    virtual ~ImageLeases() = default;

    /// Takes a shared lease, waiting while a claim() holds it, and creating the lease file if absent.
    ///
    /// The wait lasts as long as the claim does, which is as long as one prune takes to remove one
    /// image. POSIX blocks in flock() for it; Windows polls, and gives up with LeaseFailed after 30 s,
    /// a bound only a hung claimer reaches.
    /// @param leaseFile The lease file, a sibling of the image it leases.
    /// @return The held lease, or LeaseFailed.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ImageLease>, StagingError> hold(
        std::filesystem::path const& leaseFile) = 0;

    /// Takes the lease exclusively, without waiting: what lets a prune remove an image knowing nobody
    /// holds it, or can come to hold it, until the removal is done. While it is claimed, a hold()
    /// waits, and one that finds the lease file gone or replaced once it gets the lease takes it
    /// afresh.
    /// @param leaseFile The lease file.
    /// @return The claim, or null when the lease is held, or the claim could not be taken.
    [[nodiscard]] virtual std::unique_ptr<ImageLease> claim(std::filesystem::path const& leaseFile) = 0;

    /// @param leaseFile The lease file.
    /// @return Whether anyone holds the lease. A lease file that does not exist is Free.
    [[nodiscard]] virtual LeaseState probe(std::filesystem::path const& leaseFile) = 0;

    /// Takes over a lease on @p leaseFile that the process image before an exec handed over
    /// (@see ImageLease::keepAcrossExec), and stops it being inherited any further.
    /// @param leaseFile The lease file.
    /// @return The adopted lease, or null when this process inherited none.
    [[nodiscard]] virtual std::unique_ptr<ImageLease> adoptInherited(
        std::filesystem::path const& leaseFile) = 0;
};

/// Recreates symbolic links: the one thing staging needs that core::platform::FileSystem cannot do.
///
/// An interface for the reason FileSystem is one, so the store is tested without touching the disk.
class SymbolicLinks
{
  public:
    virtual ~SymbolicLinks() = default;

    /// Creates at @p to a link naming what the link @p link names, as `std::filesystem::copy_symlink`.
    /// @param link An existing symbolic link.
    /// @param to Where to create the copy; its parent exists.
    /// @return Nothing, or why the link could not be read or created.
    [[nodiscard]] virtual std::expected<void, std::string> copy(std::filesystem::path const& link,
                                                                std::filesystem::path const& to) const = 0;
};

/// @return The operating system's symbolic links, through std::filesystem.
[[nodiscard]] std::unique_ptr<SymbolicLinks> makeNativeSymbolicLinks();

/// The operating system's leases.
///
/// POSIX: hold() is `flock(LOCK_SH)` on the lease file, held open with `O_CLOEXEC` (a lease handed
/// across `execv` clears that flag right before it, and the re-executed process sets it again);
/// claim() is `flock(LOCK_EX | LOCK_NB)`, and unlinking the lease file under it is what tells a
/// waiting hold() to start again. Windows: hold() opens the lease file sharing read and write but not
/// delete; claim() opens it sharing nothing, with delete access, and deletes it on close.
/// @return The production leases.
[[nodiscard]] std::unique_ptr<ImageLeases> makeNativeImageLeases();

/// A staged image, ready to start a daemon from.
struct StagedImage
{
    /// The executable to start, absolute.
    std::filesystem::path executable;

    /// The image's lease, held: whoever holds a StagedImage keeps the image from being pruned. Null
    /// for an InPlace layout, which has no image.
    std::unique_ptr<ImageLease> lease;
};

/// What a DaemonImageStore stages, and where.
struct DaemonImageStoreConfig
{
    /// Where images live: `daemonStateDirectory() / "bin"`.
    std::filesystem::path imagesRoot {};

    /// The release this executable reports; it names the image.
    std::string releaseVersion {};

    /// The executable to stage.
    StagingInputs inputs {};

    /// This process's id, which names its partial copy.
    std::uint64_t processId = 0;

    /// Where the state directory holding imagesRoot came from (stateDirectoryOrigin()). A
    /// TemporaryFallback stages nothing and prunes nothing: every layout runs in place, because other
    /// local users can pre-create that directory, and a daemon started from it would run their binary.
    StateDirectoryOrigin stateOrigin = StateDirectoryOrigin::Configured;
};

/// Stages the running executable into a version-specific image, and prunes the images nobody leases.
///
/// Safe against concurrent launchers in other processes, of this release or any other: an image is
/// handed out only once complete, and only under a held lease, and a prune removes only what it has
/// claimed.
class DaemonImageStore
{
  public:
    /// @param config What to stage, and where.
    /// @param fs The file system the images live on.
    /// @param leases The leases that guard them.
    /// @param links What recreates a symbolic link where the plan preserves them.
    DaemonImageStore(DaemonImageStoreConfig config,
                     core::platform::FileSystem const& fs,
                     ImageLeases& leases,
                     SymbolicLinks const& links);

    /// Stages the executable, or finds it already staged.
    ///
    /// The lease is taken first, before anything is looked at: a prune that ran before it has already
    /// removed the image, and the check that follows sees it gone. A complete image (one holding
    /// `.complete`) is returned as it is. Otherwise the plan's entries are copied into
    /// `<imagesRoot>/<name>.partial-<processId>`, `.complete` is written last, and the copy is renamed
    /// into place; when the rename fails because another launcher published first, its image is used.
    /// An incomplete directory in the way (left by something other than this store, which publishes
    /// only by rename) is moved aside and the rename tried once more; a complete one is never removed.
    /// @return The executable to start and its held lease; for InPlace the executable itself, with no
    ///         lease and nothing touched.
    [[nodiscard]] std::expected<StagedImage, StagingError> stage();

    /// Removes every image, with its partial copies and its lease file, that nobody leases — except
    /// this executable's own.
    /// @return How many directories were removed.
    [[nodiscard]] std::size_t prune();

    /// @param executable An absolute path.
    /// @return Whether @p executable lies under the images root.
    [[nodiscard]] bool contains(std::filesystem::path const& executable) const;

    /// @param executable An executable under the images root.
    /// @return The image directory holding it, or empty when it lies elsewhere.
    [[nodiscard]] std::filesystem::path imageDirectoryOf(std::filesystem::path const& executable) const;

    /// @param imageDir An image directory.
    /// @return Its lease file, `<imageDir>.lease`.
    [[nodiscard]] std::filesystem::path leaseFileOf(std::filesystem::path const& imageDir) const;

    /// @return What an image of this executable holds; InPlace when nothing is staged, a temporary
    ///         fallback state directory included.
    [[nodiscard]] StagingLayout layout() const noexcept { return _plan.layout; }

  private:
    /// @return This executable's image directory, or why its executable could not be inspected.
    [[nodiscard]] std::expected<std::filesystem::path, StagingError> currentTarget() const;

    /// Copies the plan's entries into @p partial and marks it complete.
    [[nodiscard]] std::expected<void, StagingError> copyInto(std::filesystem::path const& partial) const;

    /// Copies the directory @p from to @p to, recursively.
    [[nodiscard]] std::expected<void, StagingError> copyTree(std::filesystem::path const& from,
                                                             std::filesystem::path const& to) const;

    /// @return The staged image of @p target, holding @p lease.
    [[nodiscard]] StagedImage imageAt(std::filesystem::path const& target,
                                      std::unique_ptr<ImageLease> lease) const;

    /// Moves an incomplete @p target out of the way of a publish, never removing a complete one.
    void clearIncompleteTarget(std::filesystem::path const& target) const;

    /// @return Whether @p imageDir holds a complete image.
    [[nodiscard]] bool isComplete(std::filesystem::path const& imageDir) const;

    DaemonImageStoreConfig _config;
    StagingPlan _plan;
    core::platform::FileSystem const& _fs;
    ImageLeases& _leases;
    SymbolicLinks const& _links;
};

} // namespace vthost
