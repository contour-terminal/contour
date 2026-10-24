// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <utility>

namespace vtpty
{

/// Owns one file descriptor and closes it on destruction. Move-only; -1 means "none".
class OwnedFd
{
  public:
    OwnedFd() = default;

    /// @param fd The descriptor to own, or -1 for none.
    explicit OwnedFd(int fd) noexcept: _fd { fd } {}

    OwnedFd(OwnedFd&& other) noexcept: _fd { std::exchange(other._fd, -1) } {}

    OwnedFd& operator=(OwnedFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _fd = std::exchange(other._fd, -1);
        }
        return *this;
    }

    OwnedFd(OwnedFd const&) = delete;
    OwnedFd& operator=(OwnedFd const&) = delete;

    ~OwnedFd() { reset(); }

    /// @return The descriptor, or -1 when none is owned.
    [[nodiscard]] int get() const noexcept { return _fd; }

    /// @return Whether a descriptor is owned.
    [[nodiscard]] bool isOpen() const noexcept { return _fd >= 0; }

    /// Gives up ownership without closing.
    /// @return The descriptor, or -1 when none was owned.
    [[nodiscard]] int release() noexcept { return std::exchange(_fd, -1); }

    /// Closes the owned descriptor, if any.
    void reset() noexcept;

  private:
    int _fd = -1;
};

} // namespace vtpty
