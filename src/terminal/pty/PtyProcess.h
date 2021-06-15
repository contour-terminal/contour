#pragma once
#include <terminal/pty/Pty.h>
#include <terminal/Process.h>

#include <crispy/point.h>

#include <memory>
#include <thread>

namespace terminal {

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

    PtyProcess(ExecInfo const& _exe, crispy::Size terminalSize, std::optional<crispy::Size> _pixels = std::nullopt);
    ~PtyProcess();

    Pty& pty() noexcept { return *pty_; }
    Pty const& pty() const noexcept { return *pty_; }

    Process& process() noexcept { return *process_; }
    Process const& process() const noexcept { return *process_; }

    Process::ExitStatus waitForProcessExit();

    // Pty interface
    //
    void close() override;
    void prepareParentProcess() override;
    void prepareChildProcess() override;
    int read(char* buf, size_t size, std::chrono::milliseconds _timeout) override;
    void wakeupReader() override;
    int write(char const* buf, size_t size) override;
    crispy::Size screenSize() const noexcept override;
    void resizeScreen(crispy::Size _cells, std::optional<crispy::Size> _pixels) override;

  private:
    std::unique_ptr<Pty> pty_;
    std::unique_ptr<Process> process_;
    std::thread processExitWatcher_;
};

}
