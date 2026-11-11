// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32
    #include <io.h>
#else
    #include <unistd.h>
#endif

#include <span>
#include <utility>

namespace vthost::keeper
{

/// Owns one file descriptor and closes it on destruction. Move-only; -1 means "none".
///
/// The keeper's own, header-only twin of vtpty::OwnedFd, whose closing lives in vtpty's library: the keeper
/// links nothing of vtpty's PTY code.
class UniqueFd
{
  public:
    UniqueFd() = default;

    /// @param fd The descriptor to own, or -1 for none.
    explicit UniqueFd(int fd) noexcept: _fd { fd } {}

    UniqueFd(UniqueFd&& other) noexcept: _fd { std::exchange(other._fd, -1) } {}

    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            _fd = std::exchange(other._fd, -1);
        }
        return *this;
    }

    UniqueFd(UniqueFd const&) = delete;
    UniqueFd& operator=(UniqueFd const&) = delete;

    ~UniqueFd() { reset(); }

    /// @return The descriptor, or -1 when none is owned.
    [[nodiscard]] int get() const noexcept { return _fd; }

    /// @return Whether a descriptor is owned.
    [[nodiscard]] bool isOpen() const noexcept { return _fd >= 0; }

    /// Gives up ownership without closing.
    /// @return The descriptor, or -1 when none was owned.
    [[nodiscard]] int release() noexcept { return std::exchange(_fd, -1); }

    /// Closes the owned descriptor, if any.
    void reset() noexcept
    {
        if (_fd < 0)
            return;
#ifdef _WIN32
        ::_close(_fd);
#else
        ::close(_fd);
#endif
        _fd = -1;
    }

  private:
    int _fd = -1;
};

/// Closes every descriptor of @p fds: what arrived with bytes that no frame took, or that must not be kept.
/// @param fds The descriptors; they are closed, so the caller owns none of them afterwards.
inline void closeAll(std::span<int const> fds) noexcept
{
    for (auto const fd: fds)
        UniqueFd { fd }.reset();
}

} // namespace vthost::keeper
