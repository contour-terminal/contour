// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Windows side of @c walkBeneath: a cursor over directory handles, each opened with
/// `NtCreateFile` relative to the one before it, as the reparse point itself, so a directory swapped
/// for a link or a junction between two components is met as a link, never crossed. A symbolic link
/// is followed by the walk only where its target is relative; a junction, a mount point and every
/// other name surrogate is refused (`NotPermitted`). A reparse point that names nothing else (a
/// cloud placeholder, a deduplicated file) is opened again through its filter, by the handle that
/// was checked, so its data is what its filter supplies. Private to the fs module.
///
/// The NT calls that decide all of this are behind @c INtFiles, so the decision is tested with
/// scripted answers as well as against the file system.

#include <core/Random.hpp>
#include <core/async/IExecutor.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/FsExport.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/ReparsePoint.hpp>
#include <core/platform/Clock.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace core::fs::detail
{

/// What `FileAttributeTagInfo` tells about an open object.
struct EntryAttributes
{
    std::uint32_t attributes = 0; ///< `FILE_ATTRIBUTE_*`.
    std::uint32_t reparseTag = 0; ///< The reparse tag, where `FILE_ATTRIBUTE_REPARSE_POINT` is set.

    /// @return Whether the object is a reparse point that names another object: a link to a walk.
    [[nodiscard]] constexpr bool isLink() const noexcept
    {
        return isReparsePoint() && isNameSurrogate(reparseTag);
    }

    /// @return Whether the object is a reparse point that names nothing else.
    [[nodiscard]] constexpr bool isFilteredPoint() const noexcept
    {
        return isReparsePoint() && !isNameSurrogate(reparseTag);
    }

    /// @return Whether the object is a directory (a directory link is one, too).
    [[nodiscard]] constexpr bool isDirectory() const noexcept
    {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    /// @return Whether the object is a reparse point.
    [[nodiscard]] constexpr bool isReparsePoint() const noexcept
    {
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }
};

/// Which object an open handle is: its volume and its file id there.
struct FileIdentity
{
    std::uint64_t volume = 0; ///< The volume's serial number.
    std::uint64_t idLow = 0;  ///< The file id, low 64 bits.
    std::uint64_t idHigh = 0; ///< The file id, high 64 bits (0 where the file system has 64-bit ids).

    bool operator==(FileIdentity const&) const = default;
};

/// The NT calls whose answers decide how a name is opened. Implemented by the system, and by tests
/// with scripted answers.
class INtFiles
{
  public:
    INtFiles() = default;
    INtFiles(INtFiles const&) = delete;
    INtFiles(INtFiles&&) = delete;
    INtFiles& operator=(INtFiles const&) = delete;
    INtFiles& operator=(INtFiles&&) = delete;
    virtual ~INtFiles() = default;

    /// @param directory A held handle @p name is relative to.
    /// @param name One component; empty for @p directory itself.
    /// @param request What to ask for.
    /// @return The handle; what refused it.
    [[nodiscard]] virtual FsResult<UniqueHandle> open(HANDLE directory,
                                                      std::wstring const& name,
                                                      NtOpenRequest const& request) const = 0;

    /// @param handle An open object, with `FILE_READ_ATTRIBUTES`.
    /// @return Its attributes and reparse tag.
    [[nodiscard]] virtual FsResult<EntryAttributes> attributes(HANDLE handle) const = 0;

    /// @param handle An open object.
    /// @return Which object it is.
    [[nodiscard]] virtual FsResult<FileIdentity> identity(HANDLE handle) const = 0;

    /// Reads the next records of @p directory's listing, from where its handle stands
    /// (`GetFileInformationByHandleEx(FileIdBothDirectoryInfo)`).
    /// @param directory A directory, open with `FILE_LIST_DIRECTORY`.
    /// @param records Where the `FILE_ID_BOTH_DIR_INFO` records are written, from its start.
    /// @return Nothing once records are written; what refused it, with its Win32 code kept:
    ///         `ERROR_NO_MORE_FILES` past the last record.
    [[nodiscard]] virtual FsResult<void> queryDirectory(HANDLE directory,
                                                        std::span<std::byte> records) const = 0;
};

/// @return The system's NT calls.
[[nodiscard]] CORE_FS_API INtFiles const& systemNtFiles() noexcept;

/// An entry opened by @c openEntry.
struct OpenedEntry
{
    UniqueHandle handle;        ///< The handle.
    EntryAttributes attributes; ///< What the first open found under the name.
};

/// Opens @p name beneath @p directory as the reparse point itself, never following a link. Where the
/// name is a reparse point that names nothing else, the object is opened again by the handle just
/// checked, through its filter, which then supplies its data; where no filter takes the point's tag
/// (`STATUS_IO_REPARSE_TAG_NOT_HANDLED`), the point is the file and the first handle is kept.
/// @param files The NT calls.
/// @param directory A held directory handle.
/// @param name One component in UTF-16; empty for @p directory itself.
/// @param request What to ask for; `FILE_READ_ATTRIBUTES` is added.
/// @return The entry; `NotPermitted` where the object opened again is another than the one checked,
///         and what else refused it.
[[nodiscard]] CORE_FS_API FsResult<OpenedEntry> openEntry(INtFiles const& files,
                                                          HANDLE directory,
                                                          std::wstring const& name,
                                                          NtOpenRequest request);

/// Opens @p entry's object again by its handle, with no name: the point itself, or through its
/// filter where @p entry is a reparse point that names nothing else (@c openEntry), and refusing,
/// either way, any reparse the object manager would follow. A second handle on an object is made
/// this way, never by `ReOpenFile`, which follows whatever reparse point the object holds by then.
/// @param files The NT calls.
/// @param entry What @c openEntry opened.
/// @param request What to ask for; the disposition is always `FILE_OPEN`, `FILE_READ_ATTRIBUTES` is
///        added.
/// @return The handle; `NotPermitted` where the object became a point that names another object, or
///         the handle opened is another object; what else refused it.
[[nodiscard]] CORE_FS_API FsResult<UniqueHandle> openAgain(INtFiles const& files,
                                                           OpenedEntry const& entry,
                                                           NtOpenRequest request);

/// @c openAgain for an object held by a handle the caller keeps.
/// @param files The NT calls.
/// @param handle The object, open; borrowed for the call.
/// @param attributes What @p handle's object is, read from it.
/// @param request What to ask for, as for @c openAgain.
/// @return The handle, as @c openAgain answers.
[[nodiscard]] CORE_FS_API FsResult<UniqueHandle> openAgain(INtFiles const& files,
                                                           HANDLE handle,
                                                           EntryAttributes const& attributes,
                                                           NtOpenRequest request);

/// Resolves @p plan beneath @p root over @p files: @c resolveBeneath with the NT calls a file system
/// was built with, rather than the system's.
/// @param files The NT calls.
/// @param root The descriptor's directory handle; borrowed for the call.
/// @param plan The planned path.
/// @param rules How links, `..` and the kernel's separators are treated.
/// @return Where it resolved to, or what @c walkBeneath answers.
[[nodiscard]] CORE_FS_API FsResult<Resolved> resolveBeneathWith(INtFiles const& files,
                                                                HANDLE root,
                                                                PathPlan plan,
                                                                WalkRules const& rules);

/// Builds the Windows file system over @p files instead of the system's NT calls, so a test can
/// script what the system answers; @c makeNativeDescriptorFileSystem otherwise.
/// @param home Where every operation resumes.
/// @param blocking Where every system call runs.
/// @param wallClock What `SetToNow` reads.
/// @param random Where the metadata secret is drawn from.
/// @param options What the file system is built with.
/// @param files The NT calls; must outlive the file system and its descriptors.
/// @return The file system; `Invalid` for a `maxReadChunk` of 0.
[[nodiscard]] CORE_FS_API FsResult<std::unique_ptr<IDescriptorFileSystem>> makeWindowsDescriptorFileSystem(
    async::IExecutor& home,
    async::IExecutor& blocking,
    platform::IWallClock const& wallClock,
    IRandom& random,
    NativeFileSystemOptions options,
    INtFiles const& files);

/// A cursor over directory handles: the root, borrowed, and a stack of the directories entered
/// beneath it, each held open by this cursor and closed where it is left. It takes no shortcut:
/// `NtCreateFile` has no mode that resolves several components beneath a handle.
class CORE_FS_API WindowsCursor final: public IBeneathCursor
{
  public:
    /// @param root The directory the walk starts on; borrowed, and must outlive the cursor.
    /// @param files The NT calls; must outlive the cursor.
    WindowsCursor(HANDLE root, INtFiles const& files) noexcept;

    WindowsCursor(WindowsCursor const&) = delete;
    WindowsCursor(WindowsCursor&&) = delete;
    WindowsCursor& operator=(WindowsCursor const&) = delete;
    WindowsCursor& operator=(WindowsCursor&&) = delete;
    ~WindowsCursor() override = default;

    [[nodiscard]] bool atRoot() const noexcept override;
    [[nodiscard]] FsResult<EntryKind> enter(std::string const& name) override;
    void leave() override;
    [[nodiscard]] FsResult<EntryKind> inspect(std::string const& name) override;
    [[nodiscard]] FsResult<std::string> readLink(std::string const& name) override;
    [[nodiscard]] FsResult<Shortcut> shortcut(std::span<std::string const> components) override;
    void restart() override;

    /// @return The directory the cursor stands on, borrowed.
    [[nodiscard]] HANDLE current() const noexcept;

    /// Hands over the directory the cursor stands on, where it is not the root.
    /// @return It; one holding no handle where the cursor stands on the root.
    [[nodiscard]] UniqueHandle release() noexcept;

  private:
    HANDLE _root;
    INtFiles const* _files;
    std::vector<UniqueHandle> _entered;
};

} // namespace core::fs::detail
