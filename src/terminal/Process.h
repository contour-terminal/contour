// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#pragma once

#include <variant>
#include <string>

#include <pty.h>

namespace terminal {

struct [[nodiscard]] WindowSize {
    unsigned short rows;
    unsigned short columns;
};

WindowSize currentWindowSize();

/**
 * Spawns and manages a child process with a pseudo terminal attached to it.
 */
class [[nodiscard]] Process {
  public:
    struct NormalExit {
        int exitCode;
    };

    struct SignalExit {
        int signum;
    };

    struct Suspend {
    };

    struct Resume {
    };

    using ExitStatus = std::variant<NormalExit, SignalExit, Suspend, Resume>;

    //! Returns login shell of current user.
    static std::string loginShell();

    Process(WindowSize const& windowSize, const std::string& path);
    explicit Process(const std::string& path) : Process{currentWindowSize(), path} {}

    [[nodiscard]] ExitStatus wait();

    /// Underlying file descriptor to child process I/O.
    int masterFd() const noexcept { return fd_; }

    /// Sends given data to child process.
    [[nodiscard]] ssize_t send(void const* data, size_t size);

    /**
     * Reads data from child process into @p data up to @p size bytes.
     *
     * @returns number of bytes read or 0 if child process terminated (hung up) or -1 on failure.
     */
    [[nodiscard]] int receive(uint8_t* data, size_t size);

  private:
    int fd_;
    pid_t pid_;
};

}  // namespace terminal
