// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <system_error>

#ifndef _WIN32
    #include <unistd.h>
#endif

namespace crispy
{

template <typename T>
struct CloseNativeHandle
{
};

#ifndef _WIN32
template <>
struct CloseNativeHandle<int>
{
    void operator()(int fd)
    {
        for (;;)
        {
            int const rv = ::close(fd);
            switch (rv)
            {
                case 0: return;
                case EINTR: break;
                default: throw std::system_error(errno, std::generic_category(), "close() failed");
            }
        }
    }
};
#endif

template <typename T, T const InvalidHandleValue>
class NativeHandle
{
  public:
    // Mirrors std::thread::native_handle_type, which generic code binds to by spelling.
    // NOLINTNEXTLINE(readability-identifier-naming): standard handle-type trait spelling.
    using native_handle_type = T;
    static constexpr native_handle_type InvalidNativeHandle = InvalidHandleValue;

  private:
    NativeHandle(native_handle_type fd) noexcept: _fd { fd } {}

  public:
    NativeHandle() noexcept = default;

    NativeHandle(NativeHandle const&) = delete;
    NativeHandle& operator=(NativeHandle const&) = delete;

    NativeHandle(NativeHandle&& fd) noexcept: _fd(fd.release()) {}
    NativeHandle& operator=(NativeHandle&& fd) noexcept
    {
        if (this != &fd)
        {
            close();
            _fd = fd.release();
        }
        return *this;
    }

    ~NativeHandle() { close(); }

    static NativeHandle fromNative(native_handle_type fd)
    {
        if (fd == InvalidNativeHandle)
            throw std::system_error(errno, std::generic_category(), "NativeHandle() failed");
        return NativeHandle { fd };
    }

    [[nodiscard]] native_handle_type get() const noexcept { return _fd; }
    operator native_handle_type() const noexcept { return _fd; }

    [[nodiscard]] bool isClosed() const noexcept { return _fd == InvalidNativeHandle; }
    [[nodiscard]] bool isOpen() const noexcept { return !isClosed(); }

    [[nodiscard]] native_handle_type release() noexcept
    {
        native_handle_type const fd = _fd;
        _fd = InvalidNativeHandle;
        return fd;
    }

    void close()
    {
        if (_fd == InvalidNativeHandle)
            return;

        crispy::CloseNativeHandle<native_handle_type> {}(_fd);
        _fd = InvalidNativeHandle;
    }

  private:
    native_handle_type _fd = InvalidNativeHandle;
};

#ifndef _WIN32
/// A POSIX file descriptor, closed with its owner.
///
/// There is no Windows counterpart: nothing there needs one, and a HANDLE could not be one anyway, because
/// INVALID_HANDLE_VALUE is a cast, which no template argument may be.
using FileDescriptor = NativeHandle<int, -1>;
#endif

} // end namespace crispy

#ifndef _WIN32
template <>
struct std::formatter<crispy::FileDescriptor>: std::formatter<crispy::FileDescriptor::native_handle_type>
{
    auto format(crispy::FileDescriptor const& fd, auto& ctx) const
    {
        return std::formatter<crispy::FileDescriptor::native_handle_type>::format(fd.get(), ctx);
    }
};
#endif
