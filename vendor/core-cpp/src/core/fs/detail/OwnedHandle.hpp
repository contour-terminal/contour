// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The native handles of the fs module: one a job holds alone, and one a descriptor shares with
/// every job in flight on it (decision C2), whose last owner hands the close to the blocking
/// executor so `close` never runs on the caller's loop.

#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/fs/FsExport.hpp>
#include <core/platform/Types.hpp>

#include <memory>
#include <new>
#include <utility>

namespace core::fs::detail
{

/// Closes @p handle: `close` on POSIX, `CloseHandle` on Windows. Defined by the platform's
/// descriptor source.
/// @param handle An open handle, closed exactly once.
CORE_FS_API void closeNativeHandle(platform::NativeHandle handle) noexcept;

/// An open native handle with one owner, closed where it is destroyed: a handle a job opens and
/// closes on the blocking executor, or one on its way to becoming an @c OwnedHandle.
class UniqueHandle
{
  public:
    /// @param handle An open handle; this object closes it.
    explicit UniqueHandle(platform::NativeHandle handle) noexcept: _handle { handle } {}

    UniqueHandle(UniqueHandle const&) = delete;
    UniqueHandle& operator=(UniqueHandle const&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept:
        _handle { std::exchange(other._handle, platform::InvalidHandle) }
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _handle = std::exchange(other._handle, platform::InvalidHandle);
        }
        return *this;
    }

    ~UniqueHandle() { reset(); }

    /// Closes the handle now, if it is still open.
    void reset() noexcept
    {
        if (_handle != platform::InvalidHandle)
            closeNativeHandle(std::exchange(_handle, platform::InvalidHandle));
    }

    /// @return The handle, borrowed for as long as this object holds it.
    [[nodiscard]] platform::NativeHandle get() const noexcept { return _handle; }

    /// Gives the handle up without closing it, to an owner that closes it otherwise (`fdopendir`).
    /// @return The handle, now the caller's to close.
    [[nodiscard]] platform::NativeHandle release() noexcept
    {
        return std::exchange(_handle, platform::InvalidHandle);
    }

  private:
    platform::NativeHandle _handle;
};

/// Closes @p handle on @p closer. Should the executor free this frame without running it (an
/// executor destroyed with work queued), the handle closes there and then, rather than leaking.
/// @param closer Where the close runs.
/// @param handle The handle to close.
inline async::DetachedTask closeOn(async::IExecutor* closer, UniqueHandle handle)
{
    co_await async::ResumeOn { *closer };
    handle.reset();
}

/// The handle a descriptor and every job it offloads share.
///
/// A descriptor holds it through a @c SharedHandle, and every job captures a copy of that pointer
/// by value, so the handle stays open until the last of them is gone: a descriptor dropped while a
/// read is parked leaves the read its handle, and no other open can reuse the number until the read
/// has run. Whoever lets go last, on whatever thread, the close itself runs on the blocking
/// executor: `close` on a network file system can wait as long as a read.
class OwnedHandle
{
  public:
    /// @param handle The open handle.
    /// @param closer Where it is closed; the blocking executor. Must outlive this object, or be
    ///        destroyed with the close still queued, which then closes inline.
    OwnedHandle(UniqueHandle handle, async::IExecutor& closer) noexcept:
        _handle { std::move(handle) }, _closer { &closer }
    {
    }

    OwnedHandle(OwnedHandle const&) = delete;
    OwnedHandle(OwnedHandle&&) = delete;
    OwnedHandle& operator=(OwnedHandle const&) = delete;
    OwnedHandle& operator=(OwnedHandle&&) = delete;

    /// Queues the close on the closer; where the frame that would run it cannot be allocated, the
    /// handle closes here instead, since a destructor may not throw.
    ~OwnedHandle()
    {
        try
        {
            closeOn(_closer, std::move(_handle));
        }
        catch (std::bad_alloc const&)
        {
            // The handle was moved into the call's argument, which closed it as the call unwound;
            // this closes it should it still be here.
            _handle.reset();
        }
    }

    /// @return The handle, borrowed for as long as this object lives.
    [[nodiscard]] platform::NativeHandle get() const noexcept { return _handle.get(); }

  private:
    UniqueHandle _handle;
    async::IExecutor* _closer;
};

/// How a descriptor and its jobs share an @c OwnedHandle.
using SharedHandle = std::shared_ptr<OwnedHandle const>;

/// @param handle An open handle.
/// @param closer Where its last owner's close runs: the blocking executor.
/// @return It, shared.
[[nodiscard]] inline SharedHandle shareHandle(UniqueHandle handle, async::IExecutor& closer)
{
    return std::make_shared<OwnedHandle const>(std::move(handle), closer);
}

} // namespace core::fs::detail
