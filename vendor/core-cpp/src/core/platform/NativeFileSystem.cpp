// SPDX-License-Identifier: Apache-2.0
#include <core/platform/NativeFileSystem.hpp>

#include <core/platform/PathUtils.hpp>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>

#ifndef _WIN32
    #include <unistd.h>
#else
    #include <io.h>
    #include <windows.h>
#endif

namespace fs = std::filesystem;

namespace core::platform
{

namespace
{
    /// @param path A path to name in an error message.
    /// @return @p path in UTF-8, in its native format. Not `path::string()`, which on Windows
    ///         narrows through the ANSI code page: a name the code page cannot hold was mangled,
    ///         and MSVC's conversion throws there, so the error path itself threw (core-cpp#26).
    [[nodiscard]] std::string forMessage(fs::path const& path)
    {
        auto const spelled = path.u8string();
        return std::string { reinterpret_cast<char const*>(spelled.data()), spelled.size() };
    }
} // namespace

RenameFunction nativeRename()
{
    return [](fs::path const& from, fs::path const& to, std::error_code& ec) {
        fs::rename(from, to, ec);
    };
}

NativeFileSystem::NativeFileSystem(RenameFunction rename): _rename { std::move(rename) }
{
}

NativeFileSystem& NativeFileSystem::instance()
{
    static NativeFileSystem instance;
    return instance;
}

bool NativeFileSystem::exists(fs::path const& path) const
{
    std::error_code ec;
    return fs::exists(path, ec);
}

bool NativeFileSystem::isDirectory(fs::path const& path) const
{
    std::error_code ec;
    return fs::is_directory(path, ec);
}

bool NativeFileSystem::isRegularFile(fs::path const& path) const
{
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool NativeFileSystem::isSymlink(fs::path const& path) const
{
    std::error_code ec;
    return fs::is_symlink(path, ec);
}

bool NativeFileSystem::isExecutableFile(fs::path const& path) const
{
#ifdef _WIN32
    // GetFileAttributesW does not follow reparse points, so it succeeds for App Execution
    // Alias entries (winget, Store python, …) that CreateFileW/std::filesystem cannot open.
    // Any existing non-directory entry is a runnable candidate; PATHEXT already restricts
    // bare-name search to executable extensions, matching cmd.exe / PowerShell semantics.
    auto const attrs = GetFileAttributesW(path.wstring().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    // status() follows symlinks, which is what a PATH lookup needs: a symlink to an executable
    // file is executable, a symlink to a directory is a directory, and a dangling one is nothing.
    // Testing is_symlink() first and then reading the followed status let a symlink to a
    // directory through on the directory's own execute bit -- the one that makes it searchable --
    // against this function's documented "Directories always return false". A caller that
    // trusted it ran the directory, got EACCES and never tried the next PATH entry.
    std::error_code ec;
    auto const status = fs::status(path, ec);
    if (ec || !fs::is_regular_file(status))
        return false;

    auto const perms = status.permissions();
    return (perms & fs::perms::owner_exec) != fs::perms::none
           || (perms & fs::perms::group_exec) != fs::perms::none
           || (perms & fs::perms::others_exec) != fs::perms::none;
#endif
}

fs::path NativeFileSystem::weaklyCanonical(fs::path const& path) const
{
    std::error_code ec;
    auto result = fs::weakly_canonical(path, ec);
    if (ec)
        return path;
    return result;
}

fs::path NativeFileSystem::currentPath() const
{
    return fs::current_path();
}

std::expected<std::string, std::string> NativeFileSystem::readFile(fs::path const& path) const
{
    auto ifs = std::ifstream(path, std::ios::binary);
    if (!ifs)
        return std::unexpected(std::format("Cannot open file: {}", forMessage(path)));

    auto oss = std::ostringstream {};
    oss << ifs.rdbuf();
    if (ifs.bad())
        return std::unexpected(std::format("Error reading file: {}", forMessage(path)));
    return oss.str();
}

std::expected<void, std::string> NativeFileSystem::writeFile(fs::path const& path,
                                                             std::string_view content) const
{
    auto ofs = std::ofstream(path, std::ios::binary | std::ios::trunc);
    if (!ofs)
        return std::unexpected(std::format("Cannot open file for writing: {}", forMessage(path)));

    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs)
        return std::unexpected(std::format("Error writing file: {}", forMessage(path)));
    return {};
}

std::expected<void, std::string> NativeFileSystem::appendFile(fs::path const& path,
                                                              std::string_view content) const
{
    auto ofs = std::ofstream(path, std::ios::binary | std::ios::app);
    if (!ofs)
        return std::unexpected(std::format("Cannot open file for appending: {}", forMessage(path)));

    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!ofs)
        return std::unexpected(std::format("Error appending to file: {}", forMessage(path)));
    return {};
}

namespace
{
    /// @brief Explains a failed stream open in the operating system's own words.
    ///
    /// The stream classes report only *that* the open failed, so the reason comes from errno,
    /// which the open() underlying every standard library this project targets sets. Callers
    /// previously inferred the reason from a follow-up exists() probe, which both misreported
    /// every cause other than the two it guessed between and raced with the open.
    ///
    /// @param errorNumber The value of errno immediately after the failed open.
    /// @return The reason, or a generic message when errno says nothing useful.
    [[nodiscard]] std::string describeOpenFailure(int errorNumber)
    {
        if (errorNumber == 0)
            return "Cannot open file";
        return std::error_code(errorNumber, std::generic_category()).message();
    }

    /// @brief Rejects a directory before it is opened as a file.
    ///
    /// Opening a directory succeeds on POSIX and fails only on the first read, which would
    /// otherwise be indistinguishable from an empty file.
    ///
    /// @param path The path about to be opened.
    /// @return The reason to refuse, or nullopt to proceed.
    [[nodiscard]] std::optional<std::string> refuseDirectory(fs::path const& path)
    {
        std::error_code ec;
        if (fs::is_directory(path, ec))
            return "Is a directory";
        return std::nullopt;
    }

    /// @brief Opens a stream, reporting why rather than merely that it failed.
    ///
    /// @tparam StreamT The concrete stream to construct.
    /// @tparam BaseT   The interface type to hand back.
    /// @param path The file to open.
    /// @param mode The open mode.
    /// @return The stream, or the reason it could not be opened.
    template <typename StreamT, typename BaseT>
    [[nodiscard]] std::expected<std::unique_ptr<BaseT>, std::string> openStream(fs::path const& path,
                                                                                std::ios::openmode mode)
    {
        if (auto refusal = refuseDirectory(path))
            return std::unexpected(std::move(*refusal));

        errno = 0;
        auto stream = std::make_unique<StreamT>(path, mode);
        if (!*stream)
            return std::unexpected(describeOpenFailure(errno));
        return stream;
    }
} // namespace

std::expected<std::unique_ptr<std::istream>, std::string> NativeFileSystem::openRead(
    fs::path const& path) const
{
    return openStream<std::ifstream, std::istream>(path, std::ios::binary);
}

std::expected<std::unique_ptr<std::ostream>, std::string> NativeFileSystem::openWrite(fs::path const& path,
                                                                                      WriteMode mode) const
{
    return openStream<std::ofstream, std::ostream>(
        path, std::ios::binary | (mode == WriteMode::Append ? std::ios::app : std::ios::trunc));
}

std::expected<std::unique_ptr<std::iostream>, std::string> NativeFileSystem::openReadWrite(
    fs::path const& path) const
{
    return openStream<std::fstream, std::iostream>(path, std::ios::binary | std::ios::in | std::ios::out);
}

std::expected<void, std::string> NativeFileSystem::createDirectory(fs::path const& path) const
{
    std::error_code ec;
    if (fs::create_directory(path, ec))
        return {};
    // create_directory() answering false with no error means one thing: the directory is already
    // there. Naming that "No such file or directory" -- the operating system's diagnosis for the
    // other way this fails, a missing parent -- sends a caller looking in the wrong place.
    if (!ec)
        ec = std::make_error_code(std::errc::file_exists);
    return std::unexpected(std::format("Cannot create directory '{}': {}", forMessage(path), ec.message()));
}

std::expected<void, std::string> NativeFileSystem::createDirectories(fs::path const& path) const
{
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec)
        return std::unexpected(
            std::format("Cannot create directories '{}': {}", forMessage(path), ec.message()));
    return {};
}

std::expected<bool, std::string> NativeFileSystem::remove(fs::path const& path) const
{
    std::error_code ec;
    auto const result = fs::remove(path, ec);
    if (ec)
        return std::unexpected(std::format("Cannot remove '{}': {}", forMessage(path), ec.message()));
    return result;
}

std::expected<std::uintmax_t, std::string> NativeFileSystem::removeAll(fs::path const& path) const
{
    std::error_code ec;
    auto const result = fs::remove_all(path, ec);
    if (ec)
        return std::unexpected(std::format("Cannot remove '{}': {}", forMessage(path), ec.message()));
    return result;
}

std::expected<void, std::string> NativeFileSystem::copyFile(fs::path const& from,
                                                            fs::path const& to,
                                                            OverwritePolicy policy) const
{
    std::error_code ec;
    auto const opts =
        policy == OverwritePolicy::Replace ? fs::copy_options::overwrite_existing : fs::copy_options::none;
    fs::copy_file(from, to, opts, ec);
    if (ec)
        return std::unexpected(
            std::format("Cannot copy '{}' to '{}': {}", forMessage(from), forMessage(to), ec.message()));
    return {};
}

namespace
{

    /// What a two-hop recase ended up doing.
    struct RecaseResult
    {
        bool renamed = false;     ///< The entry carries the requested spelling now.
        std::error_code error {}; ///< Why it does not, when it does not.
        /// Where the entry was left when the second hop failed and the rollback failed too.
        /// Empty otherwise. Nothing else would say, and the name is not one a caller can guess.
        fs::path stranded {};
    };

    /// Renames @p from to @p to in two hops via a unique temporary name in the
    /// destination's parent directory.
    ///
    /// Used to perform a lettercase-only rename (e.g. `foo` -> `Foo`) on case-insensitive
    /// filesystems, where a direct rename is rejected because source and destination
    /// resolve to the same entry. A free temporary name is found in the destination's
    /// directory, the entry is moved there, then moved on to its final name. If the second
    /// hop fails the entry is rolled back to its original name so it is never stranded under
    /// the temporary.
    ///
    /// @param from The existing source entry.
    /// @param to The desired destination, in the same directory and differing only in case.
    /// @param rename The primitive to rename through, injected so this path can be tested; see
    ///               NativeFileSystem's constructor.
    /// @return What happened, including the reason the operation aborted and, where it applies,
    ///         the temporary name the entry was left under.
    [[nodiscard]] RecaseResult renameViaTemporary(fs::path const& from,
                                                  fs::path const& to,
                                                  RenameFunction const& rename)
    {
        // The temporary name is built in UTF-8, not through path::string(): that narrows to the
        // ANSI code page on Windows, and a mangled candidate would rename the entry to a name
        // nobody asked for rather than merely misreport one.
        auto const parent = to.parent_path();
        auto const baseName = to.filename().u8string();
        auto ec = std::error_code {};
        for (auto const attempt: std::views::iota(0, 1000))
        {
            auto const digits = std::to_string(attempt);
            auto const candidate =
                parent / fs::path(baseName + u8".recase-" + std::u8string(digits.begin(), digits.end()));
            if (fs::exists(candidate, ec))
                continue;
            ec.clear();

            rename(from, candidate, ec);
            if (ec)
                return { .error = ec };

            rename(candidate, to, ec);
            if (!ec)
                return { .renamed = true };

            // Second hop failed: restore the original name so the entry is not stranded
            // under the temporary. The reported reason stays this hop's, and when the rollback
            // fails too, so does where the entry actually ended up.
            auto rollbackError = std::error_code {};
            auto const& originalName = from; // Named, so the argument order does not read as swapped.
            rename(candidate, originalName, rollbackError);
            return { .error = ec, .stranded = rollbackError ? candidate : fs::path {} };
        }
        return { .error = std::make_error_code(std::errc::file_exists) };
    }

} // namespace

std::expected<void, std::string> NativeFileSystem::rename(fs::path const& from, fs::path const& to) const
{
    std::error_code ec;
    _rename(from, to, ec);
    if (!ec)
        return {};

    // On case-insensitive filesystems (Windows, the default macOS volume format) a
    // rename that changes only the lettercase of the final path component — e.g.
    // `foo` -> `Foo` — can be rejected because source and destination resolve to the
    // same entry. Perform such a recase in two hops through a temporary name so the
    // lettercase change still takes effect.
    if (isCaseOnlyRename(from, to))
    {
        // The retry's own reason, not the first attempt's: the two fail for different things --
        // a direct case-only rename is refused because the two names resolve to one entry, while
        // the retry fails over the temporary name, the destination, or the rollback.
        auto const recase = renameViaTemporary(from, to, _rename);
        if (recase.renamed)
            return {};
        if (!recase.stranded.empty())
            return std::unexpected(std::format("Cannot rename '{}' to '{}': {}; it is now at '{}'",
                                               forMessage(from),
                                               forMessage(to),
                                               recase.error.message(),
                                               forMessage(recase.stranded)));
        return std::unexpected(std::format(
            "Cannot rename '{}' to '{}': {}", forMessage(from), forMessage(to), recase.error.message()));
    }

    return std::unexpected(
        std::format("Cannot rename '{}' to '{}': {}", forMessage(from), forMessage(to), ec.message()));
}

namespace
{
    /// @brief Describes one directory entry without ever throwing.
    ///
    /// The throwing status accessors escape as filesystem_error whenever an entry cannot be
    /// stat'ed -- EACCES on a readable-but-not-searchable directory, or a filesystem that
    /// reports DT_UNKNOWN -- which would abort enumerating an otherwise perfectly readable
    /// directory. An entry whose type cannot be determined is reported as neither.
    ///
    /// @param entry The entry to describe.
    /// @param depth Walk-root-relative depth; 0 for a flat listing.
    /// @return The described entry.
    [[nodiscard]] FileSystem::DirectoryEntry describeEntry(fs::directory_entry const& entry, int depth = 0)
    {
        std::error_code ec;
        return FileSystem::DirectoryEntry {
            .path = entry.path(),
            .isDirectory = entry.is_directory(ec),
            .isRegularFile = entry.is_regular_file(ec),
            .isSymlink = entry.is_symlink(ec),
            .depth = depth,
        };
    }
} // namespace

std::expected<std::vector<FileSystem::DirectoryEntry>, std::string> NativeFileSystem::listDirectory(
    fs::path const& path) const
{
    std::error_code ec;
    auto entries = std::vector<DirectoryEntry> {};
    auto it = fs::directory_iterator(path, ec);
    auto const end = fs::directory_iterator {};
    // Advanced through increment(ec) rather than a range-based for, whose operator++ throws:
    // a mid-walk readdir failure would otherwise escape this std::expected-returning function
    // as a filesystem_error.
    while (!ec && it != end)
    {
        entries.push_back(describeEntry(*it));
        it.increment(ec);
    }
    if (ec)
        return std::unexpected(std::format("Cannot list directory '{}': {}", forMessage(path), ec.message()));
    return entries;
}

Generator<FileSystem::DirectoryEntry> NativeFileSystem::walkDirectoryRecursive(
    fs::path path, std::error_code* outError) const
{
    // recursive_directory_iterator reads directories lazily as it advances, so
    // co_yield-ing per entry streams the walk to the consumer one entry at a
    // time. The error_code overload is used so a bad/unreadable root or a
    // mid-walk error yields nothing/partial rather than throwing out of the
    // coroutine; the error is surfaced via outError (when provided) so
    // destructive callers can avoid acting on an incomplete enumeration.
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    auto const end = fs::recursive_directory_iterator {};
    // Advanced through increment(ec) rather than a range-based for, whose operator++ throws:
    // the error must be threaded out rather than escaping the coroutine.
    while (!ec && it != end)
    {
        // recursive_directory_iterator::depth() is 0 for a direct child; +1 to match the
        // walk-root-relative convention (direct child = depth 1) consumers expect.
        co_yield describeEntry(*it, it.depth() + 1);
        it.increment(ec);
    }
    if (ec && outError != nullptr)
        *outError = ec;
}

std::expected<std::uintmax_t, std::string> NativeFileSystem::fileSize(fs::path const& path) const
{
    std::error_code ec;
    auto const size = fs::file_size(path, ec);
    if (ec)
        return std::unexpected(std::format("Cannot get file size '{}': {}", forMessage(path), ec.message()));
    return size;
}

std::expected<fs::file_time_type, std::string> NativeFileSystem::lastWriteTime(fs::path const& path) const
{
    std::error_code ec;
    auto const time = fs::last_write_time(path, ec);
    if (ec)
        return std::unexpected(
            std::format("Cannot get last write time '{}': {}", forMessage(path), ec.message()));
    return time;
}

std::expected<fs::perms, std::string> NativeFileSystem::permissions(fs::path const& path) const
{
    std::error_code ec;
    auto const status = fs::status(path, ec);
    if (ec)
        return std::unexpected(
            std::format("Cannot get permissions '{}': {}", forMessage(path), ec.message()));
    return status.permissions();
}

std::expected<void, std::string> NativeFileSystem::setPermissions(fs::path const& path, fs::perms perms) const
{
    std::error_code ec;
    fs::permissions(path, perms, ec);
    if (ec)
        return std::unexpected(
            std::format("Cannot set permissions '{}': {}", forMessage(path), ec.message()));
    return {};
}

std::expected<fs::path, std::string> NativeFileSystem::createTempFile(std::string_view prefix) const
{
    // The template is built as a path, not by concatenating path::string(): that narrows to the
    // platform's native narrow encoding, which on Windows is the ANSI code page, and MSVC throws
    // on a temp directory it cannot spell -- a user name outside the code page is enough. The
    // prefix goes in as UTF-8, which is what every path string in core-cpp is.
    auto const leaf = std::u8string(prefix.begin(), prefix.end()) + u8"_XXXXXX";
    auto const templatePath = fs::temp_directory_path() / fs::path(leaf);

#ifdef _WIN32
    // wchar_t end to end, for the same reason: a narrow template handed to _mktemp_s, or to
    // std::ofstream, is read back in the ANSI code page too.
    auto templateStr = templatePath.wstring();
    if (_wmktemp_s(templateStr.data(), templateStr.size() + 1) != 0)
        return std::unexpected("Failed to create temporary file name");
    auto created = fs::path(templateStr);
    auto ofs = std::ofstream(created);
    if (!ofs)
        return std::unexpected("Failed to create temporary file");
    ofs.close();
    return created;
#else
    auto templateStr = templatePath.string();
    auto const fd = mkstemp(templateStr.data());
    if (fd == -1)
        return std::unexpected(std::format("Failed to create temporary file: {}",
                                           std::error_code(errno, std::generic_category()).message()));
    ::close(fd);
    return fs::path(templateStr);
#endif
}

} // namespace core::platform
