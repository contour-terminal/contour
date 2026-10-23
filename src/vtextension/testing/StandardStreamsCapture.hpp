// SPDX-License-Identifier: Apache-2.0
//
// Test-only (POSIX): captures what the process writes to its standard output and error, so that a test
// can check that a guest's writes reach nobody.
#pragma once

#ifndef _WIN32

    #include <catch2/catch_test_macros.hpp>

    #include <algorithm>
    #include <array>
    #include <cstddef>
    #include <string>

    #include <fcntl.h>
    #include <unistd.h>

namespace vtextension::testing
{

/// Sends the process's standard output and error into a pipe for as long as it lives, or until
/// finish(); restores them however the test ends.
class StandardStreamsCapture
{
  public:
    StandardStreamsCapture()
    {
        REQUIRE(::pipe(_pipe.data()) == 0);
        REQUIRE(::fcntl(_pipe[0], F_SETFL, O_NONBLOCK) == 0);
        _savedOut = ::dup(STDOUT_FILENO);
        _savedErr = ::dup(STDERR_FILENO);
        REQUIRE(::dup2(_pipe[1], STDOUT_FILENO) >= 0);
        REQUIRE(::dup2(_pipe[1], STDERR_FILENO) >= 0);
    }

    StandardStreamsCapture(StandardStreamsCapture const&) = delete;
    StandardStreamsCapture(StandardStreamsCapture&&) = delete;
    StandardStreamsCapture& operator=(StandardStreamsCapture const&) = delete;
    StandardStreamsCapture& operator=(StandardStreamsCapture&&) = delete;

    ~StandardStreamsCapture()
    {
        restore();
        ::close(_pipe[0]);
    }

    /// Restores the streams.
    /// @return What was written to them meanwhile.
    [[nodiscard]] std::string finish()
    {
        restore();
        auto buffer = std::array<char, 256> {};
        auto const count = ::read(_pipe[0], buffer.data(), buffer.size());
        return { buffer.data(), static_cast<std::size_t>(std::max(count, ssize_t { 0 })) };
    }

  private:
    void restore() noexcept
    {
        if (_savedOut < 0)
            return;
        ::dup2(_savedOut, STDOUT_FILENO);
        ::dup2(_savedErr, STDERR_FILENO);
        ::close(_savedOut);
        ::close(_savedErr);
        ::close(_pipe[1]);
        _savedOut = -1;
    }

    std::array<int, 2> _pipe {};
    int _savedOut = -1;
    int _savedErr = -1;
};

} // namespace vtextension::testing

#endif
