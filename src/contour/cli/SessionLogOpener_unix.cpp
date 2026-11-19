// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentTranscript.hpp>

#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <streambuf>

#include <fcntl.h>
#include <unistd.h>

namespace contour::cli
{

namespace
{
    /// Owns a file descriptor, closing it once.
    class FileDescriptor
    {
      public:
        explicit FileDescriptor(int fd) noexcept: _fd { fd } {}
        ~FileDescriptor()
        {
            if (_fd >= 0)
                ::close(_fd);
        }
        FileDescriptor(FileDescriptor const&) = delete;
        FileDescriptor& operator=(FileDescriptor const&) = delete;
        FileDescriptor(FileDescriptor&&) = delete;
        FileDescriptor& operator=(FileDescriptor&&) = delete;

        [[nodiscard]] int get() const noexcept { return _fd; }

      private:
        int _fd;
    };

    /// Reads a regular file's descriptor through a buffer; a read error marks the stream it serves bad.
    class FileDescriptorBuffer final: public std::streambuf
    {
      public:
        FileDescriptorBuffer(int fd, std::istream& owner) noexcept: _fd { fd }, _owner { &owner } {}

      protected:
        int_type underflow() override
        {
            if (gptr() < egptr())
                return traits_type::to_int_type(*gptr());
            auto count = ssize_t { -1 };
            do
                count = ::read(_fd.get(), _buffer.data(), _buffer.size());
            while (count < 0 && errno == EINTR);
            if (count < 0)
            {
                _owner->setstate(
                    std::ios_base::badbit); // A read error, not an end: the reader must tell them apart.
                return traits_type::eof();
            }
            if (count == 0)
                return traits_type::eof();
            setg(_buffer.data(), _buffer.data(), _buffer.data() + count);
            return traits_type::to_int_type(*gptr());
        }

      private:
        FileDescriptor _fd;
        std::istream* _owner;
        std::array<char, std::size_t { 64 } * 1024> _buffer {};
    };

    /// A stream that owns its buffer.
    class FileDescriptorStream final: public std::istream
    {
      public:
        explicit FileDescriptorStream(int fd): std::istream { nullptr }, _buffer { fd, *this }
        {
            rdbuf(&_buffer);
        }

      private:
        FileDescriptorBuffer _buffer;
    };

    class NativeSessionLogOpener final: public ISessionLogOpener
    {
      public:
        [[nodiscard]] std::expected<std::unique_ptr<std::istream>, TranscriptError> open(
            std::filesystem::path const& log) override
        {
            // O_NOFOLLOW: a link planted as the log fails (ELOOP) instead of being followed. O_NONBLOCK: a
            // FIFO planted there opens at once instead of waiting for a writer, and fstat then refuses it.
            auto const fd = ::open(log.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                return std::unexpected(errno == ELOOP ? TranscriptError::NotRegular
                                                      : TranscriptError::Unreadable);
            auto stream = std::make_unique<FileDescriptorStream>(fd);
            struct stat status {};
            if (::fstat(fd, &status) != 0)
                return std::unexpected(TranscriptError::Unreadable);
            if (!S_ISREG(status.st_mode))
                return std::unexpected(TranscriptError::NotRegular);
            return stream;
        }

        [[nodiscard]] bool isLinkedDirectory(std::filesystem::path const& directory) override
        {
            struct stat status {};
            return ::lstat(directory.c_str(), &status) != 0 || S_ISLNK(status.st_mode);
        }
    };
} // namespace

std::unique_ptr<ISessionLogOpener> makeNativeSessionLogOpener()
{
    return std::make_unique<NativeSessionLogOpener>();
}

} // namespace contour::cli
