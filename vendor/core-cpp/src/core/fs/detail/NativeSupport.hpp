// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the native descriptors of every operating system share: the configuration a file system
/// fixes at construction, the descriptor base that offloads and recognises its siblings, the keyed
/// metadata hash, and the walk's rules.

#include <core/Random.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/Offload.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/PathPolicy.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/platform/Clock.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace core::fs::detail
{

class INtFiles;         // windows/WindowsPathWalk.hpp
class OwnerSecurity;    // windows/OwnerSecurity.hpp
class OwnerOnlyCreator; // posix/OwnerOnlyCreation.hpp
class IMountBoundaries; // posix/MountBoundaries.hpp

/// @param code What refused.
/// @return The refusal, with no system code.
[[nodiscard]] std::unexpected<FsError> refuse(FsErrorCode code);

/// Checks what a native file system is built with.
/// @param options The options.
/// @return Nothing, or `Invalid` for a `maxReadChunk` of 0, which would make every read return
///         nothing and claim more is to come.
[[nodiscard]] FsResult<void> checkOptions(NativeFileSystemOptions const& options);

/// The key of a file system's metadata hashes.
using MetadataSecret = std::array<std::byte, 32>;

/// What one native file system and all of its descriptors share, fixed at construction.
struct CORE_FS_API NativeContext
{
    async::IExecutor* home = nullptr;                ///< Where every operation resumes.
    async::IExecutor* blocking = nullptr;            ///< Where every system call runs.
    platform::IWallClock const* wallClock = nullptr; ///< What `SetToNow` reads where it is explicit.
    PathPolicy const* policy = nullptr;              ///< The name rules, never null.
    /// What the kernel itself would split a name on (`/`; `\`, `/` and `:` on Windows): refused in
    /// a name whatever @c policy allows, so an overriding policy can never hand the kernel more
    /// than one component to resolve.
    std::string_view kernelSeparators;
    NativeFileSystemOptions options; ///< What the file system was built with.
    MetadataSecret secret {};        ///< The key of its metadata hashes.
    /// The kernel's beneath step @c NativeFileSystemOptions::strategy chose, probed once, here;
    /// null where every path is walked one component at a time, which is also the case under
    /// `Best` with a `maxSymlinkHops` below @c DefaultMaxSymlinkHops, a limit only the walk keeps.
    std::shared_ptr<IBeneathOpener const> beneathOpener;
    /// The NT calls a Windows file system opens, inspects and identifies through: the system's, or a
    /// test's scripted ones. Null on every other system.
    INtFiles const* ntFiles = nullptr;
    /// The process token's user, and the security descriptors an owner-only creation is made with,
    /// read once, when the file system is built. Null on every system but Windows.
    std::shared_ptr<OwnerSecurity const> ownerSecurity;
    /// What makes an owner-only file or directory, its staging key drawn when the file system is
    /// built. Null on Windows.
    std::shared_ptr<OwnerOnlyCreator const> ownerOnlyCreator;
    /// Where a relation walk leaves a mount that may hide real parents, made when the file system is
    /// built. Null on Windows, and where this system's could not be made (every relation is then
    /// `Unsupported`).
    std::shared_ptr<IMountBoundaries const> mountBoundaries;

    /// @param homeExecutor Where every operation resumes.
    /// @param blockingExecutor Where every system call runs.
    /// @param clock What `SetToNow` reads.
    /// @param random Where the secret is drawn from, once, here.
    /// @param fileSystemOptions What the file system is built with; a null policy is the native one.
    /// @param separators What this kernel splits a name on.
    /// @param files The NT calls on Windows; null elsewhere. Must outlive the file system.
    /// @param security The owner's security on Windows; null elsewhere.
    /// @param creator What makes owner-only objects on POSIX; null on Windows.
    /// @param boundaries The mount boundaries a POSIX relation walk checks; null on Windows.
    /// The strategy's opener is made here: under `Best`, from @c makeBestBeneathOpener.
    NativeContext(async::IExecutor& homeExecutor,
                  async::IExecutor& blockingExecutor,
                  platform::IWallClock const& clock,
                  IRandom& random,
                  NativeFileSystemOptions fileSystemOptions,
                  std::string_view separators,
                  INtFiles const* files,
                  std::shared_ptr<OwnerSecurity const> security,
                  std::shared_ptr<OwnerOnlyCreator const> creator,
                  std::shared_ptr<IMountBoundaries const> boundaries);
};

/// How a native file system and its descriptors share their @c NativeContext.
using SharedContext = std::shared_ptr<NativeContext const>;

/// What a metadata hash is computed over: the object's identity, size and the times its data and
/// its metadata last changed.
struct MetadataFields
{
    std::uint64_t device = 0;     ///< The device or volume.
    std::uint64_t fileIdLow = 0;  ///< The file id on it, low 64 bits.
    std::uint64_t fileIdHigh = 0; ///< The file id, high 64 bits (Windows' 128-bit ids).
    std::uint64_t size = 0;       ///< The size in bytes.
    FileTime modified;            ///< The last change of its data.
    FileTime statusChanged;       ///< The last change of its metadata.
};

/// HMAC-SHA-256 of @p fields, keyed by @p secret, truncated to 128 bits: it changes when any field
/// does and reveals none of them to whoever does not hold the secret.
/// @param secret The file system's key.
/// @param fields What is hashed.
/// @return The hash.
[[nodiscard]] MetadataHash hashMetadata(MetadataSecret const& secret, MetadataFields const& fields) noexcept;

/// @param when An instant of the wall clock.
/// @return It as seconds and nanoseconds since the Unix epoch.
[[nodiscard]] FileTime fileTimeOf(std::chrono::system_clock::time_point when) noexcept;

/// @param type What a descriptor refers to.
/// @return Nothing for a directory; `NotDirectory` otherwise, since a file descriptor resolves no
///         path.
[[nodiscard]] FsResult<void> requireDirectory(DescriptorType type);

/// @param context A file system's context.
/// @param finalPolicy Whether a final link is followed.
/// @param onLink What a trailing `/` does to a final link under `NoFollow`.
/// @return The walk's rules under @p context's policy, hop limit and kernel separators.
[[nodiscard]] WalkRules walkRulesOf(NativeContext const& context,
                                    SymlinkPolicy finalPolicy,
                                    TrailingSlashOnLink onLink) noexcept;

/// What the native descriptors of every OS share: the file system's context, the handle, the
/// flags and the type, the offload every operation goes through, and the recognition of a
/// descriptor of the same file system passed as `newDirectory`.
class NativeDescriptorBase: public IDescriptor
{
  public:
    /// @return The flags the descriptor carries.
    [[nodiscard]] DescriptorFlags flags() const noexcept final { return _flags; }

    /// A descriptor passed to another one or to its file system: its handle, rights and type, read
    /// on the caller's thread while the caller still holds it.
    struct Target
    {
        SharedHandle handle;                           ///< Its handle, shared with the job.
        DescriptorFlags flags = DescriptorFlags::None; ///< Its rights.
        DescriptorType type = DescriptorType::Unknown; ///< What it is.
    };

    /// @param context A native file system's.
    /// @param descriptor Any descriptor.
    /// @return @p descriptor's handle, rights and type; `CrossDevice` unless it is a descriptor of
    ///         the file system @p context is (D11).
    [[nodiscard]] static FsResult<Target> targetIn(SharedContext const& context,
                                                   IDescriptor const& descriptor);

  protected:
    /// @param context The file system's.
    /// @param handle The descriptor's handle.
    /// @param flags What it carries.
    /// @param type What it refers to.
    NativeDescriptorBase(SharedContext context,
                         SharedHandle handle,
                         DescriptorFlags flags,
                         DescriptorType type):
        _context { std::move(context) }, _handle { std::move(handle) }, _flags { flags }, _type { type }
    {
    }

    /// Runs @p work on the blocking executor and resumes on home. @p work captures by value
    /// everything it uses, the handle included, and never the descriptor.
    template <typename F>
    [[nodiscard]] auto submit(F work) const
    {
        return async::offload(_context->home, _context->blocking, std::move(work));
    }

    /// @return @p other's handle, rights and type; `CrossDevice` unless it is a descriptor of this
    ///         very file system, `BadDescriptor` for none at all.
    [[nodiscard]] FsResult<Target> sameSystemTarget(IDescriptor* other) const;

    /// @return @p other's handle, rights and type, for @c isSameObject; nullopt where it is a
    ///         descriptor of another file system, which is never the same object (D11);
    ///         `BadDescriptor` for none at all.
    [[nodiscard]] FsResult<std::optional<Target>> sameObjectCandidate(IDescriptor* other) const;

    /// @return The file system's context.
    [[nodiscard]] SharedContext const& context() const noexcept { return _context; }

    /// @return The descriptor's handle.
    [[nodiscard]] SharedHandle const& handle() const noexcept { return _handle; }

    /// @return What the descriptor refers to.
    [[nodiscard]] DescriptorType objectType() const noexcept { return _type; }

  private:
    SharedContext _context;
    SharedHandle _handle;
    DescriptorFlags _flags;
    DescriptorType _type;
};

/// What a native file system's @c IDescriptorFileSystem::relation is: both descriptors read on the
/// caller's thread, then @c nativeRelation on the blocking executor.
/// @param context The file system's.
/// @param first A directory of it.
/// @param second Another, or the same.
/// @return The task; `CrossDevice` for a foreign descriptor, `NotDirectory` for one that is not a
///         directory, or what @c nativeRelation answers.
[[nodiscard]] async::Task<FsResult<Relation>> relationOf(SharedContext const& context,
                                                         IDescriptor const& first,
                                                         IDescriptor const& second);

} // namespace core::fs::detail
