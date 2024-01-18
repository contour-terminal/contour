// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <system_error>

#if defined(_WIN32)
    #include <string>

    #include <Windows.h>
#else
    #include <unistd.h>
#endif

namespace crispy
{

template <typename T>
struct close_native_handle
{
};

#if defined(_WIN32)
template <>
struct close_native_handle<HANDLE>
{
    void operator()(HANDLE value) { CloseHandle(value); }
};
#else
template <>
struct close_native_handle<int>
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
                default: throw std::system_error(errno, std::system_category(), "close() failed");
            }
        }
    }
};
#endif

template <typename T, const T InvalidHandleValue>
class native_handle
{
  public:
    using native_handle_type = T;
    static inline constexpr native_handle_type invalid_native_handle = InvalidHandleValue; // NOLINT

  private:
    native_handle(native_handle_type fd) noexcept: _fd { fd } {}

  public:
    native_handle() noexcept = default;

    native_handle(native_handle const&) = delete;
    native_handle& operator=(native_handle const&) = delete;

    native_handle(native_handle&& fd) noexcept: _fd(fd.release()) {}
    native_handle& operator=(native_handle&& fd) noexcept
    {
        if (this != &fd)
        {
            close();
            _fd = fd.release();
        }
        return *this;
    }

    ~native_handle() { close(); }

    static native_handle from_native(native_handle_type fd) // NOLINT
    {
        if (fd == invalid_native_handle)
            throw std::system_error(errno, std::system_category(), "native_handle() failed");
        return native_handle { fd };
    }

    [[nodiscard]] native_handle_type get() const noexcept { return _fd; }
    operator native_handle_type() const noexcept { return _fd; }

    [[nodiscard]] bool is_closed() const noexcept { return _fd == invalid_native_handle; }
    [[nodiscard]] bool is_open() const noexcept { return !is_closed(); }

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

        crispy::close_native_handle<native_handle_type> {}(_fd);
        _fd = invalid_native_handle;
    }

  private:
    native_handle_type _fd = invalid_native_handle;
};

#if defined(_WIN32)
using file_descriptor = native_handle<HANDLE, INVALID_HANDLE_VALUE>;
#else
using file_descriptor = native_handle<int, -1>;
#endif

} // end namespace crispy

#if defined(_WIN32)
template <>
struct fmt::formatter<HANDLE>: fmt::formatter<std::string>
{
    auto format(HANDLE value, format_context& ctx) -> format_context::iterator
    {
        auto str = fmt::format("0x{:X}", (unsigned long long) (value));
        return fmt::formatter<std::string>::format(str, ctx);
    }
};
#endif

template <>
struct fmt::formatter<crispy::file_descriptor>: fmt::formatter<crispy::file_descriptor::native_handle_type>
{
    auto format(crispy::file_descriptor const& fd, format_context& ctx) -> format_context::iterator
    {
        return fmt::formatter<crispy::file_descriptor::native_handle_type>::format(fd.get(), ctx);
    }
};
