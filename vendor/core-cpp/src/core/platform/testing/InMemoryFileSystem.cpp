// SPDX-License-Identifier: Apache-2.0
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <core/platform/PathUtils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ios>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <set>
#include <streambuf>
#include <system_error>

namespace core::platform::testing
{

namespace
{
    /// @brief Turns a key of this filesystem back into a path.
    ///
    /// Every key is UTF-8: normalize() spells it through core::platform::normalizePath(), which
    /// goes via generic_u8string() for exactly that reason. std::filesystem::path's narrow
    /// constructor reads a char sequence in the platform's *native* narrow encoding, which on
    /// Windows is the ANSI code page -- so building a path back from a key that way mangles or
    /// throws on any name the code page cannot spell, which is the same defect that keying the
    /// map through generic_string() was. A u8string_view is UTF-8 by definition, so this is the
    /// one way back out of the key space, as normalizePath() is the one way in.
    ///
    /// @param key A normalized key of this filesystem.
    /// @return The path it names.
    [[nodiscard]] std::filesystem::path pathFromKey(std::string_view key)
    {
        return std::filesystem::path { std::u8string_view { reinterpret_cast<char8_t const*>(key.data()),
                                                            key.size() } };
    }

    /// Streambuf that appends into the string an InMemoryFileSystem holds for a file.
    ///
    /// It shares ownership of that string rather than pointing at the map's: remove() and
    /// rename() take the entry away while a stream may still be open on it, and a raw pointer
    /// would dangle. Sharing also models what POSIX does, where an unlinked file stays alive for
    /// whoever still holds it open, and a rename moves the name rather than the contents.
    class MemoryOutputBuf final: public std::streambuf
    {
      public:
        MemoryOutputBuf(std::shared_ptr<std::string> target, WriteMode mode): _target(std::move(target))
        {
            if (mode == WriteMode::Truncate)
                _target->clear();
        }

      protected:
        std::streamsize xsputn(char const* s, std::streamsize n) override
        {
            _target->append(s, static_cast<std::size_t>(n));
            return n;
        }

        int_type overflow(int_type ch) override
        {
            if (!traits_type::eq_int_type(ch, traits_type::eof()))
                _target->push_back(traits_type::to_char_type(ch));
            return ch;
        }

      private:
        std::shared_ptr<std::string> _target;
    };

    // Custom ostream that owns the streambuf.
    class MemoryOStream final: public std::ostream
    {
      public:
        MemoryOStream(std::shared_ptr<std::string> target, WriteMode mode):
            std::ostream(&_buf), _buf(std::move(target), mode)
        {
        }

      private:
        MemoryOutputBuf _buf;
    };

    /// The read half of a stream over a file this filesystem holds.
    ///
    /// Two things keep it honest about a file that changes behind its back, which this one can.
    /// It shares ownership of the string, so the file outlives a remove() or a rename() that
    /// takes the map entry away -- as an open descriptor does on POSIX. And it caches no pointer
    /// into that string at all: the get area is left empty, so every read comes through
    /// underflow(), uflow() or xsgetn() and re-reads where the data is now. A get area spanning
    /// the string would be read directly by sgetc()/sbumpc() without entering this class, so a
    /// reallocation -- which writeFile() causes -- could not be noticed between two reads.
    ///
    /// Reading live rather than from a snapshot is also what the native backend does: a read
    /// descriptor sees writes that land after it was opened.
    class MemoryReadBuf: public std::streambuf
    {
      public:
        explicit MemoryReadBuf(std::shared_ptr<std::string> target): _target(std::move(target)) {}

      protected:
        std::streamsize xsgetn(char* s, std::streamsize n) override
        {
            if (n <= 0)
                return 0;

            // A put-back character is delivered before the file's own bytes.
            auto delivered = std::streamsize { 0 };
            if (_pushedBack.has_value())
            {
                *s = *_pushedBack;
                _pushedBack.reset();
                ++_position;
                ++s;
                ++delivered;
            }

            auto const count = std::min(static_cast<std::size_t>(n - delivered), readable());
            // Guarded rather than relying on data() + size() being formable: nothing may compute
            // a pointer past the end even when it would not be dereferenced.
            if (count != 0)
            {
                std::copy_n(contents().data() + _position, count, s);
                _position += count;
            }
            return delivered + static_cast<std::streamsize>(count);
        }

        int_type underflow() override
        {
            if (_pushedBack.has_value())
                return traits_type::to_int_type(*_pushedBack);
            if (readable() == 0)
                return traits_type::eof();
            return traits_type::to_int_type(contents()[_position]);
        }

        int_type uflow() override
        {
            auto const ch = underflow();
            if (traits_type::eq_int_type(ch, traits_type::eof()))
                return ch;
            _pushedBack.reset();
            ++_position;
            return ch;
        }

        /// @brief Puts a character back, which is where every unget() and putback() lands here.
        ///
        /// The get area is deliberately empty -- see the class comment -- so std::streambuf never
        /// satisfies a put-back itself and always asks this. The default implementation refuses,
        /// which set badbit on every unget() and putback() the fake handed out, where
        /// std::ifstream and std::fstream both succeed.
        ///
        /// @param ch The character to put back, or eof() for unget(), which puts back whatever
        ///           was read.
        /// @return @p ch on success, eof() if there is nothing to put back.
        int_type pbackfail(int_type ch) override
        {
            if (_position == 0)
                return traits_type::eof(); // Nothing has been read, so there is nothing to undo.

            if (traits_type::eq_int_type(ch, traits_type::eof()))
            {
                // unget(): the character at the restored position is the one that was read --
                // from the file. So an unget() *after* a put-back character has been read steps
                // back to the file's byte, where std::filebuf hands the put-back one out again
                // from its own slot. Nothing pins that down: the standard does not describe it,
                // and libc++ refuses the put-back that creates the situation at all. Left as it
                // is and listed in core-cpp#27 rather than chased into one library's internals.
                --_position;
                _pushedBack.reset();
                return traits_type::not_eof(ch);
            }

            auto const byte = traits_type::to_char_type(ch);
            auto const holdsIt = _position - 1 < contents().size() && contents()[_position - 1] == byte;
            // std::filebuf carries one put-back slot, so a second put-back of a character the
            // file does not hold is refused. Refused *before* moving, unlike filebuf, which
            // leaves the position moved back on a failure the standard does not describe.
            if (!holdsIt && _pushedBack.has_value())
                return traits_type::eof();

            --_position;
            // A put-back may name a character the file does not hold. filebuf keeps such a one in
            // a slot of its own and leaves the file alone; so does this, so the next read returns
            // it and the file still reads as it did.
            if (holdsIt)
                _pushedBack.reset();
            else
                _pushedBack = byte;
            return ch;
        }

        std::streamsize showmanyc() override { return static_cast<std::streamsize>(readable()); }

        pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override
        {
            constexpr auto Refused = off_type { -1 };
            auto const size = static_cast<off_type>(contents().size());

            auto anchor = off_type { 0 }; // std::ios_base::beg
            if (dir == std::ios_base::end)
                anchor = size;
            else if (dir == std::ios_base::cur)
                anchor = static_cast<off_type>(_position);

            auto const target = anchor + off;
            if (target < 0 || target > size)
                return { Refused };
            _position = static_cast<std::size_t>(target);
            _pushedBack.reset(); // A seek discards a pending put-back, as it does for a filebuf.
            return { target };
        }

        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override
        {
            return seekoff(off_type(pos), std::ios_base::beg, which);
        }

        /// @return The file's storage, shared with the filesystem and with every other stream
        ///         open on it. Not const: this buffer's own constness says nothing about the
        ///         file's, and the write half below assigns through it.
        [[nodiscard]] std::string& contents() const noexcept { return *_target; }

        /// @return Where the stream stands, for reading as much as for writing.
        [[nodiscard]] std::size_t position() const noexcept { return _position; }

        void setPosition(std::size_t at) noexcept { _position = at; }

        /// @return How many bytes are left from where the stream stands. Recomputed every time,
        ///         because the file can have grown or shrunk since the last call.
        [[nodiscard]] std::size_t readable() const noexcept
        {
            return contents().size() > _position ? contents().size() - _position : 0;
        }

        /// Drops a pending put-back, for a write that overwrites where it sat.
        void discardPutback() noexcept { _pushedBack.reset(); }

      private:
        std::shared_ptr<std::string> _target;
        std::size_t _position = 0;
        /// A character put back that the file does not hold, waiting to be read. std::filebuf
        /// carries exactly one such slot, and so does this.
        std::optional<char> _pushedBack;
    };

    class MemoryIStream final: public std::istream
    {
      public:
        explicit MemoryIStream(std::shared_ptr<std::string> target):
            std::istream(&_buf), _buf(std::move(target))
        {
        }

      private:
        MemoryReadBuf _buf;
    };

    /// Combined read-write stream backed by in-memory data.
    ///
    /// The read half above, plus one position shared with the write half, as std::filebuf has: a
    /// write overwrites from wherever the stream stands and extends the file only past its end.
    class MemoryIOBuf final: public MemoryReadBuf
    {
      public:
        using MemoryReadBuf::MemoryReadBuf;

      protected:
        std::streamsize xsputn(char const* s, std::streamsize n) override
        {
            auto const count = static_cast<std::size_t>(n);
            auto const at = position();
            if (at + count > contents().size())
                contents().resize(at + count);
            contents().replace(at, count, s, count);
            setPosition(at + count);
            discardPutback(); // The write landed where a put-back would have been read from.
            return n;
        }

        int_type overflow(int_type ch) override
        {
            if (traits_type::eq_int_type(ch, traits_type::eof()))
                return traits_type::not_eof(ch);
            auto const byte = traits_type::to_char_type(ch);
            return xsputn(&byte, 1) == 1 ? ch : traits_type::eof();
        }
    };

    class MemoryIOStream final: public std::iostream
    {
      public:
        explicit MemoryIOStream(std::shared_ptr<std::string> target):
            std::iostream(&_buf), _buf(std::move(target))
        {
        }

      private:
        MemoryIOBuf _buf;
    };
} // namespace

InMemoryFileSystem::InMemoryFileSystem(std::initializer_list<FileEntry> entries)
{
    for (auto const& entry: entries)
    {
        auto perms = entry.perms;
        if (entry.isExecutable)
            perms |= std::filesystem::perms::owner_exec;

        if (entry.isDirectory)
        {
            addDirectory(entry.path);
        }
        else if (entry.isSymlink)
        {
            addSymlink(entry.path, pathFromKey(entry.content));
        }
        else
        {
            addFile(entry.path, entry.content, perms);
        }
    }
}

std::string InMemoryFileSystem::normalize(std::filesystem::path const& path) const
{
    // stripTrailingSeparator() normalizes, spells the result with forward slashes for
    // cross-platform consistency, and drops the trailing separator lexically_normal()
    // leaves behind for a final "." or ".." -- without which "/test/." would never match
    // the "/test" held in the directory set.
    auto result = platform::stripTrailingSeparator(path.is_relative() ? _currentPath / path : path);
    // generic_string() only folds separators the host platform recognises, so a Windows-style
    // fixture path keeps its backslashes on POSIX and would never match a key spelled with
    // forward slashes. Fold them unconditionally: this filesystem is keyed by spelling.
    std::ranges::replace(result, '\\', '/');
    return result;
}

void InMemoryFileSystem::ensureParentDirectories(std::filesystem::path const& path) const
{
    auto p = pathFromKey(normalize(path)).parent_path();
    while (!p.empty() && p != p.root_path())
    {
        _directories.insert(normalizePath(p));
        p = p.parent_path();
    }
    if (!p.empty())
        _directories.insert(normalizePath(p));
}

std::shared_ptr<std::string>& InMemoryFileSystem::fileAt(std::string const& key) const
{
    // Never replaces the string a key already has: a stream open on this file shares it, and a
    // writer that swapped in a fresh one would leave that stream writing where nobody looks.
    auto& file = _files[key];
    if (!file)
        file = std::make_shared<std::string>();
    return file;
}

std::string InMemoryFileSystem::resolveSymlinks(std::string key) const
{
    // Bounded so a cycle terminates; the real filesystem answers ELOOP, and every caller here
    // treats "not a file and not a directory" the same way it would treat that.
    constexpr auto MaxHops = 32;
    for ([[maybe_unused]] auto const hop: std::views::iota(0, MaxHops))
    {
        auto const it = _symlinks.find(key);
        if (it == _symlinks.end())
            return key;
        key = normalize(pathFromKey(it->second));
    }
    return key;
}

bool InMemoryFileSystem::exists(std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    return _files.contains(key) || _directories.contains(key) || _symlinks.contains(key);
}

bool InMemoryFileSystem::isDirectory(std::filesystem::path const& path) const
{
    return _directories.contains(resolveSymlinks(normalize(path)));
}

bool InMemoryFileSystem::isRegularFile(std::filesystem::path const& path) const
{
    return _files.contains(resolveSymlinks(normalize(path)));
}

bool InMemoryFileSystem::isSymlink(std::filesystem::path const& path) const
{
    return _symlinks.contains(normalize(path));
}

bool InMemoryFileSystem::isExecutableFile(std::filesystem::path const& path) const
{
    // Through the link, as the native backend's followed status is: a link to an executable file is
    // executable, a link to a directory or to nothing is not, and the bit is the target's.
    auto const key = resolveSymlinks(normalize(path));
    if (!_files.contains(key))
        return false;

    auto const it = _permissions.find(key);
    auto const perms = it != _permissions.end()
                           ? it->second
                           : std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    return (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none
           || (perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none
           || (perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none;
}

std::filesystem::path InMemoryFileSystem::weaklyCanonical(std::filesystem::path const& path) const
{
    return pathFromKey(normalize(path));
}

std::filesystem::path InMemoryFileSystem::currentPath() const
{
    return _currentPath;
}

std::expected<std::string, std::string> InMemoryFileSystem::readFile(std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    if (_deniedPaths.contains(key))
        return std::unexpected(std::format("Permission denied: {}", key));
    if (auto const it = _files.find(key); it != _files.end())
        return *it->second;
    return std::unexpected(std::format("File not found: {}", key));
}

std::expected<void, std::string> InMemoryFileSystem::writeFile(std::filesystem::path const& path,
                                                               std::string_view content) const
{
    auto const key = normalize(path);
    if (_deniedPaths.contains(key))
        return std::unexpected(std::format("Permission denied: {}", key));
    ensureParentDirectories(path);
    *fileAt(key) = std::string(content);
    return {};
}

std::expected<void, std::string> InMemoryFileSystem::appendFile(std::filesystem::path const& path,
                                                                std::string_view content) const
{
    auto const key = normalize(path);
    if (_deniedPaths.contains(key))
        return std::unexpected(std::format("Permission denied: {}", key));
    ensureParentDirectories(path);
    fileAt(key)->append(content);
    return {};
}

std::optional<std::string> InMemoryFileSystem::refuseOpen(std::string const& key) const
{
    if (_deniedPaths.contains(key))
        return "Permission denied";
    if (_directories.contains(key))
        return "Is a directory";
    return std::nullopt;
}

std::expected<std::unique_ptr<std::istream>, std::string> InMemoryFileSystem::openRead(
    std::filesystem::path const& path) const
{
    auto const key = resolveSymlinks(normalize(path));
    if (auto refusal = refuseOpen(key))
        return std::unexpected(std::move(*refusal));
    auto const it = _files.find(key);
    if (it == _files.end())
        return std::unexpected("No such file or directory");
    return std::make_unique<MemoryIStream>(it->second);
}

std::expected<std::unique_ptr<std::ostream>, std::string> InMemoryFileSystem::openWrite(
    std::filesystem::path const& path, WriteMode mode) const
{
    auto const key = normalize(path);
    if (auto refusal = refuseOpen(key))
        return std::unexpected(std::move(*refusal));
    ensureParentDirectories(path);
    return std::make_unique<MemoryOStream>(fileAt(key), mode);
}

std::expected<std::unique_ptr<std::iostream>, std::string> InMemoryFileSystem::openReadWrite(
    std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    if (auto refusal = refuseOpen(key))
        return std::unexpected(std::move(*refusal));
    ensureParentDirectories(path);
    return std::make_unique<MemoryIOStream>(fileAt(key));
}

std::expected<void, std::string> InMemoryFileSystem::createDirectory(std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    if (_deniedPaths.contains(key))
        return std::unexpected(std::format("Permission denied: {}", key));
    // Whatever is there already -- a directory, a file, a link -- is refused as the native backend
    // refuses it, with the operating system's "File exists".
    if (_directories.contains(key) || _files.contains(key) || _symlinks.contains(key))
        return std::unexpected(std::format(
            "Cannot create directory '{}': {}", key, std::make_error_code(std::errc::file_exists).message()));
    // Check that parent exists
    auto const parent = normalizePath(pathFromKey(key).parent_path());
    if (!parent.empty() && parent != "/" && !_directories.contains(parent))
        return std::unexpected(std::format("No such file or directory: {}", parent));
    _directories.insert(key);
    return {};
}

std::expected<void, std::string> InMemoryFileSystem::createDirectories(
    std::filesystem::path const& path) const
{
    auto p = pathFromKey(normalize(path));
    while (!p.empty() && p != p.root_path())
    {
        _directories.insert(normalizePath(p));
        p = p.parent_path();
    }
    if (!p.empty())
        _directories.insert(normalizePath(p));
    return {};
}

std::expected<bool, std::string> InMemoryFileSystem::remove(std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    if (_deniedPaths.contains(key))
        return std::unexpected(std::format("Permission denied: {}", key));
    auto const fileErased = _files.erase(key) > 0;
    auto const dirErased = _directories.erase(key) > 0;
    auto const symlinkErased = _symlinks.erase(key) > 0;
    _permissions.erase(key);
    return fileErased || dirErased || symlinkErased;
}

std::expected<std::uintmax_t, std::string> InMemoryFileSystem::removeAll(
    std::filesystem::path const& path) const
{
    auto const prefix = normalize(path);
    if (_deniedPaths.contains(prefix))
        return std::unexpected(std::format("Permission denied: {}", prefix));
    auto const isUnder = [&prefix, nested = prefix + "/"](std::string const& key) {
        return key == prefix || key.starts_with(nested);
    };
    auto const keyIsUnder = [&isUnder](auto const& entry) {
        return isUnder(entry.first);
    };

    // Everything under this path: files, directories and symlinks.
    auto count = std::uintmax_t { 0 };
    count += std::erase_if(_files, keyIsUnder);
    count += std::erase_if(_directories, isUnder);
    count += std::erase_if(_symlinks, keyIsUnder);
    return count;
}

std::expected<void, std::string> InMemoryFileSystem::copyFile(std::filesystem::path const& from,
                                                              std::filesystem::path const& to,
                                                              OverwritePolicy policy) const
{
    auto const srcKey = normalize(from);
    auto const dstKey = normalize(to);

    auto const it = _files.find(srcKey);
    if (it == _files.end())
        return std::unexpected(std::format("Source file not found: {}", srcKey));

    if (policy == OverwritePolicy::Refuse && _files.contains(dstKey))
        return std::unexpected(std::format("Destination already exists: {}", dstKey));

    ensureParentDirectories(to);
    // Through fileAt(), so a stream open on the destination sees the copy rather than being
    // detached from it -- which is what overwriting a file in place does.
    *fileAt(dstKey) = *it->second;
    return {};
}

std::expected<void, std::string> InMemoryFileSystem::rename(std::filesystem::path const& from,
                                                            std::filesystem::path const& to) const
{
    auto const srcKey = normalize(from);
    auto const dstKey = normalize(to);

    if (auto const it = _files.find(srcKey); it != _files.end())
    {
        ensureParentDirectories(to);
        _files[dstKey] = std::move(it->second);
        _files.erase(it);
        return {};
    }

    if (_directories.contains(srcKey))
    {
        auto const srcPrefix = srcKey.ends_with('/') ? srcKey : srcKey + "/";
        auto const dstPrefix = dstKey.ends_with('/') ? dstKey : dstKey + "/";

        // Move nested files
        auto filesToMove = std::vector<std::pair<std::string, std::string>> {};
        for (auto const& [filePath, content]: _files)
        {
            if (filePath.starts_with(srcPrefix))
                filesToMove.emplace_back(filePath, dstPrefix + filePath.substr(srcPrefix.size()));
        }
        for (auto const& [oldPath, newPath]: filesToMove)
        {
            _files[newPath] = std::move(_files[oldPath]);
            _files.erase(oldPath);
        }

        // Move nested directories
        auto dirsToMove = std::vector<std::pair<std::string, std::string>> {};
        for (auto const& dirPath: _directories)
        {
            if (dirPath.starts_with(srcPrefix))
                dirsToMove.emplace_back(dirPath, dstPrefix + dirPath.substr(srcPrefix.size()));
        }
        for (auto const& [oldPath, newPath]: dirsToMove)
        {
            _directories.erase(oldPath);
            _directories.insert(newPath);
        }

        // Move the directory itself
        _directories.erase(srcKey);
        _directories.insert(dstKey);
        ensureParentDirectories(to);
        return {};
    }

    return std::unexpected(std::format("Source not found: {}", srcKey));
}

std::expected<std::vector<FileSystem::DirectoryEntry>, std::string> InMemoryFileSystem::listDirectory(
    std::filesystem::path const& path) const
{
    auto const dirKey = normalize(path);
    if (!_directories.contains(dirKey))
        return std::unexpected(std::format("Not a directory: {}", dirKey));

    auto const prefix = dirKey.ends_with('/') ? dirKey : dirKey + "/";
    auto entries = std::vector<DirectoryEntry> {};

    // Collect direct children (files)
    for (auto const& [filePath, _]: _files)
    {
        if (!filePath.starts_with(prefix))
            continue;
        auto const rest = std::string_view(filePath).substr(prefix.size());
        if (rest.contains('/'))
            continue; // deeper than one level
        entries.push_back(DirectoryEntry {
            .path = pathFromKey(filePath),
            .isDirectory = false,
            .isRegularFile = true,
            .isSymlink = _symlinks.contains(filePath),
        });
    }

    // Collect direct children (directories)
    for (auto const& dirPath: _directories)
    {
        if (!dirPath.starts_with(prefix))
            continue;
        auto const rest = std::string_view(dirPath).substr(prefix.size());
        if (rest.empty() || rest.contains('/'))
            continue;
        entries.push_back(DirectoryEntry {
            .path = pathFromKey(dirPath),
            .isDirectory = true,
            .isRegularFile = false,
            .isSymlink = _symlinks.contains(dirPath),
        });
    }

    // Collect symlink-only entries (not already in _files or _directories)
    for (auto const& [symlinkPath, _]: _symlinks)
    {
        if (!symlinkPath.starts_with(prefix))
            continue;
        auto const rest = std::string_view(symlinkPath).substr(prefix.size());
        if (rest.contains('/'))
            continue;
        if (!_files.contains(symlinkPath) && !_directories.contains(symlinkPath))
        {
            entries.push_back(DirectoryEntry {
                .path = pathFromKey(symlinkPath),
                .isDirectory = false,
                .isRegularFile = false,
                .isSymlink = true,
            });
        }
    }

    return entries;
}

Generator<FileSystem::DirectoryEntry> InMemoryFileSystem::walkDirectoryRecursive(
    std::filesystem::path path, std::error_code* outError) const
{
    auto const dirKey = normalize(path);
    if (!_directories.contains(dirKey))
    {
        if (outError != nullptr)
            *outError = std::make_error_code(std::errc::not_a_directory);
        co_return; // Non-directory root yields nothing (callers pre-check, like NativeFileSystem).
    }

    auto const prefix = dirKey.ends_with('/') ? dirKey : dirKey + "/";

    // Depth below the root, counted from the normalized key: a direct child of the root
    // ("/root/a") has one path component after the prefix and depth 1, matching
    // NativeFileSystem's recursive_directory_iterator::depth() + 1 convention.
    auto const depthOf = [&prefix](std::string const& key) {
        return static_cast<int>(std::ranges::count(key.substr(prefix.size()), '/')) + 1;
    };

    // Gather every entry under the prefix, then yield in sorted path order so that
    // a directory always precedes its own contents ("a/b" < "a/b/c"), matching
    // NativeFileSystem's pre-order recursive_directory_iterator. (The maps are
    // sorted individually, but files-then-dirs-then-symlinks would otherwise break
    // the parents-before-children ordering that cp/rm rely on.)
    auto entries = std::vector<DirectoryEntry> {};
    for (auto const& [filePath, _]: _files)
        if (filePath.starts_with(prefix))
            entries.push_back({ .path = pathFromKey(filePath),
                                .isDirectory = false,
                                .isRegularFile = true,
                                .isSymlink = _symlinks.contains(filePath),
                                .depth = depthOf(filePath) });
    for (auto const& dirPath: _directories)
        if (dirPath.starts_with(prefix))
            entries.push_back({ .path = pathFromKey(dirPath),
                                .isDirectory = true,
                                .isRegularFile = false,
                                .isSymlink = _symlinks.contains(dirPath),
                                .depth = depthOf(dirPath) });
    for (auto const& [symlinkPath, _]: _symlinks)
        if (symlinkPath.starts_with(prefix) && !_files.contains(symlinkPath)
            && !_directories.contains(symlinkPath))
            entries.push_back({ .path = pathFromKey(symlinkPath),
                                .isDirectory = false,
                                .isRegularFile = false,
                                .isSymlink = true,
                                .depth = depthOf(symlinkPath) });

    std::ranges::sort(
        entries, std::ranges::less {}, [](DirectoryEntry const& e) { return normalizePath(e.path); });

    for (auto const& entry: entries)
        co_yield entry;
}

std::expected<std::uintmax_t, std::string> InMemoryFileSystem::fileSize(
    std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    auto const it = _files.find(key);
    if (it == _files.end())
        return std::unexpected(std::format("File not found: {}", key));
    return static_cast<std::uintmax_t>(it->second->size());
}

std::expected<std::filesystem::file_time_type, std::string> InMemoryFileSystem::lastWriteTime(
    std::filesystem::path const& path) const
{
    auto const key = normalize(path);
    if (!_files.contains(key) && !_directories.contains(key))
        return std::unexpected(std::format("Path not found: {}", key));
    return std::filesystem::file_time_type::clock::now();
}

std::expected<std::filesystem::perms, std::string> InMemoryFileSystem::permissions(
    std::filesystem::path const& path) const
{
    // Through a link, as `std::filesystem::status` is: the permissions are the target's.
    auto const key = resolveSymlinks(normalize(path));
    if (!_files.contains(key) && !_directories.contains(key))
        return std::unexpected(std::format("Path not found: {}", key));
    if (auto const it = _permissions.find(key); it != _permissions.end())
        return it->second;
    return std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
}

std::expected<void, std::string> InMemoryFileSystem::setPermissions(std::filesystem::path const& path,
                                                                    std::filesystem::perms perms) const
{
    // Onto the target, as `std::filesystem::permissions` follows a link; a dangling one has none.
    auto const key = resolveSymlinks(normalize(path));
    if (!_files.contains(key) && !_directories.contains(key))
        return std::unexpected(std::format("Path not found: {}", key));
    _permissions[key] = perms;
    return {};
}

std::expected<std::filesystem::path, std::string> InMemoryFileSystem::createTempFile(
    std::string_view prefix) const
{
    auto const name = std::format("/tmp/{}_{}", prefix, ++_tempCounter);
    fileAt(name)->clear();
    ensureParentDirectories(pathFromKey(name));
    return pathFromKey(name);
}

void InMemoryFileSystem::setCurrentPath(std::filesystem::path const& path)
{
    // Normalized, like every other key: an unnormalized one (a trailing separator, or the
    // backslashes operator/ inserts on Windows) would name a directory no lookup can ever
    // find, since every query spells its key through normalize(). Normalizing first also
    // resolves a relative path against the previous working directory, as chdir() does.
    auto key = normalize(path);
    _currentPath = pathFromKey(key);
    _directories.insert(std::move(key));
}

void InMemoryFileSystem::addFile(std::filesystem::path const& path,
                                 std::string content,
                                 std::filesystem::perms perms)
{
    auto const key = normalize(path);
    *fileAt(key) = std::move(content);
    _permissions[key] = perms;
    ensureParentDirectories(path);
}

void InMemoryFileSystem::addExecutable(std::filesystem::path const& path, std::string content)
{
    addFile(path,
            std::move(content),
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
                | std::filesystem::perms::owner_exec);
}

void InMemoryFileSystem::addDirectory(std::filesystem::path const& path)
{
    auto p = pathFromKey(normalize(path));
    while (!p.empty() && p != p.root_path())
    {
        _directories.insert(normalizePath(p));
        p = p.parent_path();
    }
    if (!p.empty())
        _directories.insert(normalizePath(p));
}

void InMemoryFileSystem::addSymlink(std::filesystem::path const& path, std::filesystem::path const& target)
{
    auto const key = normalize(path);
    _symlinks[key] = normalizePath(target);
    ensureParentDirectories(path);
}

void InMemoryFileSystem::denyAccess(std::filesystem::path const& path)
{
    _deniedPaths.insert(normalize(path));
    _permissions[normalize(path)] = std::filesystem::perms::none;
}

} // namespace core::platform::testing
