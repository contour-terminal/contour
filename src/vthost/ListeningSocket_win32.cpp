// SPDX-License-Identifier: Apache-2.0

// clang-format off
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #include <winternl.h>
#endif
// clang-format on

#include <vthost/ListeningSocket.hpp>

#ifdef _WIN32

    #include <vtpty/WindowsHandle.hpp>

namespace vthost
{

namespace
{
    /// FILE_COMPLETION_INFORMATION (ntifs.h): the port a file is bound to, and the key its completions carry.
    struct FileCompletionInformation
    {
        HANDLE port;
        void* key;
    };

    /// FileReplaceCompletionInformation, of FILE_INFORMATION_CLASS (ntifs.h): Windows 8.1 and later.
    constexpr auto FileReplaceCompletionInformation = static_cast<FILE_INFORMATION_CLASS>(61);

    using NtSetInformationFileFn =
        LONG(NTAPI*)(HANDLE, IO_STATUS_BLOCK*, void*, ULONG, FILE_INFORMATION_CLASS);
    using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(LONG);
} // namespace

std::expected<void, std::error_code> releaseCompletionPort(std::uint64_t socket)
{
    static auto* const setInformation = vtpty::ntdllFunction<NtSetInformationFileFn>("NtSetInformationFile");
    static auto* const errorOf = vtpty::ntdllFunction<RtlNtStatusToDosErrorFn>("RtlNtStatusToDosError");
    if (setInformation == nullptr || errorOf == nullptr)
        return std::unexpected(std::make_error_code(std::errc::function_not_supported));
    // No port at all: the file is bound to none, and the next one may take it.
    auto information = FileCompletionInformation { .port = nullptr, .key = nullptr };
    auto status = IO_STATUS_BLOCK {};
    auto const result = setInformation(vtpty::handleOf(socket),
                                       &status,
                                       &information,
                                       sizeof(information),
                                       FileReplaceCompletionInformation);
    if (result != 0)
        return std::unexpected(vtpty::errorFrom(errorOf(result)));
    return {};
}

} // namespace vthost

#endif
