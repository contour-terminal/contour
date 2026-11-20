// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/graphics/KittyGraphics.hpp>
#include <vtbackend/graphics/KittyMediaPolicy.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

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

/// The source of a host that reads no files: every file medium is refused. The only one on Windows, where
/// the owner, hidden-file and reparse-point rules are not defined yet.
class RefusingMediumSource final: public KittyMediumSource
{
  public:
    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view /*path*/, Medium /*medium*/, MediaPolicy const& /*policy*/) const override
    {
        return std::unexpected(MediaRefusal::Disabled);
    }
};

#ifndef _WIN32
/// The points between the steps of a PosixMediumSource read where a test acts, to prove that a file swapped
/// there is refused or left alone rather than read or deleted. Production passes none.
struct MediumReadInterleavings
{
    /// Called with the canonical path once checkPath() accepted it, just before it is opened.
    std::function<void(std::filesystem::path const&)> pathAccepted {};
    /// Called with the canonical path after the read, just before a `t=t` file is unlinked.
    std::function<void(std::filesystem::path const&)> beforeUnlink {};
};

/// The POSIX (Linux, macOS) source. In this order, refusing at the first step that fails: canonicalize the
/// path (`realpath`) -> checkPath() on the canonical path -> `open(O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)`
/// -> the opened descriptor's own path must be the canonical one (a swap between the check and the open is
/// refused) -> `fstat` -> checkOpenedFile() -> a read bounded by the size limit -> deletionAfterRead() ->
/// `unlink`, only if the path still names the file that was read. Nothing is opened before the path policy
/// accepted it: opening a device node has side effects. A FIFO is opened without blocking and refused.
class PosixMediumSource final: public KittyMediumSource
{
  public:
    /// @param interleavings What runs between the steps; none in production.
    explicit PosixMediumSource(MediumReadInterleavings interleavings = {}):
        _interleavings { std::move(interleavings) }
    {
    }

    [[nodiscard]] std::expected<std::vector<uint8_t>, MediaRefusal> read(
        std::string_view path, Medium medium, MediaPolicy const& policy) const override;

  private:
    MediumReadInterleavings _interleavings;
};
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
