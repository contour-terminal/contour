// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How the POSIX descriptors create a file or directory no other user can reach at any moment
/// (@c OpenFlags::OwnerOnly, @c CreationAccess::OwnerOnly). Private to the fs module.

#include <core/Random.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>

#include <sys/stat.h>
#include <sys/types.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace core::fs::detail
{

/// Where an owner-only object is created.
enum class OwnerOnlyCreation : std::uint8_t
{
    /// Under its final name, with mode 0600/0700 given to the creating call: right where no ACL a
    /// directory hands down can widen the mode (Linux, whose default ACL entries the mode caps).
    Direct,
    /// Inside a fresh 0700 staging directory beside it, stripped of its inherited ACL and verified
    /// before anything is created in it, then moved to its final name: where an inherited ACL could
    /// widen the new object between its creation and a strip (macOS, FreeBSD).
    Staged,
};

/// @return How this system creates owner-only objects; defined with its ACL reader
///         (`posix/`, `bsd/`, `darwin/AccessFacts.cpp`).
[[nodiscard]] CORE_FS_API OwnerOnlyCreation nativeOwnerOnlyCreation() noexcept;

/// Reads the status of an open object: `fstat`, or a test's stand-in for a file system that
/// reports another mode than it was given (vfat or exfat under `fmask`, cifs under `noperm`).
using StatHandle = std::function<int(int handle, struct ::stat* status)>;

/// `fstat` itself, the @c StatHandle every native file system uses.
/// @param handle The open object.
/// @param status Where its status goes.
/// @return 0, or -1 with `errno` set.
[[nodiscard]] CORE_FS_API int systemStatHandle(int handle, struct ::stat* status);

/// Creates files and directories beneath a held directory that no other user can reach at any
/// moment, as @c OwnerOnlyCreation says. One per file system, fixed at construction; safe to use
/// from every thread at once. Where any step fails, what was made is removed through the directory.
class CORE_FS_API OwnerOnlyCreator
{
  public:
    /// @param random Draws the key of the staging names, once, here: nobody without it can tell a
    ///        staging name before it is made, so no name made in advance can stand in the way.
    /// @param how Where an object is created.
    /// @param statHandle How a new object's status is read back.
    OwnerOnlyCreator(IRandom& random,
                     OwnerOnlyCreation how,
                     StatHandle statHandle = StatHandle { &systemStatHandle });

    /// Creates the regular file @p name beneath @p directory, mode 0600.
    /// @param directory The held parent.
    /// @param name One component.
    /// @param flags The `openat` flags, `O_CREAT | O_EXCL` among them.
    /// @return The open file; `Exist` where @p name exists; `NotPermitted` where the new file does
    ///         not read owner-only; or what refused a step.
    [[nodiscard]] FsResult<UniqueHandle> createFile(int directory, std::string const& name, int flags) const;

    /// Creates the directory @p name beneath @p directory, mode 0700.
    /// @param directory The held parent.
    /// @param name One component.
    /// @return Nothing; `Exist` where @p name exists; `NotPermitted` where the new directory does not
    ///         read owner-only; or what refused a step.
    [[nodiscard]] FsResult<void> createDirectory(int directory, std::string const& name) const;

    /// @return A staging name no earlier call returned and nobody without the key can predict:
    ///         HMAC-SHA-256 of a counter under the key, 128 bits of it in hex.
    [[nodiscard]] std::string nextStagingName() const;

    /// Strips an ACL @p handle's new object inherited (@c stripInheritedAcl) and reads its access
    /// back through the @c StatHandle.
    /// @param handle The new object.
    /// @param mode The mode it was created with.
    /// @return Nothing once its handle reads owner-only; `NotPermitted` where it does not, or what
    ///         refused the stripping.
    [[nodiscard]] FsResult<void> holdOwnerOnly(int handle, ::mode_t mode) const;

  private:
    std::array<std::byte, 32> _stagingKey {};
    OwnerOnlyCreation _how;
    StatHandle _statHandle;
    mutable std::atomic<std::uint64_t> _stagingCounter { 0 }; ///< Each call's own staging name.
};

} // namespace core::fs::detail
