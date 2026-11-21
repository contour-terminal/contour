// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentTranscript.hpp>

#include <array>
#include <streambuf>

#include <windows.h>

namespace contour::cli
{

namespace
{
    /// Owns a file handle, closing it once.
    class FileHandle
    {
      public:
        explicit FileHandle(HANDLE handle) noexcept: _handle { handle } {}
        ~FileHandle()
        {
            if (_handle != INVALID_HANDLE_VALUE)
                ::CloseHandle(_handle);
        }
        FileHandle(FileHandle const&) = delete;
        FileHandle& operator=(FileHandle const&) = delete;
        FileHandle(FileHandle&&) = delete;
        FileHandle& operator=(FileHandle&&) = delete;

        [[nodiscard]] HANDLE get() const noexcept { return _handle; }

      private:
        HANDLE _handle;
    };

    /// Reads a disk file's handle through a buffer; a read error marks the stream it serves bad.
    class FileHandleBuffer final: public std::streambuf
    {
      public:
        FileHandleBuffer(HANDLE handle, std::istream& owner) noexcept: _handle { handle }, _owner { &owner }
        {
        }

      protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            auto count = DWORD { 0 };
            if (!::ReadFile(
                    _handle.get(), _buffer.data(), static_cast<DWORD>(_buffer.size()), &count, nullptr))
            {
                _owner->setstate(std::ios_base::badbit);
                return traits_type::eof();
            }
            if (count == 0)
                return traits_type::eof();
            setg(_buffer.data(), _buffer.data(), _buffer.data() + count);
            return traits_type::to_int_type(*gptr());
        }

      private:
        FileHandle _handle;
        std::istream* _owner;
        std::array<char, std::size_t { 64 } * 1024> _buffer {};
    };

    /// A stream that owns its buffer.
    class FileHandleStream final: public std::istream
    {
      public:
        explicit FileHandleStream(HANDLE handle): std::istream { nullptr }, _buffer { handle, *this }
        {
            rdbuf(&_buffer);
        }

      private:
        FileHandleBuffer _buffer;
    };

    class NativeSessionLogOpener final: public ISessionLogOpener
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<std::istream>, TranscriptError> open(
            std::filesystem::path const& log) override
        {
            // FILE_FLAG_OPEN_REPARSE_POINT opens a symbolic link or junction as itself, which the attribute
            // check below then refuses; a pipe or a device is not a disk file.
            auto const handle = ::CreateFileW(log.c_str(),
                                              GENERIC_READ,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                              nullptr,
                                              OPEN_EXISTING,
                                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                              nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return std::unexpected(TranscriptError::Unreadable);
            auto stream = std::make_unique<FileHandleStream>(handle);
            if (::GetFileType(handle) != FILE_TYPE_DISK)
                return std::unexpected(TranscriptError::NotRegular);
            auto information = BY_HANDLE_FILE_INFORMATION {};
            if (!::GetFileInformationByHandle(handle, &information))
                return std::unexpected(TranscriptError::Unreadable);
            if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
                != 0)
                return std::unexpected(TranscriptError::NotRegular);
            return stream;
        }

        [[nodiscard]] bool isLinkedDirectory(std::filesystem::path const& directory) override
        {
            // A junction is a reparse point, not a symbolic link: std::filesystem does not call it one.
            auto const attributes = ::GetFileAttributesW(directory.c_str());
            return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        }
    };
} // namespace

std::unique_ptr<ISessionLogOpener> makeNativeSessionLogOpener()
{
    return std::make_unique<NativeSessionLogOpener>();
}

} // namespace contour::cli
