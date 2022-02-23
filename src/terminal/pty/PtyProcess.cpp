#include <terminal/pty/PtyProcess.h>

using namespace std;

namespace terminal
{

PtyProcess::PtyProcess(ExecInfo const& _exe, PageSize _terminalSize, optional<ImageSize> _pixels):
    pty_ { createPty(_terminalSize, _pixels) },
    process_ { std::make_unique<Process>(_exe, *pty_) },
    processExitWatcher_ { [this]() {
        auto const exitStatus = process_->wait();
        PtyLog()("Process terminated with exit code {}.", exitStatus);
        pty_->close();
    } }
{
}

PtyProcess::~PtyProcess()
{
    processExitWatcher_.join();
}

Process::ExitStatus PtyProcess::waitForProcessExit()
{
    processExitWatcher_.join();
    return process_->checkStatus().value();
}

// {{{ Pty interface
void PtyProcess::close()
{
    pty_->close();
    // process_->terminate(Process::TerminationHint::Hangup);
}

bool PtyProcess::isClosed() const noexcept
{
    return pty_->isClosed();
}

optional<string_view> PtyProcess::read(size_t _size, std::chrono::milliseconds _timeout)
{
    return pty_->read(_size, _timeout);
}

void PtyProcess::wakeupReader()
{
    return pty_->wakeupReader();
}

int PtyProcess::write(char const* _buf, size_t _size)
{
    return pty_->write(_buf, _size);
}

PageSize PtyProcess::pageSize() const noexcept
{
    return pty_->pageSize();
}

void PtyProcess::resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels)
{
    pty_->resizeScreen(_cells, _pixels);
}
// }}}

} // namespace terminal
