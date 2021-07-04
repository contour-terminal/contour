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
#include <terminal/pty/ConPty.h>

#include <Windows.h>

using namespace std;

namespace {
    string GetLastErrorAsString()
    {
        DWORD errorMessageID = GetLastError();
        if (errorMessageID == 0)
            return "";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorMessageID,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)& messageBuffer,
            0,
            nullptr
        );

        string message(messageBuffer, size);

        LocalFree(messageBuffer);

        return message;
    }
} // anonymous namespace

namespace terminal {

ConPty::ConPty(PageSize const& _windowSize) :
    size_{ _windowSize }
{
    master_ = INVALID_HANDLE_VALUE;
    input_ = INVALID_HANDLE_VALUE;
    output_ = INVALID_HANDLE_VALUE;

    HANDLE hPipePTYIn{ INVALID_HANDLE_VALUE };
    HANDLE hPipePTYOut{ INVALID_HANDLE_VALUE };

    // Create the pipes to which the ConPty will connect to
    if (!CreatePipe(&hPipePTYIn, &output_, NULL, 0))
        throw runtime_error{ GetLastErrorAsString() };

    if (!CreatePipe(&input_, &hPipePTYOut, NULL, 0))
    {
        CloseHandle(hPipePTYIn);
        throw runtime_error{ GetLastErrorAsString() };
    }

    // Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
    HRESULT hr = CreatePseudoConsole(
        { unbox<SHORT>(_windowSize.columns), unbox<SHORT>(_windowSize.lines) },
        hPipePTYIn,
        hPipePTYOut,
        0,
        &master_
    );

    if (hPipePTYIn != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYIn);

    if (hPipePTYOut != INVALID_HANDLE_VALUE)
        CloseHandle(hPipePTYOut);

    if (hr != S_OK)
        throw runtime_error{ GetLastErrorAsString() };
}

ConPty::~ConPty()
{
    close();
}

void ConPty::close()
{
    if (master_ != INVALID_HANDLE_VALUE)
    {
        ClosePseudoConsole(master_);
        master_ = INVALID_HANDLE_VALUE;
    }

    if (input_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(input_);
        input_ = INVALID_HANDLE_VALUE;
    }

    if (output_ != INVALID_HANDLE_VALUE)
    {
        CloseHandle(output_);
        output_ = INVALID_HANDLE_VALUE;
    }
}

void ConPty::prepareParentProcess()
{
}

void ConPty::prepareChildProcess()
{
}

int ConPty::read(char* buf, size_t size, std::chrono::milliseconds _timeout)
{
    // TODO: wait for _timeout time at most AND got woken up upon wakeupReader() invokcation.
    (void) _timeout;

    DWORD nread{};
    if (ReadFile(input_, buf, static_cast<DWORD>(size), &nread, nullptr))
        return static_cast<int>(nread);
    else
        return -1;
}

void ConPty::wakeupReader()
{
    // TODO: Windows ConPTY does *NOT* support non-blocking / overlapped I/O.
    // How can we make ReadFile() return early? We could maybe WriteFile() to it?
}

int ConPty::write(char const* buf, size_t size)
{
    DWORD nwritten{};
    if (WriteFile(output_, buf, static_cast<DWORD>(size), &nwritten, nullptr))
        return static_cast<int>(nwritten);
    else
        return -1;
}

PageSize ConPty::screenSize() const noexcept
{
    return size_;
}

void ConPty::resizeScreen(PageSize _cells, std::optional<ImageSize> _pixels)
{
    (void) _pixels; // TODO Can we pass that information, too?

    COORD coords;
    coords.X = unbox<unsigned short>(_cells.columns);
    coords.Y = unbox<unsigned short>(_cells.lines);

    HRESULT const result = ResizePseudoConsole(master_, coords);
    if (result != S_OK)
        throw runtime_error{ GetLastErrorAsString() };

    size_ = _cells;
}

}  // namespace terminal
