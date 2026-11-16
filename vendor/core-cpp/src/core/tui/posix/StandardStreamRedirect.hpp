// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: swaps a standard descriptor for another for a scope. @c TerminalInput reads
/// `STDIN_FILENO` and writes `STDOUT_FILENO` and has no other seam to hand it a descriptor, so the
/// POSIX tests that reach it through the real descriptors redirect them with this.

#include <unistd.h>

namespace core::tui::test
{

/// @brief Makes @p fd the process's @p standardStream (`STDIN_FILENO` or `STDOUT_FILENO`) until
/// destroyed, then restores the descriptor it replaced.
class StandardStreamRedirect
{
  public:
    /// @brief Redirects @p standardStream to @p fd.
    /// @param standardStream The descriptor to replace, such as `STDIN_FILENO`.
    /// @param fd The descriptor that stands in for it; it stays open and owned by the caller.
    StandardStreamRedirect(int standardStream, int fd) noexcept:
        _standardStream(standardStream),
        _saved(::dup(standardStream)),
        _isRedirected(_saved >= 0 && ::dup2(fd, standardStream) == standardStream)
    {
    }

    ~StandardStreamRedirect()
    {
        if (_saved >= 0)
        {
            ::dup2(_saved, _standardStream);
            ::close(_saved);
        }
    }

    StandardStreamRedirect(StandardStreamRedirect const&) = delete;
    StandardStreamRedirect& operator=(StandardStreamRedirect const&) = delete;
    StandardStreamRedirect(StandardStreamRedirect&&) = delete;
    StandardStreamRedirect& operator=(StandardStreamRedirect&&) = delete;

    /// @brief Whether the descriptor was swapped.
    /// @return True when the redirect is in effect.
    [[nodiscard]] bool isRedirected() const noexcept { return _isRedirected; }

  private:
    int _standardStream;
    int _saved;
    bool _isRedirected;
};

} // namespace core::tui::test
