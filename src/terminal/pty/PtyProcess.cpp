#include <terminal/pty/PtyProcess.h>
#include <crispy/debuglog.h>

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#else
#include <terminal/pty/UnixPty.h>
#endif

using namespace std;

namespace terminal {

auto const inline ProcessTag = crispy::debugtag::make("system.process", "Logs OS process informations.");

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
            (void) process_->wait();
            auto const status = process_->checkStatus();
            if (status.has_value())
                debuglog(ProcessTag).write("Process terminated. ({})", status.value());
            else
                debuglog(ProcessTag).write("Process terminated. (Unknown status)");
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

void PtyProcess::prepareParentProcess()
{
    assert(false && "Don't!");
}

void PtyProcess::prepareChildProcess()
{
    assert(false && "Don't!");
}

int PtyProcess::read(char* _buf, size_t _size, std::chrono::milliseconds _timeout)
{
    return pty_->read(_buf, _size, _timeout);
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
