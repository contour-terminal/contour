// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentRecord.hpp>

#include <format>
#include <system_error>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/stat.h>

    #include <cerrno>
    #include <cstring>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace contour::cli
{

namespace
{
    class NativeRecordAppender final: public IRecordAppender
    {
      public:
        [[nodiscard]] std::expected<void, std::string> append(std::filesystem::path const& file,
                                                              std::string_view bytes) override
        {
            auto ec = std::error_code {};
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec)
                return std::unexpected(std::format("{}: {}", file.parent_path().string(), ec.message()));
#ifdef _WIN32
            auto* const handle = ::CreateFileW(file.c_str(),
                                               FILE_APPEND_DATA,
                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                               nullptr,
                                               OPEN_ALWAYS,
                                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                               nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return std::unexpected(std::format("{}: cannot be opened", file.string()));
            auto written = DWORD {};
            auto const ok =
                ::WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0
                && written == bytes.size();
            ::CloseHandle(handle);
            if (!ok)
                return std::unexpected(std::format("{}: cannot be written", file.string()));
            return {};
#else
            auto const fd =
                ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0)
                return std::unexpected(std::format("{}: {}", file.string(), std::strerror(errno)));
            // One write with O_APPEND: lines appended by two writers do not interleave.
            auto const written = ::write(fd, bytes.data(), bytes.size());
            auto const error = errno;
            ::close(fd);
            if (written != static_cast<ssize_t>(bytes.size()))
                return std::unexpected(std::format("{}: {}", file.string(), std::strerror(error)));
            return {};
#endif
        }
    };
} // namespace

std::unique_ptr<IRecordAppender> makeNativeRecordAppender()
{
    return std::make_unique<NativeRecordAppender>();
}

} // namespace contour::cli
