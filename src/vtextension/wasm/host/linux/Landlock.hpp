// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The extension host's file system restriction on Linux: a Landlock ruleset that handles every right the
/// kernel's ABI knows and grants none (but an instrumented build's own), enforced on every thread where the
/// ABI can, and on every thread started later in any case.
///
/// The ABI's rights and flags are our own constants, ABI by ABI: the headers a build sees may be older than
/// the kernel it runs on. A kernel without Landlock, or one whose Landlock is off, is no failure: the system
/// call filter confines the host alone there.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include <vtextension/wasm/host/Sandbox.hpp>

namespace vtextension::wasm::host
{

/// What one Landlock ABI handles, cumulatively.
struct LandlockAbiRow
{
    int abi;                ///< The ABI version, as landlock_create_ruleset() reports it.
    uint64_t fsRights;      ///< Every file system right it knows (LANDLOCK_ACCESS_FS_*).
    uint64_t netRights;     ///< Every network right it knows (LANDLOCK_ACCESS_NET_*).
    uint64_t scopes;        ///< Every scope it knows (LANDLOCK_SCOPE_*).
    uint32_t restrictFlags; ///< What landlock_restrict_self() is passed: TSYNC where it has it.
};

/// @return One row per ABI version from 1, in order.
[[nodiscard]] std::span<LandlockAbiRow const> landlockAbis() noexcept;

// Landlock's file system rights an instrumented build grants itself: our values of LANDLOCK_ACCESS_FS_*.
inline constexpr auto LandlockReadFile = uint64_t { 0b0100 };           ///< LANDLOCK_ACCESS_FS_READ_FILE.
inline constexpr auto LandlockWriteFile = uint64_t { 0b0010 };          ///< LANDLOCK_ACCESS_FS_WRITE_FILE.
inline constexpr auto LandlockReadDirectory = uint64_t { 0b1000 };      ///< LANDLOCK_ACCESS_FS_READ_DIR.
inline constexpr auto LandlockMakeRegular = uint64_t { 0b1'0000'0000 }; ///< LANDLOCK_ACCESS_FS_MAKE_REG.
inline constexpr auto LandlockTruncate = uint64_t { 0b100'0000'0000'0000 }; ///< LANDLOCK_ACCESS_FS_TRUNCATE.

/// What landlock_create_ruleset() is given: the fields every ABI up to the scopes knows.
struct LandlockRulesetAttr
{
    uint64_t handledAccessFs;  ///< The file system rights handled: denied unless a rule grants them.
    uint64_t handledAccessNet; ///< The network rights handled.
    uint64_t scoped;           ///< The scopes restricted.
};

/// A directory beneath which some rights are granted.
struct PathBeneathRule
{
    int parent;             ///< The directory, opened O_PATH; the caller's to close.
    uint64_t allowedAccess; ///< What is granted beneath it; reduced to what the ABI handles.
};

/// The Landlock system calls, injectable. Each returns what the system call returns, or `-errno` on failure.
struct LandlockSystem
{
    /// landlock_create_ruleset(); with a null attribute, size 0 and flag VERSION, the ABI version.
    long (*createRuleset)(LandlockRulesetAttr const* attr, std::size_t size, uint32_t flags) noexcept;
    /// landlock_add_rule() of a LANDLOCK_RULE_PATH_BENEATH rule.
    long (*addPathBeneathRule)(int ruleset, PathBeneathRule rule) noexcept;
    /// landlock_restrict_self().
    long (*restrictSelf)(int ruleset, uint32_t flags) noexcept;
    /// Closes a ruleset's descriptor.
    void (*closeRuleset)(int ruleset) noexcept;
};

/// @return The kernel's Landlock system calls.
[[nodiscard]] LandlockSystem systemLandlock() noexcept;

/// Whether the file system was restricted.
enum class LandlockOutcome : uint8_t
{
    Unavailable, ///< The kernel has no Landlock, or it is off: nothing was restricted, and nothing failed.
    Restricted,  ///< Every right the best ABI knows is handled, and the threads it reaches restricted.
};

/// Restricts the calling thread -- every thread, where the ABI has TSYNC -- and the threads it starts later
/// to the rights @p allowed grants, under the best ABI both the kernel and landlockAbis() know. The thread
/// must have no_new_privs set.
/// @param allowed What is granted, beneath which directories.
/// @param system The system calls.
/// @return `Restricted`, or `Unavailable` where the kernel has no Landlock (`ENOSYS`) or has it off
///         (`EOPNOTSUPP`); `FileSystem` with the errno on any other failure.
[[nodiscard]] std::expected<LandlockOutcome, ConfinementFailure> restrictFileSystem(
    std::span<PathBeneathRule const> allowed, LandlockSystem const& system);

} // namespace vtextension::wasm::host
