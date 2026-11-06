// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/CrossingHandle.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

#include <vthost/handover/HandoverCodec.hpp>

namespace vthost::handover
{

/// The largest handover image there may be: what the old image refuses to write
/// (UpgradeRefusal::ImageTooLarge) and what the new image refuses to read, one constant, so that an image the
/// writer lets go is one the reader takes. A 64-bit count, as it exceeds what a 32-bit size_t holds.
inline constexpr uint64_t MaxHandoverImageSize = uint64_t { 4 } << 30;

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

    /// Reads everything a memory object holds, from its start.
    /// @param handle What write() returned, in this or in the process that took over from this one.
    /// @return The bytes, or why they could not be read.
    [[nodiscard]] virtual std::expected<std::vector<uint8_t>, HandoverError> read(
        vtpty::CrossingHandle handle) = 0;

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
    /// `pwrite`/`pread` on the descriptor: the object's size is the image's.
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
