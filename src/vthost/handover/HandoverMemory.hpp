// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/CrossingHandle.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <vthost/handover/HandoverCodec.hpp>

namespace vthost::handover
{

/// The largest handover image there may be: what the old image refuses to write
/// (UpgradeRefusal::ImageTooLarge) and what the new image refuses to read, one constant, so that an image the
/// writer lets go is one the reader takes. A 64-bit count, as it exceeds what a 32-bit size_t holds.
inline constexpr uint64_t MaxHandoverImageSize = uint64_t { 4 } << 30;

/// A handover image as HandoverMemory::read() hands it out: the bytes where they were written, in a view of
/// the memory object mapped into this process, so that the image is decoded in place rather than copied out
/// first. Unmapped as the last copy of it is destroyed; it holds its mapping by itself, so the memory
/// object's handle may be closed first.
class HandoverView
{
  public:
    /// An empty image, which nothing maps.
    HandoverView() = default;

    /// @param bytes The image's bytes.
    /// @param keepAlive What keeps @p bytes where they are -- the mapping they are in, unmapped as it is
    ///        destroyed, or a buffer that holds them.
    HandoverView(std::span<uint8_t const> bytes, std::shared_ptr<void const> keepAlive) noexcept:
        _bytes { bytes }, _keepAlive { std::move(keepAlive) }
    {
    }

    /// @param buffer The image, which the view takes.
    /// @return A view of it, holding it.
    [[nodiscard]] static HandoverView holding(std::vector<uint8_t> buffer)
    {
        auto held = std::make_shared<std::vector<uint8_t> const>(std::move(buffer));
        return HandoverView { *held, held };
    }

    /// @return The image's bytes, valid for as long as this view lives and is not moved from.
    [[nodiscard]] std::span<uint8_t const> bytes() const noexcept { return _bytes; }

  private:
    std::span<uint8_t const> _bytes;
    std::shared_ptr<void const> _keepAlive;
};

/// The anonymous memory object a handover image is written into, and which the new process reads it back
/// from. An interface, so the orchestrator is tested without touching the operating system.
///
/// The descriptor write() returns has no name on disk. It is close-on-exec until keepAcrossExec(), so that
/// it is inherited by the one exec that should and by no child spawned in between. On Windows it is the
/// handle of an unnamed, pagefile-backed file mapping. It is never inheritable: the successor process the
/// upgrade starts takes it, with every other handle the image names, through that one launch's handle list
/// (@see SuccessorHandoff), and keepAcrossExec() and closeOnExecAgain() do nothing there.
class HandoverMemory
{
  public:
    HandoverMemory() = default;
    HandoverMemory(HandoverMemory const&) = delete;
    HandoverMemory(HandoverMemory&&) = delete;
    HandoverMemory& operator=(HandoverMemory const&) = delete;
    HandoverMemory& operator=(HandoverMemory&&) = delete;
    virtual ~HandoverMemory() = default;

    /// Stores @p image in a new anonymous memory object.
    /// @param image The bytes.
    /// @return The descriptor or handle, which the caller owns and closes; or why it could not be made.
    [[nodiscard]] virtual std::expected<vtpty::CrossingHandle, HandoverError> write(
        std::span<uint8_t const> image) = 0;

    /// Maps the image a memory object holds into this process, without copying it.
    /// @param handle What write() returned, in this or in the process that took over from this one. Not
    ///        closed: the view outlives it.
    /// @return A view of the image; or Truncated where the length the object carries ahead of the image
    ///         exceeds what it holds or any image may have, and MemoryUnavailable where it cannot be mapped.
    [[nodiscard]] virtual std::expected<HandoverView, HandoverError> read(vtpty::CrossingHandle handle) = 0;

    /// Makes @p handle survive the next exec: clears its close-on-exec flag. Called right before the exec,
    /// for the memory object and for every other descriptor the image names (PTY masters, listeners, the
    /// lease).
    /// @param handle A descriptor of this process.
    /// @return Nothing, or why the flag could not be cleared.
    [[nodiscard]] virtual std::expected<void, HandoverError> keepAcrossExec(vtpty::CrossingHandle handle) = 0;

    /// Undoes keepAcrossExec() after an exec that failed: sets @p handle's close-on-exec flag again, so that
    /// no program this process starts later inherits it.
    /// @param handle A descriptor keepAcrossExec() was called for.
    /// @return Nothing, or why the flag could not be set.
    [[nodiscard]] virtual std::expected<void, HandoverError> closeOnExecAgain(
        vtpty::CrossingHandle handle) = 0;

    /// Lets go of a memory object no exec took: closes what write() returned.
    /// @param handle What write() returned.
    virtual void discard(vtpty::CrossingHandle handle) = 0;
};

/// What kind of anonymous memory object an image is stored in.
enum class HandoverMemoryObject : uint8_t
{
    MemFd = 0,    ///< `memfd_create`: Linux, FreeBSD 13 and later.
    SharedMemory, ///< A POSIX shared memory object, created under a random name and unlinked at once.
};

/// How an image's bytes get into the memory object and out of it again.
enum class HandoverMemoryAccess : uint8_t
{
    /// `pwrite` on the descriptor, and a mapping of the whole object to read it: the object's size is the
    /// image's.
    Descriptor = 0,
    /// A shared mapping, the size of the image first: Darwin's shared memory objects take no `read`/`write`,
    /// may be sized only once, and report their size rounded up to a page.
    Mapped,
};

/// How a platform stores a handover image.
struct HandoverMemoryKind
{
    HandoverMemoryObject object = HandoverMemoryObject::MemFd;      ///< What it is stored in.
    HandoverMemoryAccess access = HandoverMemoryAccess::Descriptor; ///< How it is written and read.

    bool operator==(HandoverMemoryKind const&) const = default;
};

/// @return How this platform stores a handover image: a memfd on Linux and FreeBSD, a mapped shared memory
///         object on macOS, a shared memory object read through its descriptor on the other BSDs.
[[nodiscard]] constexpr HandoverMemoryKind nativeHandoverMemoryKind() noexcept
{
#if defined(__linux__) || defined(__FreeBSD__)
    return HandoverMemoryKind { .object = HandoverMemoryObject::MemFd,
                                .access = HandoverMemoryAccess::Descriptor };
#elifdef __APPLE__
    return HandoverMemoryKind { .object = HandoverMemoryObject::SharedMemory,
                                .access = HandoverMemoryAccess::Mapped };
#else
    return HandoverMemoryKind { .object = HandoverMemoryObject::SharedMemory,
                                .access = HandoverMemoryAccess::Descriptor };
#endif
}

/// @return The platform's memory object (nativeHandoverMemoryKind()); on Windows an unnamed file mapping.
[[nodiscard]] std::unique_ptr<HandoverMemory> makeNativeHandoverMemory();

/// @param kind How the image is stored. A kind the platform lacks (a memfd on macOS) fails every write with
///        MemoryUnavailable. What lets the macOS kind be tested where the tests run.
/// @return A memory object of that kind; on Windows, which has only the one, an unnamed file mapping.
[[nodiscard]] std::unique_ptr<HandoverMemory> makeHandoverMemory(HandoverMemoryKind kind);

} // namespace vthost::handover
