// SPDX-License-Identifier: Apache-2.0
#include <core/platform/detail/DurableFiles.hpp>

#include <memory>
#include <type_traits>

#include <windows.h>

namespace core::platform::detail
{

namespace
{
    /// @return The reason the last Windows API call failed.
    [[nodiscard]] std::error_code lastError() noexcept
    {
        return { static_cast<int>(GetLastError()), std::system_category() };
    }

    /// Owns a file handle, so it is closed on every return path.
    using FileHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>;
} // namespace

std::expected<void, std::error_code> createNewFile(std::filesystem::path const& path)
{
    // CREATE_NEW fails on anything already there -- but follows a symbolic link to see what is
    // there, so on its own it creates a dangling link's target. FILE_FLAG_OPEN_REPARSE_POINT makes
    // the link itself what is there.
    auto* const raw = CreateFileW(path.c_str(),
                                  GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return std::unexpected(lastError());
    CloseHandle(raw);
    return {};
}

std::expected<void, std::error_code> syncFile(std::filesystem::path const& path)
{
    // FlushFileBuffers needs a handle with write access; the sharing lets whoever else holds the
    // file keep it open.
    auto* const raw = CreateFileW(path.c_str(),
                                  GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return std::unexpected(lastError());
    auto const file = FileHandle { raw, &CloseHandle };
    if (!FlushFileBuffers(file.get()))
        return std::unexpected(lastError());
    return {};
}

std::expected<void, std::error_code> replaceFile(std::filesystem::path const& from,
                                                 std::filesystem::path const& to)
{
    // MOVEFILE_WRITE_THROUGH returns only once the move is on the disk.
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return std::unexpected(lastError());
    return {};
}

std::expected<void, std::error_code> syncDirectory(std::filesystem::path const& directory)
{
    // A directory cannot be flushed through FlushFileBuffers without backup semantics and write
    // access, and replaceFile() wrote the rename through already. What is still answered is whether
    // the directory is there, so a caller learns of a wrong path on every platform alike.
    auto const attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return std::unexpected(lastError());
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return std::unexpected(std::make_error_code(std::errc::not_a_directory));
    return {};
}

} // namespace core::platform::detail
