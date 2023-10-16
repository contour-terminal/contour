// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ConPty.h>

#include <crispy/BufferObject.h>

#include <utility>

#include <Windows.h>

using namespace std;

namespace
{
string GetLastErrorAsString()
{
    DWORD errorMessageID = GetLastError();
    if (errorMessageID == 0)
        return "";

    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                     | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr,
                                 errorMessageID,
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                 (LPSTR) &messageBuffer,
                                 0,
                                 nullptr);

    string message(messageBuffer, size);

    LocalFree(messageBuffer);

    return message;
}
} // anonymous namespace

namespace vtpty
{

struct ConPtySlave: public PtySlaveDummy
{
    HANDLE _output;

    explicit ConPtySlave(HANDLE output): _output { output } {}

    int write(std::string_view text) noexcept override
    {
        DWORD nwritten {};
        if (WriteFile(_output, text.data(), static_cast<DWORD>(text.size()), &nwritten, nullptr))
            return static_cast<int>(nwritten);
        else
            return -1;
    }
};

ConPty::ConPty(PageSize const& windowSize): _size { windowSize }
{
    _master = INVALID_HANDLE_VALUE;
    _input = INVALID_HANDLE_VALUE;
    _output = INVALID_HANDLE_VALUE;
    _buffer.resize(10240);
}

ConPty::~ConPty()
{
    ptyLog()("~ConPty()");
    close();
}

bool ConPty::isClosed() const noexcept
{
    return _master == INVALID_HANDLE_VALUE;
}

void ConPty::start()
{
    ptyLog()("Starting ConPTY");
    assert(!_slave);

    _slave = make_unique<ConPtySlave>(_output);

    HANDLE hPipePTYIn { INVALID_HANDLE_VALUE };
    HANDLE hPipePTYOut { INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPty will connect to
    if (!CreatePipe(&hPipePTYIn, &_output, NULL, 0))
        throw runtime_error { GetLastErrorAsString() };

    if (!CreatePipe(&_input, &hPipePTYOut, NULL, 0))
    {
        CloseHandle(hPipePTYIn);
        throw runtime_error { GetLastErrorAsString() };
    }

    // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
    HRESULT hr = CreatePseudoConsole(
        { unbox<SHORT>(_size.columns), unbox<SHORT>(_size.lines) }, hPipePTYIn, hPipePTYOut, 0, &_master);

    if (hPipePTYIn != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYIn);

    if (hPipePTYOut != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYOut);

    if (hr != S_OK)
        throw runtime_error { GetLastErrorAsString() };
}

void ConPty::close()
{
    ptyLog()("ConPty.close()");
    auto const _ = std::lock_guard { _mutex };

    if (_master != INVALID_HANDLE_VALUE)
    {
        ClosePseudoConsole(_master);
        _master = INVALID_HANDLE_VALUE;
    }

    if (_input != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_input);
        _input = INVALID_HANDLE_VALUE;
    }

    if (_output != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_output);
        _output = INVALID_HANDLE_VALUE;
    }
}

Pty::ReadResult ConPty::read(crispy::buffer_object<char>& buffer,
                             std::optional<std::chrono::milliseconds> timeout,
                             size_t size)
{
    // TODO: wait for timeout time at most AND got woken up upon wakeupReader() invokcation.
    (void) timeout;

    auto const n = static_cast<DWORD>(min(size, buffer.bytesAvailable()));

    DWORD nread {};
    if (!ReadFile(_input, buffer.hotEnd(), n, &nread, nullptr))
        return nullopt;

    return { tuple { string_view { buffer.hotEnd(), nread }, false } };
}

void ConPty::wakeupReader()
{
    // TODO: Windows ConPTY does *NOT* support non-blocking / overlapped I/O.
    // How can we make ReadFile() return early? We could maybe WriteFile() to it?
}

int ConPty::write(std::string_view data)
{
    auto const* buf = data.data();
    auto const size = data.size();

    DWORD nwritten {};
    if (WriteFile(_output, buf, static_cast<DWORD>(size), &nwritten, nullptr))
        return static_cast<int>(nwritten);
    else
        return -1;
}

PageSize ConPty::pageSize() const noexcept
{
    return _size;
}

void ConPty::resizeScreen(PageSize cells, std::optional<ImageSize> pixels)
{
    if (!_slave)
        return;

    (void) pixels; // TODO Can we pass that information, too?

    COORD coords;
    coords.X = unbox<unsigned short>(cells.columns);
    coords.Y = unbox<unsigned short>(cells.lines);

    HRESULT const result = ResizePseudoConsole(_master, coords);
    if (result != S_OK)
        throw runtime_error { GetLastErrorAsString() };

    _size = cells;
}

PtySlave& ConPty::slave() noexcept
{
    return *_slave;
}

} // namespace vtpty
