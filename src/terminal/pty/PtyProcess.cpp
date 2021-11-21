#include <terminal/pty/PtyProcess.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

using namespace std;

namespace terminal {

PtyProcess::PtyProcess(ExecInfo const& _exe, PageSize _terminalSize, optional<ImageSize> _pixels):
    pty_{
        #if defined(_MSC_VER)
            make_unique<terminal::ConPty>(_terminalSize/*TODO: , _pixels*/),
        #else
            make_unique<terminal::UnixPty>(_terminalSize, _pixels),
        #endif
    },
    process_{ std::make_unique<Process>(_exe, *pty_) },
    processExitWatcher_{
        [this]() {
            auto const exitStatus = process_->wait();
            LOGSTORE(PtyLog)("Process terminated with exit code {}.", exitStatus);
            pty_->close();
        }
    }
{
}

PtyProcess::~PtyProcess()
{
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
    //process_->terminate(Process::TerminationHint::Hangup);
}

bool PtyProcess::isClosed() const
{
    return pty_->isClosed();
}

void PtyProcess::prepareParentProcess()
{
    assert(false && "Don't!");
}

void PtyProcess::prepareChildProcess()
{
    assert(false && "Don't!");
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

PageSize PtyProcess::screenSize() const noexcept
{
    return pty_->screenSize();
}

void PtyProcess::resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels)
{
    pty_->resizeScreen(_cells, _pixels);
}
// }}}

}
