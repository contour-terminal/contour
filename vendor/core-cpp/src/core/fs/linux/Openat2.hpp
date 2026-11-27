// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Linux's beneath step: `openat2` with `RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS`, which resolves a
/// whole directory path in the kernel and refuses, atomically, anything that would leave the
/// directory it starts from. The system call is reached through @c IOpenat2System, so a test can
/// script what it answers. Private to the fs module.

#include <core/fs/FsExport.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace core::fs::detail
{

/// `struct open_how`, `openat2`'s argument, spelled here rather than taken from `<linux/openat2.h>`:
/// the kernel's ABI, which does not change, so building needs neither UAPI 5.6 headers nor glibc
/// 2.32. Whether the running kernel has the call is still asked at run time.
struct OpenHow
{
    std::uint64_t flags = 0;   ///< `open`'s flags.
    std::uint64_t mode = 0;    ///< The mode of a file created.
    std::uint64_t resolve = 0; ///< The `Resolve` bits.
};

/// `openat2`'s resolve bits this module asks for, as `<linux/openat2.h>` numbers them.
enum class Resolve : std::uint8_t
{
    NoMagicLinks = 0x02, ///< `RESOLVE_NO_MAGICLINKS`: no /proc magic link is followed.
    Beneath = 0x08,      ///< `RESOLVE_BENEATH`: nothing resolves above the starting directory.
};

/// `openat2`'s system call number: 437 in the table every architecture shares since Linux 5.6.
inline constexpr long Openat2Syscall = 437;

/// How many times an `openat2` the kernel answered `EAGAIN` (a rename or mount raced the lookup
/// of `..`) is made again before that one call walks instead.
inline constexpr std::size_t Openat2Retries = 8;

/// The `openat2` system call.
class IOpenat2System
{
  public:
    IOpenat2System() = default;
    IOpenat2System(IOpenat2System const&) = delete;
    IOpenat2System(IOpenat2System&&) = delete;
    IOpenat2System& operator=(IOpenat2System const&) = delete;
    IOpenat2System& operator=(IOpenat2System&&) = delete;
    virtual ~IOpenat2System() = default;

    /// @param directory The directory @p path is relative to.
    /// @param path The path.
    /// @param how What to open it with.
    /// @return The new descriptor; -1 with `errno` set on failure.
    [[nodiscard]] virtual int openat2(int directory, char const* path, OpenHow const& how) const noexcept = 0;
};

/// @return The kernel's `openat2`, through `syscall`.
[[nodiscard]] CORE_FS_API std::shared_ptr<IOpenat2System const> systemOpenat2();

/// Builds the `openat2` opener over @p system, once it has asked @p system whether the call is
/// there: with a descriptor no process has (-1), so the probe touches no file system, and the
/// kernel's answer tells `EBADF` (the call is there) from `ENOSYS` (too old a kernel) and `EPERM`
/// (a seccomp filter refusing it).
/// @param system The system call.
/// @return The opener; null where the call is not there, and the walk alone resolves.
[[nodiscard]] CORE_FS_API std::unique_ptr<IBeneathOpener const> makeOpenat2Opener(
    std::shared_ptr<IOpenat2System const> system);

} // namespace core::fs::detail
