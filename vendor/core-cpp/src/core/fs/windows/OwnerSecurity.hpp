// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Who "the owner" is on Windows, read once per file system from the process token, and the
/// security descriptors an owner-only creation hands to `NtCreateFile`. Private to the fs module.

#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>

#include <cstddef>
#include <memory>
#include <vector>

#include <windows.h>

namespace core::fs::detail
{

/// The process token's user (`TokenUser`), and two absolute security descriptors made for it: owner
/// that user, and a protected DACL (`SE_DACL_PROTECTED`, nothing inherited) with one ALLOW entry
/// granting that user every right, inheritable on the directory's so what is created inside stays
/// the user's too. The owner is set explicitly: an elevated administrator's new objects are owned by
/// BUILTIN\Administrators by default, which would leave a file the user cannot reach unelevated.
///
/// The descriptors point into the object's own buffers, so it is neither copied nor moved; it is
/// built by @c ofProcess and shared.
class CORE_FS_API OwnerSecurity
{
  public:
    /// Reads the process token's user and builds both descriptors.
    /// @return The owner's security; the Win32 error, classified, where the token cannot be read.
    [[nodiscard]] static FsResult<std::shared_ptr<OwnerSecurity const>> ofProcess();

    OwnerSecurity(OwnerSecurity const&) = delete;
    OwnerSecurity(OwnerSecurity&&) = delete;
    OwnerSecurity& operator=(OwnerSecurity const&) = delete;
    OwnerSecurity& operator=(OwnerSecurity&&) = delete;
    ~OwnerSecurity() = default;

    /// @return The token's user SID, for as long as this object lives.
    [[nodiscard]] PSID user() const noexcept;

    /// @return The descriptor an owner-only file is created with.
    [[nodiscard]] PSECURITY_DESCRIPTOR forFile() const noexcept;

    /// @return The descriptor an owner-only directory is created with.
    [[nodiscard]] PSECURITY_DESCRIPTOR forDirectory() const noexcept;

  private:
    /// A protected DACL with its one entry, and the descriptor naming it.
    struct Descriptor
    {
        std::vector<std::byte> acl;
        SECURITY_DESCRIPTOR descriptor {};
    };

    explicit OwnerSecurity(std::vector<std::byte> user);

    /// Builds @p into for @c _user with @p inheritance on its entry.
    [[nodiscard]] FsResult<void> build(Descriptor& into, BYTE inheritance);

    std::vector<std::byte> _user;
    Descriptor _file;
    Descriptor _directory;
};

} // namespace core::fs::detail
