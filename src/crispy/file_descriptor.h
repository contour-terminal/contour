// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fmt/format.h>

#include <system_error>

#if defined(_WIN32)
    #include <Windows.h>
#else
    #include <unistd.h>
#endif

namespace crispy
{

class file_descriptor
{
  public:
#if defined(_WIN32)
    using native_handle_type = HANDLE;
    static inline constexpr native_handle_type invalid_native_handle = INVALID_HANDLE_VALUE; // NOLINT
#else
    using native_handle_type = int;
    static inline constexpr native_handle_type invalid_native_handle = -1; // NOLINT
#endif

  private:
    file_descriptor(native_handle_type fd) noexcept: _fd { fd } {}

  public:
    file_descriptor() noexcept = default;

    file_descriptor(file_descriptor const&) = delete;
    file_descriptor& operator=(file_descriptor const&) = delete;

    file_descriptor(file_descriptor&& fd) noexcept: _fd(fd.release()) {}
    file_descriptor& operator=(file_descriptor&& fd) noexcept
    {
        if (this != &fd)
        {
            close();
            _fd = fd.release();
        }
        return *this;
    }

    ~file_descriptor() { close(); }

    static file_descriptor from_native(native_handle_type fd) // NOLINT
    {
        if (fd == invalid_native_handle)
            throw std::system_error(errno, std::system_category(), "file_descriptor() failed");
        return file_descriptor { fd };
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

#if defined(_WIN32)
        CloseHandle(_fd);
        _fd = invalid_native_handle;
#else
        for (;;)
        {
            // clang-format off
            int rv = ::close(_fd);
            switch (rv)
            {
                case 0:
                    _fd = invalid_native_handle;
                    return;
                case EINTR:
                    break;
                default:
                    throw std::system_error(errno, std::system_category(), "close() failed");
            }
            // clang-format on
        }
#endif
    }

  private:
    native_handle_type _fd = invalid_native_handle;
};

} // end namespace crispy

template <>
struct fmt::formatter<crispy::file_descriptor>: fmt::formatter<crispy::file_descriptor::native_handle_type>
{
    auto format(crispy::file_descriptor const& fd, format_context& ctx) -> format_context::iterator
    {
        return fmt::formatter<int>::format(fd.get(), ctx);
    }
};
