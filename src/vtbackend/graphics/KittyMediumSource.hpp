// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>
#include <system_error>
#include <vector>

// Defined where a platform source can read: PosixMediumSource judges a file by the path its open descriptor
// names, which Linux (/proc/self/fd) and macOS (F_GETPATH) report and the BSDs do not; Win32MediumSource by
// the final path of its handle. Elsewhere -- the BSDs -- every file medium is refused.
#if defined(__linux__) || defined(__APPLE__) || defined(_WIN32)
    #define VTBACKEND_KITTY_FILE_MEDIA 1
#endif

namespace vtbackend::kitty_graphics
{

/// Reads the file a kitty graphics command names (`t=f`, `t=t`) under a MediaPolicy: the seam between the
/// terminal and the filesystem, so the terminal itself never opens a path.
class KittyMediumSource
{
  public:
    virtual ~KittyMediumSource() = default;

    /// Reads the file @p path names, if @p policy allows it, and deletes it afterwards where the policy says.
    /// @param path The path as the application sent it (decoded from base64).
    /// @param medium The command's `t=`.
    /// @param policy The terminal's media policy.
    /// @return The file's bytes, or why it was refused.
    [[nodiscard]] virtual std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view path, Medium medium, MediaPolicy const& policy) const = 0;
};

/// The source of a host that reads no files: every file medium is refused. The only one where
/// VTBACKEND_KITTY_FILE_MEDIA is not defined.
class RefusingMediumSource final: public KittyMediumSource
{
  public:
    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view /*path*/, Medium /*medium*/, MediaPolicy const& /*policy*/) const override
    {
        return std::unexpected(MediaRefusal::Disabled);
    }
};

/// The points between the steps of a platform source's read where a test acts, to prove that a file swapped
/// there is refused or left alone rather than read or deleted. Production passes none.
struct MediumReadInterleavings
{
    /// Called with the requested path once checkPath() accepted it, just before it is walked and opened.
    std::function<void(std::filesystem::path const&)> pathAccepted {};
    /// Called with each component's real path below the root just before it is opened.
    std::function<void(std::filesystem::path const&)> opening {};
    /// Called with the canonical path after the read, just before a `t=t` file is deleted.
    std::function<void(std::filesystem::path const&)> beforeDelete {};
};

#ifndef _WIN32

/// The POSIX (Linux, macOS) source. In this order, refusing at the first step that fails:
/// - checkPath() on the requested path, lexically normal, before anything is looked up;
/// - open its enclosingRoot(), whose own links are followed, and checkPath() on the real path that the root's
///   real path and the requested components make, before anything below the root is opened;
/// - walk down one component at a time with `openat(O_NOFOLLOW)`, refusing any link below the root without
///   looking its target up (the file itself opened `O_RDONLY|O_NONBLOCK`, so a FIFO does not block);
/// - the opened descriptor's own path must be the real path judged;
/// - `fstat`, the owner compared with the reader's user id, checkOpenedFile(), a read bounded by the size
///   limit;
/// - deletionAfterRead(), then `unlinkat` in the walked directory, only if it is still where it was and the
///   name still names the file read.
/// Nothing outside the roots is resolved, and nothing is opened before the path policy accepted it: opening a
/// device node has side effects.
class PosixMediumSource final: public KittyMediumSource
{
  public:
    /// @param readerId The user id the terminal reads as (`getuid()`): every file read must be its.
    /// @param interleavings What runs between the steps; none in production.
    explicit PosixMediumSource(uint32_t readerId, MediumReadInterleavings interleavings = {}):
        _readerId { readerId }, _interleavings { std::move(interleavings) }
    {
    }

    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view path, Medium medium, MediaPolicy const& policy) const override;

  private:
    uint32_t _readerId;
    MediumReadInterleavings _interleavings;
};
#else
/// The Windows source. In this order, refusing at the first step that fails:
/// - requestedPath(), then the spelling as Windows reads it (win32::checkPathSpelling()): drive-absolute,
///   no UNC or device namespace, no stream, device name, wildcard or trailing dot; then the drive's type
///   (fixed or RAM disk only);
/// - open its enclosingRoot() (`\\?\`, its own links followed), and checkPath() on the real path that the
///   root's final path and the requested components make, before anything below the root is opened;
/// - walk down one component at a time with NtCreateFile relative to the directory above
///   (FILE_OPEN_REPARSE_POINT), refusing any reparse point -- junction or link -- and any hidden or system
///   entry below the root, holding every directory so none is moved meanwhile;
/// - open the file shared for reading only (nobody writes, renames or deletes it meanwhile), with DELETE
///   when deletionAfterRead() says so;
/// - its final path must be the path walked (ignoring case), which also refuses a short (8.3) alias;
/// - its attributes (win32::checkAttributes()), its owner compared with the reader's SIDs,
///   checkOpenedFile(), a read bounded by the size limit;
/// - a `t=t` file is deleted through the very handle read.
class Win32MediumSource final: public KittyMediumSource
{
  public:
    /// A security identifier, as its bytes.
    using Sid = std::vector<std::byte>;

    /// @param readers The SIDs a file's owner must be one of: the reading token's user, and the owner it
    ///                gives what it creates (currentReaderSids()).
    /// @param interleavings What runs between the steps; none in production.
    explicit Win32MediumSource(std::vector<Sid> readers, MediumReadInterleavings interleavings = {});

    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view path, Medium medium, MediaPolicy const& policy) const override;

  private:
    std::vector<Sid> _readers;
    MediumReadInterleavings _interleavings;
};

/// @return The SIDs this process reads as: its token's user and, where it differs (an elevated
///         administrator's `BUILTIN\Administrators`), the token's default owner; why not, where the token
///         cannot be read.
[[nodiscard]] std::expected<std::vector<Win32MediumSource::Sid>, std::error_code> currentReaderSids();
#endif

/// What a terminal reads kitty file media with, and under which policy. Given to a Terminal only by a
/// composition root whose bytes come from a local process; the policy is the parsing host's own.
struct KittyMediumServices
{
    /// Borrowed: it must outlive the terminal.
    KittyMediumSource const& source;
    /// The policy every read is judged by.
    MediaPolicy policy;
};

} // namespace vtbackend::kitty_graphics
