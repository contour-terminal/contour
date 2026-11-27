// SPDX-License-Identifier: Apache-2.0
//
// The Windows handles of StandardStreamsTestSupport.hpp.
#include <core/net/StandardStreamsTestSupport.hpp>

#include <filesystem>
#include <system_error>

#include <windows.h>

namespace core::net::streams_test
{

namespace
{
    /// @return @p handle owned, or a throw naming @p what where it is invalid.
    [[nodiscard]] OwnedHandle ownedOrThrow(HANDLE handle, char const* what)
    {
        if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
        return OwnedHandle { handle };
    }

    /// @return @p path opened with @p access and @p disposition, shared for reading and writing.
    [[nodiscard]] OwnedHandle openFile(wchar_t const* path, DWORD access, DWORD disposition, char const* what)
    {
        return ownedOrThrow(CreateFileW(path,
                                        access,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        disposition,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr),
                            what);
    }
} // namespace

TestPipe makeTestPipe()
{
    auto read = HANDLE {};
    auto write = HANDLE {};
    // CreatePipe's pipe is synchronous, as the anonymous pipe a parent hands its child is.
    if (CreatePipe(&read, &write, nullptr, 0) == 0)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreatePipe");
    return TestPipe { .read = OwnedHandle { read }, .write = OwnedHandle { write } };
}

OwnedHandle openForReading(std::filesystem::path const& path)
{
    return openFile(path.c_str(), GENERIC_READ, OPEN_EXISTING, "open for reading");
}

OwnedHandle openForWriting(std::filesystem::path const& path)
{
    return openFile(path.c_str(), GENERIC_WRITE, CREATE_ALWAYS, "open for writing");
}

OwnedHandle openNullDevice()
{
    return openFile(L"NUL", GENERIC_READ, OPEN_EXISTING, "open NUL");
}

StreamKind pipeStreamKind() noexcept
{
    return StreamKind::Blocking;
}

RunningReadStop runningReadStop() noexcept
{
    return RunningReadStop::Cancels;
}

} // namespace core::net::streams_test
