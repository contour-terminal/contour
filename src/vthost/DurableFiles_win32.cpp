// SPDX-License-Identifier: Apache-2.0
#ifdef _WIN32

// clang-format off
    #include <winsock2.h>
    #include <windows.h>
// clang-format on

    #include <format>
    #include <memory>
    #include <type_traits>

    #include <vthost/DurableFiles.hpp>

namespace vthost
{

namespace
{
    struct HandleCloser
    {
        void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
    };

    using FileHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;
} // namespace

std::expected<void, std::string> NativeDurableFiles::createExclusive(std::filesystem::path const& path) const
{
    // CREATE_NEW fails on anything already there, a reparse point included.
    auto* const raw =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return std::unexpected(std::format("cannot create {}: error {}", path.string(), GetLastError()));
    CloseHandle(raw);
    return {};
}

std::expected<void, std::string> NativeDurableFiles::syncFile(std::filesystem::path const& path) const
{
    // FlushFileBuffers needs a handle with write access; the sharing lets whoever else holds it keep it.
    auto* const raw = CreateFileW(path.c_str(),
                                  GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (raw == INVALID_HANDLE_VALUE)
        return std::unexpected(std::format("cannot open {}: error {}", path.string(), GetLastError()));
    auto const file = FileHandle { raw };
    if (!FlushFileBuffers(file.get()))
        return std::unexpected(std::format("cannot sync {}: error {}", path.string(), GetLastError()));
    return {};
}

std::expected<void, std::string> NativeDurableFiles::replace(std::filesystem::path const& from,
                                                             std::filesystem::path const& to) const
{
    // MOVEFILE_WRITE_THROUGH returns only once the move is on the disk.
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return std::unexpected(
            std::format("cannot rename {} to {}: error {}", from.string(), to.string(), GetLastError()));
    return {};
}

std::expected<void, std::string> NativeDurableFiles::syncDirectory(
    std::filesystem::path const& /*directory*/) const
{
    return {};
}

} // namespace vthost

#endif
