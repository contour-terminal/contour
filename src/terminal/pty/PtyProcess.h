/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <terminal/Process.h>
#include <terminal/pty/Pty.h>

#include <crispy/point.h>

#include <memory>

namespace terminal
{

/**
 * Manages a local process that is connected to a PTY.
 *
 * @see Pty, Process
 */
class PtyProcess: public Pty
{
  public:
    using ExecInfo = Process::ExecInfo;
    using ExitStatus = Process::ExitStatus;

    PtyProcess(ExecInfo const& _exe, PageSize terminalSize, std::optional<ImageSize> _pixels = std::nullopt);

    Pty& pty() noexcept { return *pty_; }
    Pty const& pty() const noexcept { return *pty_; }

    Process& process() noexcept { return *process_; }
    Process const& process() const noexcept { return *process_; }

    // Pty interface
    //
    PtySlave& slave() noexcept override { return pty_->slave(); }
    PtyMasterHandle handle() const noexcept override { return pty_->handle(); }
    void close() override;
    bool isClosed() const noexcept override;
    std::optional<std::string_view> read(size_t _size, std::chrono::milliseconds _timeout) override;
    void wakeupReader() override;
    int write(char const* buf, size_t size) override;
    PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels) override;

  private:
    std::unique_ptr<Pty> pty_;
    std::unique_ptr<Process> process_;
};

} // namespace terminal
