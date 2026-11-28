// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Who may reach a file system object, decided from facts read off its open handle.
///
/// Each operating system has its own reader, selected by CMake, that turns what the handle says
/// (`fstat`; an NFSv4 ACL on FreeBSD; an extended ACL on macOS; the owner and DACL on Windows) into
/// one OS-neutral @c AccessFacts. The decision is @c classifyAccess, one table, the same on every
/// OS, so every OS's rows are tested everywhere. **Nothing is decided by name**: a check of a path
/// followed by an open of it is a race another user wins by renaming, so the facts are always read
/// from the handle the caller then uses.

#include <core/fs/FsExport.hpp>

#include <cstdint>
#include <vector>

namespace core::fs
{

/// Who may reach an object, read from its open handle. **`Shared` is zero**, so a default value
/// fails closed.
enum class AccessScope : std::uint8_t
{
    /// Someone other than the owner may read or write it, or that cannot be ruled out: a group or
    /// other mode bit, an ACL entry granting another principal, a NULL DACL, an entry not understood.
    Shared,
    /// Only the current user may: on POSIX no group or other bit and no ACL beyond the mode; on
    /// Windows every ALLOW entry names the user, LocalSystem or BUILTIN\Administrators.
    OwnerOnly,
    /// Another user owns it, and so may change who reaches it at any time.
    ForeignOwner,
};

/// One entry of an access control list, as the per-OS reader understood it.
struct AccessEntry
{
    /// What the entry does.
    enum class Kind : std::uint8_t
    {
        Allow,   ///< Grants its rights (Windows `ACCESS_ALLOWED_ACE_TYPE`).
        Deny,    ///< Denies them; never widens access, so it is ignored.
        Unknown, ///< Any other type, or an entry the reader cannot interpret: counts as granting.
    };
    /// Whom it names.
    enum class Principal : std::uint8_t
    {
        Owner,          ///< The current user (Windows `TokenUser`).
        System,         ///< LocalSystem, which can reach every object anyway.
        Administrators, ///< BUILTIN\Administrators, which can take ownership of every object anyway.
        Other,          ///< Anyone else: Everyone, Users, Authenticated Users, any group or account.
    };
    /// What it grants.
    enum class Rights : std::uint8_t
    {
        None,        ///< An empty access mask.
        ReadOrWrite, ///< Any right at all.
    };
    /// Where it came from. Inherited entries count exactly as explicit ones do: an object reachable
    /// through an entry its parent handed down is reachable all the same.
    enum class Origin : std::uint8_t
    {
        Explicit,  ///< Set on the object itself.
        Inherited, ///< Handed down from a parent (Windows `INHERITED_ACE`).
    };

    Kind kind = Kind::Unknown;              ///< What it does; unknown by default, which fails closed.
    Principal principal = Principal::Other; ///< Whom it names.
    Rights rights = Rights::ReadOrWrite;    ///< What it grants.
    Origin origin = Origin::Explicit;       ///< Where it came from.
};

/// What a per-OS reader extracts from an open handle; @c classifyAccess decides from it alone.
struct AccessFacts
{
    /// Who owns the object.
    enum class Owner : std::uint8_t
    {
        CurrentUser, ///< The effective uid; Windows' `TokenUser`.
        Other,       ///< Anyone else, BUILTIN\Administrators included.
    };
    /// What the access control list beyond the mode is.
    enum class AclState : std::uint8_t
    {
        Absent,  ///< None: the mode is all (Linux, where POSIX ACL entries are bounded by the mode's group
                 ///< bits).
        Trivial, ///< One equivalent to the mode (a trivial FreeBSD NFSv4 ACL).
        Present, ///< One of its own: @c entries lists it, an entry the reader cannot judge as `Unknown`.
        Null,    ///< A Windows NULL DACL, which grants everyone everything.
    };

    Owner owner = Owner::Other;       ///< Who owns it; another by default, which fails closed.
    std::uint32_t posixMode = 07777;  ///< The POSIX mode bits; 0 where the OS has none (Windows).
    AclState acl = AclState::Null;    ///< The ACL beyond the mode; NULL by default, which fails closed.
    std::vector<AccessEntry> entries; ///< Its entries, where @c acl is @c AclState::Present.
};

/// Decides who may reach an object. Pure: the same facts give the same answer on every OS.
///
/// `ForeignOwner` where another user owns it; else `Shared` where a group or other mode bit is set,
/// the DACL is NULL, or an entry that is not a DENY is unknown or grants any right to a principal
/// other than the user, LocalSystem or BUILTIN\Administrators; else `OwnerOnly`.
/// @param facts What the handle told.
/// @return The scope.
[[nodiscard]] CORE_FS_API AccessScope classifyAccess(AccessFacts const& facts) noexcept;

} // namespace core::fs
