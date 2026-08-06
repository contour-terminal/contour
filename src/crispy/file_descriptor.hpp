// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <system_error>

#ifdef _WIN32
    #include <string>

    #include <Windows.h>
#else
    #include <unistd.h>
#endif

namespace crispy
{

template <typename T>
struct CloseNativeHandle
{
};

#ifdef _WIN32
template <>
struct CloseNativeHandle<HANDLE>
{
    void operator()(HANDLE value) { CloseHandle(value); }
};
#else
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
    using native_handle_type = T;
    static inline constexpr native_handle_type invalid_native_handle = InvalidHandleValue; // NOLINT

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

    static NativeHandle from_native(native_handle_type fd) // NOLINT
    {
        if (fd == invalid_native_handle)
            throw std::system_error(errno, std::generic_category(), "NativeHandle() failed");
        return NativeHandle { fd };
    }

    [[nodiscard]] native_handle_type get() const noexcept { return _fd; }
    operator native_handle_type() const noexcept { return _fd; }

    [[nodiscard]] bool isClosed() const noexcept { return _fd == invalid_native_handle; }
    [[nodiscard]] bool isOpen() const noexcept { return !isClosed(); }

    [[nodiscard]] native_handle_type release() noexcept
    {
        native_handle_type const fd = _fd;
        _fd = invalid_native_handle;
        return fd;
    }

    void close()
    {
        if (_fd == invalid_native_handle)
            return;

        crispy::CloseNativeHandle<native_handle_type> {}(_fd);
        _fd = invalid_native_handle;
    }

  private:
    native_handle_type _fd = invalid_native_handle;
};

#ifdef _WIN32
using FileDescriptor = NativeHandle<HANDLE, INVALID_HANDLE_VALUE>;
#else
using FileDescriptor = NativeHandle<int, -1>;
#endif

} // end namespace crispy

#ifdef _WIN32
template <>
struct std::formatter<HANDLE>: std::formatter<std::string>
{
    auto format(HANDLE value, auto& ctx) const
    {
        auto str = std::format("0x{:X}", (unsigned long long) (value));
        return std::formatter<std::string>::format(str, ctx);
    }
};
#endif

template <>
struct std::formatter<crispy::FileDescriptor>: std::formatter<crispy::FileDescriptor::native_handle_type>
{
    auto format(crispy::FileDescriptor const& fd, auto& ctx) const
    {
        return std::formatter<crispy::FileDescriptor::native_handle_type>::format(fd.get(), ctx);
    }
};
