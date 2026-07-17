// SPDX-License-Identifier: Apache-2.0
#include <chrono>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <vtconformance/TerminalHarness.h>

using namespace std::chrono_literals;

namespace vtconformance
{

namespace
{
    constexpr auto PollInterval = 5ms;

    /// Spawns the child and returns the PTY the engine takes as its device, reporting the process back.
    ///
    /// The chain is one spine -- Terminal owns Process owns the UnixPty -- so the harness has to take
    /// its reference on the way past, before ownership moves on.
    [[nodiscard]] std::unique_ptr<vtpty::Pty> buildDevice(vtpty::Process::ExecInfo const& exec,
                                                          vtpty::PageSize pageSize,
                                                          vtpty::Process*& process)
    {
        auto spawned = std::make_unique<vtpty::Process>(exec,
                                                        vtpty::createPty(pageSize, std::nullopt),
                                                        /* escapeSandbox */ false);
        process = spawned.get();
        return spawned;
    }
} // namespace

TerminalHarness::TerminalHarness(vtpty::Process::ExecInfo const& exec, Options options):
    _options(options),
    _engine(std::make_unique<TerminalEngine>(buildDevice(exec, options.engine.pageSize, _process),
                                             options.engine))
{
}

TerminalHarness::~TerminalHarness()
{
    stop();
}

void TerminalHarness::start(Pumping pumping)
{
    _engine->device().start();
    if (pumping == Pumping::Internally)
        _pumpThread = std::make_unique<std::thread>([this] { pump(); });
}

void TerminalHarness::pump()
{
    while (!_terminating.load())
        if (!_engine->processInputOnce())
            break;

    _pumpDrained.store(true);
}

void TerminalHarness::startScanning(std::span<std::string_view const> markers)
{
    _scanner.emplace(markers);
    _readBuffer = _bufferPool.allocateBufferObject();
}

TerminalHarness::Batch TerminalHarness::readBatch()
{
    // A fresh buffer per batch: the bytes are handed out as a view and fed before the next read, so the
    // pool only has to keep one alive at a time.
    if (_readBuffer->bytesAvailable() < ReadBufferSize)
        _readBuffer = _bufferPool.allocateBufferObject();

    auto const result = _engine->device().read(*_readBuffer, _options.stepTimeout, ReadBufferSize);
    if (!result)
    {
        // A timeout is not an ending -- vttest sleeps a full second after each answer it replays, and
        // five after a reset. Only a real failure closes the device.
        if (errno == EINTR || errno == EAGAIN)
            return Batch { .bytes = {}, .matches = {}, .closed = false };

        return Batch { .bytes = {}, .matches = {}, .closed = true };
    }

    auto const bytes = result->data;
    if (bytes.empty())
        return Batch { .bytes = {}, .matches = {}, .closed = true };

    return Batch { .bytes = bytes, .matches = _scanner->scan(bytes), .closed = false };
}

void TerminalHarness::stop()
{
    _terminating.store(true);

    if (_process && _process->alive())
        _process->terminate(vtpty::Process::TerminationHint::Hangup);

    if (!_pumpThread)
        return; // The caller drove the reads; there is no thread to unwedge or join.

    // The pump thread is parked in a blocking read; the hangup above unblocks it, but a child that
    // ignores SIGHUP would leave it there forever. Waking the reader explicitly closes that gap.
    _engine->device().wakeupReader();

    if (_pumpThread->joinable())
        _pumpThread->join();
    _pumpThread.reset();
}

bool TerminalHarness::alive() const noexcept
{
    return _process && _process->alive();
}

bool TerminalHarness::waitForExit(std::chrono::milliseconds timeout) const
{
    // The child's death is NOT a barrier for its output. Its last words outlive it: they sit in the
    // pty's buffer until someone reads them, and the pump thread reads on its own schedule. Returning
    // on `!alive()` alone hands the caller a screen the child's final write may not have reached yet --
    // which is a race the caller cannot even see, because the screen it gets is a plausible one.
    //
    // The causal barrier is end-of-file. Once the child is gone its slave end is closed, so the read
    // drains what is buffered and then reports EOF, and the pump loop leaves. That is the point at
    // which "everything the child said has been rendered" is true, and nothing earlier is.
    auto const drained = [this] {
        return !_pumpThread || _pumpDrained.load();
    };

    auto const deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (!alive() && drained())
            return true;
        std::this_thread::sleep_for(PollInterval);
    }
    return !alive() && drained();
}

void TerminalHarness::writeInput(std::string_view text)
{
    _engine->writeInput(text);
}

} // namespace vtconformance
