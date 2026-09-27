// SPDX-License-Identifier: Apache-2.0
#include <core/platform/windows/WindowsFileInfoProvider.hpp>

#ifdef _WIN32
    #include <core/platform/GlobMatch.hpp>
    #include <core/platform/PathUtils.hpp>

    #include <algorithm>
    #include <chrono>
    #include <filesystem>
    #include <memory>
    #include <string>
    #include <string_view>
    #include <vector>

namespace core::platform
{

namespace
{

    namespace fs = std::filesystem;

    /// Populates a FileEntry from a filesystem directory_entry.
    /// @param dirEntry The entry to inspect.
    /// @param absoluteParent Absolute, normalized directory holding the entry, used to build
    ///                       FileEntry::path. Empty leaves the path empty.
    /// @param fileEntry Output entry to populate.
    /// @return true on success, false if the entry could not be stat'd.
    [[nodiscard]] bool statEntry(fs::directory_entry const& dirEntry,
                                 std::string_view absoluteParent,
                                 FileEntry& fileEntry)
    {
        std::error_code ec;

        fileEntry.name = normalizePath(dirEntry.path().filename());
        if (!absoluteParent.empty())
            fileEntry.path = joinPath(absoluteParent, fileEntry.name);

        // status() follows reparse points. App Execution Aliases (winget, Microsoft Store
        // python, …) are reparse points that cannot be opened or followed through normal
        // filesystem APIs, so the follow fails. Fall back to the non-following
        // symlink_status() so such entries are still listed — matching `cmd.exe` / `dir`.
        auto status = dirEntry.status(ec);
        if (ec)
        {
            ec.clear();
            status = dirEntry.symlink_status(ec);
            if (ec)
                return false;
        }

        fileEntry.isDir = fs::is_directory(status);
        fileEntry.isSymlink = fs::is_symlink(dirEntry.symlink_status(ec));
        ec.clear();
        if (fileEntry.isSymlink)
        {
            // Read the link target verbatim. Reading a reparse point can fail without the
            // required privilege; on failure leave the target empty (graceful degradation).
            auto const target = fs::read_symlink(dirEntry.path(), ec);
            if (!ec)
                fileEntry.symlinkTarget = normalizePath(target);
            ec.clear();
        }
        fileEntry.size = 0;

        // Windows std::filesystem does not expose st_blocks/st_dev/st_ino. Leave the
        // documented sentinels (blocks = -1, dev = ino = 0); disk-usage callers fall
        // back to apparent size and disable hardlink dedup / cross-device detection.
        fileEntry.blocks = -1;
        fileEntry.dev = 0;
        fileEntry.ino = 0;

        if (!fileEntry.isDir)
        {
            auto const fileSize = dirEntry.file_size(ec);
            if (!ec)
                fileEntry.size = static_cast<int64_t>(fileSize);
            else
                ec.clear();
        }

        auto const perms = status.permissions();
        int mode = 0;
        if ((perms & fs::perms::owner_read) != fs::perms::none)
            mode |= 0444;
        if ((perms & fs::perms::owner_write) != fs::perms::none)
            mode |= 0222;
        if ((perms & fs::perms::owner_exec) != fs::perms::none)
            mode |= 0111;
        if (fileEntry.isDir)
            mode |= 0111;
        fileEntry.mode = mode;

        auto const lastWrite = dirEntry.last_write_time(ec);
        if (!ec)
        {
            auto const sctp = std::chrono::clock_cast<std::chrono::system_clock>(lastWrite);
            fileEntry.mtime =
                std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count();
        }
        else
        {
            fileEntry.mtime = 0;
        }

        return true;
    }

} // namespace

std::unique_ptr<FileInfoProvider> nativeFileInfoProvider()
{
    return std::make_unique<WindowsFileInfoProvider>();
}

std::vector<FileEntry> WindowsFileInfoProvider::listDirectory(std::string const& path) const
{
    std::vector<FileEntry> result;
    std::error_code ec;

    // Case 1: Glob pattern — check first (pure string scan, avoids unnecessary syscalls).
    if (containsGlobChars(path))
    {
        auto const patternPath = fs::path(path);
        auto parentDir = patternPath.parent_path();
        // normalizePath(), not path::string(): the latter converts to the ANSI code page and throws
        // on any name it cannot represent, which would take the whole listing down.
        auto const filePattern = normalizePath(patternPath.filename());

        if (parentDir.empty())
            parentDir = ".";

        auto const absoluteParent = absoluteDirectory(parentDir);

        for (auto const& entry: fs::directory_iterator(parentDir, ec))
        {
            if (ec)
                break;

            auto const filename = normalizePath(entry.path().filename());
            if (globMatchFilename(filename, filePattern))
            {
                FileEntry fileEntry {};
                if (statEntry(entry, absoluteParent, fileEntry))
                    result.push_back(std::move(fileEntry));
            }
        }

        std::ranges::sort(result, {}, &FileEntry::name);
        return result;
    }

    // Case 2: Directory path — enumerate contents.
    auto const dir = fs::path(path);
    if (fs::is_directory(dir, ec) && !ec)
    {
        auto const absoluteParent = absoluteDirectory(dir);

        for (auto const& entry: fs::directory_iterator(dir, ec))
        {
            if (ec)
                break;

            FileEntry fileEntry {};
            if (statEntry(entry, absoluteParent, fileEntry))
                result.push_back(std::move(fileEntry));
        }

        std::ranges::sort(result, {}, &FileEntry::name);
        return result;
    }

    // Case 3: Single file path — stat and return one entry.
    if (auto const dirEntry = fs::directory_entry(dir, ec); !ec)
    {
        FileEntry fileEntry {};
        if (statEntry(dirEntry, absoluteDirectory(dir.parent_path()), fileEntry))
            result.push_back(std::move(fileEntry));
    }

    return result;
}

} // namespace core::platform

#endif // _WIN32
