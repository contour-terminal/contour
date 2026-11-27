// SPDX-License-Identifier: Apache-2.0
//
// The native descriptors of Windows. Every system call is made inside async::offload, on the
// blocking executor, by a job that holds the descriptor's handle by shared ownership (decision C2),
// and the caller resumes on its home executor. Rights are checked from rightsTable() before
// anything else, inside the job.
//
// A path is resolved beneath the descriptor by detail::resolveBeneath, one component at a time over
// held directory handles (windows/WindowsPathWalk.cpp), and the final name is acted on without
// following, in the directory the walk holds: opened with NtCreateFile relative to that handle, as
// the reparse point itself, then read, written, renamed or linked by handle. A link is made and read
// through FSCTL_SET_REPARSE_POINT and FSCTL_GET_REPARSE_POINT, renamed and linked through
// NtSetInformationFile with the new directory's handle as RootDirectory; no call takes a path.
//
// A directory stream opens the directory again by its handle (detail::openAgain), so its position
// is its own, and reads it with GetFileInformationByHandleEx(FileIdBothDirectoryInfo) into one
// 64 KiB buffer per batch, one offload each; each record carries what its entry is.
#include <core/async/IExecutor.hpp>
#include <core/async/Offload.hpp>
#include <core/async/Task.hpp>
#include <core/fs/AccessScope.hpp>
#include <core/fs/Descriptor.hpp>
#include <core/fs/FsError.hpp>
#include <core/fs/NativeFileSystem.hpp>
#include <core/fs/Rights.hpp>
#include <core/fs/detail/DirectoryStream.hpp>
#include <core/fs/detail/NativeSupport.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/windows/AccessFacts.hpp>
#include <core/fs/windows/DirectoryRecords.hpp>
#include <core/fs/windows/NtFile.hpp>
#include <core/fs/windows/OwnerSecurity.hpp>
#include <core/fs/windows/ReparsePoint.hpp>
#include <core/fs/windows/WindowsPathWalk.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace core::fs
{

void detail::closeNativeHandle(platform::NativeHandle handle) noexcept
{
    ::CloseHandle(handle);
}

namespace
{

    using detail::NativeContext;
    using detail::refuse;
    using detail::SharedContext;
    using detail::SharedHandle;
    using detail::UniqueHandle;

    /// What the kernel splits a name on (and `:`, which names a stream): never part of a name handed
    /// to it, whatever the policy.
    constexpr std::string_view KernelSeparators = "\\/:";

    /// The largest offset a positioned read or write takes.
    constexpr auto OffsetMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    /// 100-nanosecond intervals from 1601-01-01 to 1970-01-01, and per second.
    constexpr std::int64_t UnixEpochTicks = 116'444'736'000'000'000;
    constexpr std::int64_t TicksPerSecond = 10'000'000;
    constexpr std::int64_t NanosecondsPerTick = 100;

    std::unexpected<FsError> lastError(char const* call)
    {
        return std::unexpected { fsErrorFromSystemCode(std::int64_t { ::GetLastError() }, call) };
    }

    FsResult<void> succeededOr(BOOL result, char const* call)
    {
        if (result == FALSE)
            return lastError(call);
        return {};
    }

    FileTime fileTimeOf(LARGE_INTEGER ticks) noexcept
    {
        auto const sinceEpoch = ticks.QuadPart - UnixEpochTicks;
        auto seconds = sinceEpoch / TicksPerSecond;
        auto remainder = sinceEpoch % TicksPerSecond;
        if (remainder < 0)
        {
            --seconds;
            remainder += TicksPerSecond;
        }
        return FileTime { .seconds = seconds,
                          .nanoseconds = static_cast<std::uint32_t>(remainder * NanosecondsPerTick) };
    }

    /// @return @p time in FILETIME ticks; `Invalid` past what 64 bits hold, and at or before
    ///         1601-01-01T00:00:00Z: `FILE_BASIC_INFO` reads a time of 0 as "leave it" and -1 as
    ///         "stop updating it", so neither instant can be set, and refusing says so rather than
    ///         doing something else silently.
    FsResult<std::int64_t> ticksOf(FileTime time)
    {
        constexpr auto Max = std::numeric_limits<std::int64_t>::max();
        if (time.nanoseconds >= 1'000'000'000)
            return refuse(FsErrorCode::Invalid);
        if (time.seconds < -UnixEpochTicks / TicksPerSecond
            || time.seconds > ((Max - UnixEpochTicks) / TicksPerSecond) - 1)
            return refuse(FsErrorCode::Invalid);
        auto const ticks =
            UnixEpochTicks + (time.seconds * TicksPerSecond) + (time.nanoseconds / NanosecondsPerTick);
        if (ticks <= 0)
            return refuse(FsErrorCode::Invalid);
        return ticks;
    }

    /// Replaces `SetToNow` with the wall clock's instant, read on the caller's thread before the hop,
    /// so the clock is never read from the blocking executor.
    NewTimestamp resolveNow(NewTimestamp change, platform::IWallClock const& clock)
    {
        if (std::holds_alternative<SetToNow>(change))
            return detail::fileTimeOf(clock.now());
        return change;
    }

    /// The `FILE_BASIC_INFO` time for @p change, `SetToNow` already resolved: 0 leaves the time.
    FsResult<std::int64_t> basicTimeOf(NewTimestamp const& change)
    {
        if (auto const* time = std::get_if<FileTime>(&change))
            return ticksOf(*time);
        if (std::holds_alternative<NoChange>(change))
            return std::int64_t { 0 };
        return refuse(FsErrorCode::Invalid);
    }

    /// What one handle tells about its object.
    struct Information
    {
        FILE_BASIC_INFO basic {};
        FILE_STANDARD_INFO standard {};
        FILE_ATTRIBUTE_TAG_INFO tag {};
    };

    FsResult<Information> informationOf(HANDLE handle)
    {
        auto information = Information {};
        if (::GetFileInformationByHandleEx(
                handle, FileBasicInfo, &information.basic, sizeof(information.basic))
                == FALSE
            || ::GetFileInformationByHandleEx(
                   handle, FileStandardInfo, &information.standard, sizeof(information.standard))
                   == FALSE)
            return lastError("GetFileInformationByHandleEx");
        if (::GetFileInformationByHandleEx(
                handle, FileAttributeTagInfo, &information.tag, sizeof(information.tag))
            == FALSE)
        {
            // FAT and exFAT keep no reparse point and answer no FileAttributeTagInfo: their
            // attributes are the basic ones, and no tag.
            if (::GetLastError() != ERROR_INVALID_PARAMETER)
                return lastError("GetFileInformationByHandleEx");
            information.tag = FILE_ATTRIBUTE_TAG_INFO { .FileAttributes = information.basic.FileAttributes,
                                                        .ReparseTag = 0 };
        }
        return information;
    }

    /// A reparse point that names another object (a symbolic link, a junction, a mount point) is a
    /// link; one that names nothing else is the file or directory it is.
    DescriptorType typeOf(Information const& information) noexcept
    {
        auto const attributes = detail::EntryAttributes { .attributes = information.tag.FileAttributes,
                                                          .reparseTag = information.tag.ReparseTag };
        if (attributes.isLink())
            return DescriptorType::SymbolicLink;
        if (information.standard.Directory != FALSE)
            return DescriptorType::Directory;
        return DescriptorType::RegularFile;
    }

    /// What @p handle tells: its attributes, times and, read from its security descriptor, who may
    /// reach it. The handle carries `READ_CONTROL`: every open a stat is made through asks for it.
    FsResult<DescriptorStat> statOf(NativeContext const& context, HANDLE handle)
    {
        return informationOf(handle).transform([&](Information const& information) {
            return DescriptorStat {
                .type = typeOf(information),
                .linkCount = information.standard.NumberOfLinks,
                .size = static_cast<std::uint64_t>(information.standard.EndOfFile.QuadPart),
                .accessed = fileTimeOf(information.basic.LastAccessTime),
                .modified = fileTimeOf(information.basic.LastWriteTime),
                .statusChanged = fileTimeOf(information.basic.ChangeTime),
                .access =
                    detail::accessFactsOf(handle, context.ownerSecurity->user()).transform(classifyAccess),
            };
        });
    }

    FsResult<MetadataHash> hashOf(NativeContext const& context, HANDLE handle)
    {
        auto const identity = context.ntFiles->identity(handle);
        if (!identity)
            return std::unexpected { identity.error() };
        return informationOf(handle).transform([&](Information const& information) {
            return detail::hashMetadata(
                context.secret,
                detail::MetadataFields {
                    .device = identity->volume,
                    .fileIdLow = identity->idLow,
                    .fileIdHigh = identity->idHigh,
                    .size = static_cast<std::uint64_t>(information.standard.EndOfFile.QuadPart),
                    .modified = fileTimeOf(information.basic.LastWriteTime),
                    .statusChanged = fileTimeOf(information.basic.ChangeTime) });
        });
    }

    /// How far a directory stream has read its directory.
    enum class ListingProgress : std::uint8_t
    {
        AtStart, ///< No batch has been read.
        Started, ///< At least one has.
    };

    /// A Win32 code the listing call answers that is the end of the directory rather than a failure.
    struct ListingEndRow
    {
        DWORD code;
        /// The code ends the directory only on the first batch, or on any.
        std::optional<ListingProgress> onlyAt;
    };

    constexpr auto ListingEndRows = std::to_array<ListingEndRow>({
        { .code = ERROR_NO_MORE_FILES, .onlyAt = std::nullopt },
        // A directory with no entry at all, not even `.` and `..`, as a FAT or exFAT volume's root
        // is (STATUS_NO_SUCH_FILE); later, the same code would be a failure.
        { .code = ERROR_FILE_NOT_FOUND, .onlyAt = ListingProgress::AtStart },
    });

    /// A directory stream's own handle on its directory, the NT calls it lists through and the
    /// buffer its batches are read into, shared by the stream and the batch read in flight. The
    /// handle keeps a position, so a batch holds @c lock while it reads.
    struct DirectoryReader
    {
        SharedHandle handle;                ///< Opened for the stream alone.
        detail::INtFiles const* files;      ///< What it lists through.
        std::mutex lock;                    ///< Held by the batch that reads.
        std::vector<std::uint64_t> records; ///< @c detail::DirectoryBufferBytes, 8-byte aligned.
        ListingProgress progress = ListingProgress::AtStart; ///< Under @c lock.

        DirectoryReader(SharedHandle directory, detail::INtFiles const& ntFiles):
            handle { std::move(directory) },
            files { &ntFiles },
            records(detail::DirectoryBufferBytes / sizeof(std::uint64_t))
        {
        }
    };

    /// @return Whether @p error, met at @p progress, is the end of the directory.
    bool endsListing(FsError const& error, ListingProgress progress) noexcept
    {
        return std::ranges::any_of(ListingEndRows, [&](ListingEndRow const& row) {
            return error.systemCode == std::int64_t { row.code } && (!row.onlyAt || *row.onlyAt == progress);
        });
    }

    /// Reads the records that fit in @p reader's buffer, from where its handle stands.
    FsResult<detail::DirectoryBatch> readBatch(DirectoryReader& reader)
    {
        auto const held = std::scoped_lock { reader.lock };
        auto const buffer = std::as_writable_bytes(std::span { reader.records });
        auto const progress = std::exchange(reader.progress, ListingProgress::Started);
        if (auto listed = reader.files->queryDirectory(reader.handle->get(), buffer); !listed)
        {
            if (endsListing(listed.error(), progress))
                return detail::DirectoryBatch { .entries = {}, .end = StreamEnd::Reached };
            return std::unexpected { std::move(listed.error()) };
        }
        return detail::parseDirectoryRecords(buffer).transform(
            [](std::vector<FsResult<DirectoryEntry>> entries) {
                return detail::DirectoryBatch { .entries = std::move(entries), .end = StreamEnd::More };
            });
    }

    class WindowsDirectoryStream final: public detail::BatchedDirectoryStream
    {
      public:
        WindowsDirectoryStream(SharedContext context, std::shared_ptr<DirectoryReader> reader):
            _context { std::move(context) }, _reader { std::move(reader) }
        {
        }

      protected:
        async::Task<FsResult<detail::DirectoryBatch>> fetch() override
        {
            return async::offload(
                _context->home, _context->blocking, [reader = _reader]() { return readBatch(*reader); });
        }

      private:
        SharedContext _context;
        std::shared_ptr<DirectoryReader> _reader;
    };

    /// What a directory stream opens its directory again with.
    constexpr auto ListRequest = detail::NtOpenRequest { .access = FILE_LIST_DIRECTORY,
                                                         .disposition = FILE_OPEN,
                                                         .options = FILE_DIRECTORY_FILE };

    using detail::EntryKind;
    using detail::FinalComponent;
    using detail::Resolved;
    using detail::TrailingSlashOnLink;

    /// Resolves @p path beneath @p root, following a final link where @p finalPolicy, or a trailing
    /// `/` under @p onLink, says so.
    FsResult<Resolved> resolve(HANDLE root,
                               std::string_view path,
                               NativeContext const& context,
                               SymlinkPolicy finalPolicy,
                               TrailingSlashOnLink onLink)
    {
        return detail::planRelativePath(path, *context.policy).and_then([&](detail::PathPlan plan) {
            return detail::resolveBeneathWith(
                *context.ntFiles, root, std::move(plan), detail::walkRulesOf(context, finalPolicy, onLink));
        });
    }

    /// @return @p resolved, where it names a place an entry of @p creating can be made; `Exist`
    ///         where something is there already (the directory itself included), `NotDirectory`
    ///         where the path ends in `/` and @p creating is not a directory.
    FsResult<Resolved> vacant(Resolved resolved, DescriptorType creating)
    {
        if (resolved.name.empty() || resolved.kind != EntryKind::Missing)
            return refuse(FsErrorCode::Exist);
        if (creating != DescriptorType::Directory && resolved.finalComponent == FinalComponent::Directory)
            return refuse(FsErrorCode::NotDirectory);
        return resolved;
    }

    /// Opens the final name of @p resolved, not following it: the directory itself again where the
    /// path names it.
    FsResult<UniqueHandle> openFinal(NativeContext const& context,
                                     Resolved const& resolved,
                                     detail::NtOpenRequest const& request)
    {
        return detail::utf16Of(resolved.name).and_then([&](std::wstring const& name) {
            return context.ntFiles->open(resolved.directory(), name, request);
        });
    }

    /// @return The request that opens an entry to read its attributes and, with @p access, more; a
    ///         stat asks for `READ_CONTROL` too (@c statRequest), so it can read who may reach it.
    detail::NtOpenRequest attributesRequest(ACCESS_MASK access) noexcept
    {
        return detail::NtOpenRequest { .access = access | FILE_READ_ATTRIBUTES,
                                       .disposition = FILE_OPEN,
                                       .options = 0,
                                       .reparse = detail::ReparseHandling::OpenThePoint };
    }

    /// @return The request a stat by name opens its entry with: attributes and the security descriptor.
    detail::NtOpenRequest statRequest() noexcept
    {
        return attributesRequest(READ_CONTROL);
    }

    /// Opens the final name of @p resolved for a stat: with `READ_CONTROL`, and where the object's
    /// DACL withholds that (`Access`), again without it, so a stat that succeeded before access was
    /// read still does; its access is then unknown (nullopt), never guessed.
    FsResult<UniqueHandle> openForStat(NativeContext const& context, Resolved const& resolved)
    {
        auto opened = openFinal(context, resolved, statRequest());
        if (opened || opened.error().code != FsErrorCode::Access)
            return opened;
        return openFinal(context, resolved, attributesRequest(0));
    }

    struct SyncFlagRow
    {
        DescriptorFlags flag;
        ULONG createOption; ///< 0 where Windows has no such mode.
    };

    constexpr auto SyncFlagRows = std::to_array<SyncFlagRow>({
        { .flag = DescriptorFlags::FileIntegritySync, .createOption = FILE_WRITE_THROUGH },
        { .flag = DescriptorFlags::DataIntegritySync, .createOption = FILE_WRITE_THROUGH },
        { .flag = DescriptorFlags::RequestedWriteSync, .createOption = 0 },
    });

    /// The `NtCreateFile` request for @p open and @p flags.
    FsResult<detail::NtOpenRequest> openRequestOf(OpenFlags open, DescriptorFlags flags)
    {
        // READ_CONTROL: the descriptor's stat reads who may reach the object from this very handle.
        auto request = detail::NtOpenRequest { .access = FILE_READ_ATTRIBUTES | READ_CONTROL,
                                               .disposition = FILE_OPEN,
                                               .options = 0 };
        if (hasFlag(flags, DescriptorFlags::Read))
            request.access |= FILE_GENERIC_READ;
        if (hasFlag(flags, DescriptorFlags::Write))
            request.access |= FILE_GENERIC_WRITE;
        if (hasFlag(flags, DescriptorFlags::MutateDirectory))
            request.access |= FILE_WRITE_ATTRIBUTES;
        auto const create = hasFlag(open, OpenFlags::Create);
        auto const truncate = hasFlag(open, OpenFlags::Truncate);
        // Never an overwrite disposition: it would truncate whatever the name opens before the
        // caller sees what that is, a symbolic link included. openAt truncates by handle, after.
        if (create && hasFlag(open, OpenFlags::Exclusive))
            request.disposition = FILE_CREATE;
        else if (create)
            request.disposition = FILE_OPEN_IF;
        if (truncate)
            request.access |= FILE_WRITE_DATA;
        if (hasFlag(open, OpenFlags::Directory))
            request.options |= FILE_DIRECTORY_FILE;
        // A directory never carries Write, and is never created or truncated by openAt.
        if (hasFlag(flags, DescriptorFlags::Write) || create || truncate)
            request.options |= FILE_NON_DIRECTORY_FILE;
        for (auto const& row: SyncFlagRows)
        {
            if (!hasFlag(flags, row.flag))
                continue;
            if (row.createOption == 0)
                return refuse(FsErrorCode::Unsupported);
            request.options |= row.createOption;
        }
        return request;
    }

    /// Marks @p handle's object for deletion with POSIX semantics (its name goes at once, even while
    /// another handle is open on it), or the classic way where the file system has no such mode.
    FsResult<void> deleteOpened(HANDLE handle)
    {
        auto posix = FILE_DISPOSITION_INFO_EX { .Flags = FILE_DISPOSITION_FLAG_DELETE
                                                         | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
                                                         | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE };
        if (::SetFileInformationByHandle(handle, FileDispositionInfoEx, &posix, sizeof(posix)) != FALSE)
            return {};
        auto const error = ::GetLastError();
        if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED
            && error != ERROR_INVALID_FUNCTION)
            return std::unexpected { fsErrorFromSystemCode(std::int64_t { error },
                                                           "SetFileInformationByHandle") };
        auto classic = FILE_DISPOSITION_INFO { .DeleteFile = TRUE };
        return succeededOr(
            ::SetFileInformationByHandle(handle, FileDispositionInfo, &classic, sizeof(classic)),
            "SetFileInformationByHandle");
    }

    /// Reads @p handle's access back and refuses an object that is not owner-only: a file system that
    /// keeps no security descriptor (FAT, exFAT) drops the one it was created with and says nothing.
    /// The refused object is deleted through @p handle, which carries `READ_CONTROL` and `DELETE`, so
    /// it goes when the handle is closed and no name is ever looked up again.
    /// @param context The file system, for the token's user.
    /// @param handle The object just created.
    /// @return Nothing where it reads owner-only; `NotPermitted` otherwise.
    FsResult<void> requireOwnerOnly(NativeContext const& context, HANDLE handle)
    {
        if (detail::accessFactsOf(handle, context.ownerSecurity->user()).transform(classifyAccess)
            == AccessScope::OwnerOnly)
            return {};
        std::ignore = deleteOpened(handle);
        return std::unexpected { FsError { .code = FsErrorCode::NotPermitted,
                                           .systemCode = 0,
                                           .context = "the new object is not owner-only" } };
    }

    FsResult<void> truncateOpened(HANDLE handle)
    {
        auto end = FILE_END_OF_FILE_INFO {};
        end.EndOfFile.QuadPart = 0;
        return succeededOr(::SetFileInformationByHandle(handle, FileEndOfFileInfo, &end, sizeof(end)),
                           "SetFileInformationByHandle");
    }

    /// @return Whether @p resolved's final name is a directory link, which is a directory to the
    ///         system: no file replaces it, as none replaces a directory.
    bool isDirectoryLink(NativeContext const& context, Resolved const& resolved)
    {
        if (resolved.kind != EntryKind::SymbolicLink)
            return false;
        auto const link = openFinal(context, resolved, attributesRequest(0));
        if (!link)
            return false;
        auto const attributes = context.ntFiles->attributes(link->get());
        return attributes && attributes->isDirectory();
    }

    /// Whether a symbolic link is made as a directory link, which is what the system, and every
    /// program on it, can follow to a directory; to this file system both are links alike.
    enum class LinkFlavour : std::uint8_t
    {
        File,
        Directory,
    };

    /// @return The flavour of a link to @p target made at @p newPath beneath @p root: a directory
    ///         link where the target says so by its spelling (it ends in a separator, `.` or `..`),
    ///         or where it names a directory beneath @p root when followed from where the link will
    ///         be; a file link otherwise, a target that cannot be resolved beneath @p root included.
    LinkFlavour flavourOf(HANDLE root,
                          std::string_view newPath,
                          std::string_view target,
                          NativeContext const& context)
    {
        auto const& policy = *context.policy;
        auto const last = target.substr(target.find_last_of(policy.targetSeparators) + 1);
        if (last.empty() || last == "." || last == "..")
            return LinkFlavour::Directory;
        auto plan = detail::planRelativePath(newPath, policy);
        if (!plan || !detail::spliceSymlink(*plan, plan->components.size() - 1, target, policy))
            return LinkFlavour::File;
        auto const followed = detail::resolveBeneathWith(
            *context.ntFiles,
            root,
            std::move(*plan),
            detail::walkRulesOf(context, SymlinkPolicy::Follow, TrailingSlashOnLink::Follow));
        if (followed && (followed->name.empty() || followed->kind == EntryKind::Directory))
            return LinkFlavour::Directory;
        return LinkFlavour::File;
    }

    /// Checks a link's substitute name against what the kernel itself reads in a relative link's
    /// target, whatever the policy let through: a leading `\` resolves from the volume's root and
    /// `\??\` from the object namespace, so either is `NotPermitted`; a `:` names a drive in the
    /// first component (`NotPermitted`) and a stream anywhere else (`Invalid`).
    FsResult<void> checkKernelTarget(std::wstring const& substituteName)
    {
        if (substituteName.starts_with(L'\\'))
            return refuse(FsErrorCode::NotPermitted);
        auto const colon = substituteName.find(L':');
        if (colon == std::wstring::npos)
            return {};
        if (colon < substituteName.find(L'\\'))
            return refuse(FsErrorCode::NotPermitted);
        return refuse(FsErrorCode::Invalid);
    }

    /// Makes @p resolved's name a symbolic link to @p target: a new file or directory, given the
    /// link's reparse data, and removed again where that is refused. Where the removal fails too, the
    /// refusal is still the write's, which says why the link was not made, and its context names the
    /// empty entry left behind.
    FsResult<void> createSymlink(NativeContext const& context,
                                 Resolved const& resolved,
                                 std::string const& target,
                                 LinkFlavour flavour)
    {
        auto const printName = detail::utf16Of(target);
        if (!printName)
            return std::unexpected { printName.error() };
        auto substituteName = *printName;
        std::ranges::replace(substituteName, L'/', L'\\');
        if (auto checked = checkKernelTarget(substituteName); !checked)
            return checked;
        auto const link =
            openFinal(context,
                      resolved,
                      detail::NtOpenRequest {
                          .access = FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES | DELETE,
                          .disposition = FILE_CREATE,
                          .options = flavour == LinkFlavour::Directory ? ULONG { FILE_DIRECTORY_FILE }
                                                                       : ULONG { FILE_NON_DIRECTORY_FILE },
                          .reparse = detail::ReparseHandling::OpenThePoint });
        if (!link)
            return std::unexpected { link.error() };
        auto written = detail::writeReparseData(
            link->get(), detail::relativeSymlinkReparseData(substituteName, *printName));
        if (written)
            return written;
        auto failure = written.error();
        if (auto removed = deleteOpened(link->get()); !removed)
            failure.context +=
                "; the empty entry made for the link was not removed: " + removed.error().toString();
        return std::unexpected { std::move(failure) };
    }

    /// The statuses with which a file system refuses `FileRenameInformationEx` it does not know.
    constexpr auto RenameExUnknownStatuses = std::to_array<std::int64_t>({ STATUS_INVALID_INFO_CLASS,
                                                                           STATUS_NOT_IMPLEMENTED,
                                                                           STATUS_NOT_SUPPORTED,
                                                                           STATUS_INVALID_PARAMETER });

    /// Renames @p handle's object to @p name beneath @p directory, replacing what is there: with POSIX
    /// semantics, so an open target's name goes at once, where the file system knows them.
    FsResult<void> renameOpened(HANDLE handle, HANDLE directory, std::wstring const& name)
    {
        auto renamed = detail::ntSetName(handle,
                                         detail::FileRenameInformationExClass,
                                         detail::RenameReplaceIfExists | detail::RenamePosixSemantics
                                             | detail::RenameIgnoreReadOnlyAttribute,
                                         directory,
                                         name);
        if (renamed
            || std::ranges::find(RenameExUnknownStatuses, renamed.error().systemCode)
                   == RenameExUnknownStatuses.end())
            return renamed;
        return detail::ntSetName(
            handle, detail::FileRenameInformationClass, detail::RenameReplaceIfExists, directory, name);
    }

    FsResult<void> setTimesOf(HANDLE handle, NewTimestamp const& accessed, NewTimestamp const& modified)
    {
        auto const accessTicks = basicTimeOf(accessed);
        if (!accessTicks)
            return std::unexpected { accessTicks.error() };
        auto const writeTicks = basicTimeOf(modified);
        if (!writeTicks)
            return std::unexpected { writeTicks.error() };
        auto basic = FILE_BASIC_INFO {};
        basic.LastAccessTime.QuadPart = *accessTicks;
        basic.LastWriteTime.QuadPart = *writeTicks;
        return succeededOr(::SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic)),
                           "SetFileInformationByHandle");
    }

    OVERLAPPED overlappedAt(std::uint64_t offset) noexcept
    {
        auto overlapped = OVERLAPPED {};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFF'FFFFU);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        return overlapped;
    }

    /// Writes all of @p bytes through @p handle, each chunk placed by @p offsetOf.
    template <typename OffsetOf>
    FsResult<std::size_t> writeAll(HANDLE handle, std::span<std::byte const> bytes, OffsetOf offsetOf)
    {
        auto written = std::size_t { 0 };
        while (written < bytes.size())
        {
            auto const chunk = static_cast<DWORD>(
                std::min<std::size_t>(bytes.size() - written, std::numeric_limits<DWORD>::max()));
            auto overlapped = offsetOf(written);
            auto count = DWORD { 0 };
            if (::WriteFile(handle, bytes.data() + written, chunk, &count, &overlapped) == FALSE)
            {
                if (written > 0)
                    break;
                return lastError("WriteFile");
            }
            if (count == 0)
                break;
            written += count;
        }
        return written;
    }

    class WindowsDescriptor final: public detail::NativeDescriptorBase
    {
      public:
        WindowsDescriptor(SharedContext context,
                          SharedHandle handle,
                          SharedHandle appendHandle,
                          DescriptorFlags flags,
                          DescriptorType type):
            NativeDescriptorBase { std::move(context), std::move(handle), flags, type },
            _appendHandle { std::move(appendHandle) }
        {
        }

        async::Task<FsResult<ReadChunk>> read(std::size_t maxBytes, std::uint64_t offset) override
        {
            return submit([handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           want = std::min({ maxBytes,
                                             context()->options.maxReadChunk,
                                             std::size_t { std::numeric_limits<DWORD>::max() } }),
                           offset]() -> FsResult<ReadChunk> {
                return checkRights(flags, Operation::Read).and_then([&]() -> FsResult<ReadChunk> {
                    if (offset > OffsetMax)
                        return ReadChunk { .bytes = {}, .end = StreamEnd::Reached };
                    auto bytes = std::vector<std::byte>(want);
                    auto overlapped = overlappedAt(offset);
                    auto count = DWORD { 0 };
                    if (::ReadFile(handle->get(), bytes.data(), static_cast<DWORD>(want), &count, &overlapped)
                            == FALSE
                        && ::GetLastError() != ERROR_HANDLE_EOF)
                    {
                        // A directory's handle reads nothing: POSIX's EISDIR, said in Windows' words.
                        if (::GetLastError() == ERROR_INVALID_FUNCTION && type == DescriptorType::Directory)
                            return refuse(FsErrorCode::IsDirectory);
                        return lastError("ReadFile");
                    }
                    bytes.resize(count);
                    auto const end = bytes.size() < want ? StreamEnd::Reached : StreamEnd::More;
                    return ReadChunk { .bytes = std::move(bytes), .end = end };
                });
            });
        }

        async::Task<FsResult<std::size_t>> write(std::vector<std::byte> bytes, std::uint64_t offset) override
        {
            return submit([handle = handle(), flags = flags(), bytes = std::move(bytes), offset]() {
                return checkRights(flags, Operation::Write).and_then([&]() -> FsResult<std::size_t> {
                    if (offset > OffsetMax || bytes.size() > OffsetMax - offset)
                        return refuse(FsErrorCode::FileTooLarge);
                    return writeAll(handle->get(), bytes, [offset](std::size_t written) {
                        return overlappedAt(offset + written);
                    });
                });
            });
        }

        async::Task<FsResult<std::size_t>> append(std::vector<std::byte> bytes) override
        {
            return submit([appendHandle = _appendHandle, flags = flags(), bytes = std::move(bytes)]() {
                return checkRights(flags, Operation::Append).and_then([&]() -> FsResult<std::size_t> {
                    if (!appendHandle)
                        return refuse(FsErrorCode::BadDescriptor);
                    // An offset of all ones writes at the end of the file.
                    return writeAll(appendHandle->get(), bytes, [](std::size_t) {
                        auto overlapped = OVERLAPPED {};
                        overlapped.Offset = 0xFFFF'FFFFU;
                        overlapped.OffsetHigh = 0xFFFF'FFFFU;
                        return overlapped;
                    });
                });
            });
        }

        async::Task<FsResult<void>> advise(std::uint64_t, std::uint64_t, Advice) override
        {
            // Windows takes no advice on a range of an open file; it succeeds, as advice may.
            return submit([flags = flags()]() { return checkRights(flags, Operation::Advise); });
        }

        async::Task<FsResult<void>> syncData() override { return sync(); }

        async::Task<FsResult<void>> sync() override
        {
            return submit([handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Sync).and_then([&]() -> FsResult<void> {
                    // FlushFileBuffers needs write access, which a read-only or directory handle
                    // lacks; such a descriptor wrote nothing to flush, and succeeds as fsync does.
                    if (!hasFlag(flags, DescriptorFlags::Write))
                        return {};
                    return succeededOr(::FlushFileBuffers(handle->get()), "FlushFileBuffers");
                });
            });
        }

        async::Task<FsResult<DescriptorType>> type() override
        {
            return submit([handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Stat)
                    .and_then([&] { return informationOf(handle->get()); })
                    .transform([](Information const& information) { return typeOf(information); });
            });
        }

        async::Task<FsResult<void>> setSize(std::uint64_t size) override
        {
            return submit([handle = handle(), flags = flags(), size]() {
                return checkRights(flags, Operation::SetSize).and_then([&]() -> FsResult<void> {
                    if (size > OffsetMax)
                        return refuse(FsErrorCode::FileTooLarge);
                    auto end = FILE_END_OF_FILE_INFO {};
                    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
                    return succeededOr(
                        ::SetFileInformationByHandle(handle->get(), FileEndOfFileInfo, &end, sizeof(end)),
                        "SetFileInformationByHandle");
                });
            });
        }

        async::Task<FsResult<void>> setTimes(NewTimestamp accessed, NewTimestamp modified) override
        {
            auto const& clock = *context()->wallClock;
            return submit([handle = handle(),
                           flags = flags(),
                           accessed = resolveNow(accessed, clock),
                           modified = resolveNow(modified, clock)]() {
                return checkRights(flags, Operation::SetTimes).and_then([&] {
                    return setTimesOf(handle->get(), accessed, modified);
                });
            });
        }

        async::Task<FsResult<DescriptorStat>> stat() override
        {
            return submit([context = context(), handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Stat).and_then([&] {
                    return statOf(*context, handle->get());
                });
            });
        }

        async::Task<FsResult<MetadataHash>> metadataHash() override
        {
            return submit([context = context(), handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::MetadataHash).and_then([&] {
                    return hashOf(*context, handle->get());
                });
            });
        }

        async::Task<FsResult<std::unique_ptr<IDirectoryStream>>> readDirectory() override
        {
            return submit([context = context(), handle = handle(), flags = flags(), type = objectType()]() {
                auto const& files = *context->ntFiles;
                return checkRights(flags, Operation::ReadDirectory)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] { return files.attributes(handle->get()); })
                    .and_then([&](detail::EntryAttributes const& attributes) {
                        return detail::openAgain(files, handle->get(), attributes, ListRequest);
                    })
                    .transform([&](UniqueHandle opened) -> std::unique_ptr<IDirectoryStream> {
                        return std::make_unique<WindowsDirectoryStream>(
                            context,
                            std::make_shared<DirectoryReader>(
                                detail::shareHandle(std::move(opened), *context->blocking), files));
                    });
            });
        }

        async::Task<FsResult<std::unique_ptr<IDescriptor>>> openAt(SymlinkPolicy policy,
                                                                   std::string path,
                                                                   OpenFlags open,
                                                                   DescriptorFlags childFlags) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           policy,
                           path = std::move(path),
                           open,
                           childFlags]() -> FsResult<std::unique_ptr<IDescriptor>> {
                // Create | Exclusive never follows a final link: a dangling one is Exist rather than
                // created through.
                auto const exclusive =
                    hasFlag(open, OpenFlags::Create) && hasFlag(open, OpenFlags::Exclusive);
                auto const resolved = checkRights(flags, Operation::OpenAt)
                                          .and_then([&] { return checkOpenRights(flags, open, childFlags); })
                                          .and_then([&] { return detail::requireDirectory(type); })
                                          .and_then([&] {
                                              return resolve(handle->get(),
                                                             path,
                                                             *context,
                                                             exclusive ? SymlinkPolicy::NoFollow : policy,
                                                             TrailingSlashOnLink::Follow);
                                          });
                if (!resolved)
                    return std::unexpected { resolved.error() };
                auto request = openRequestOf(open, childFlags);
                if (!request)
                    return std::unexpected { request.error() };
                // A link not followed is Loop, as `O_NOFOLLOW` answers, before the request's
                // FILE_NON_DIRECTORY_FILE could call a directory link IsDirectory.
                if (!exclusive && resolved->kind == EntryKind::SymbolicLink)
                    return refuse(FsErrorCode::Loop);
                if (resolved->finalComponent == FinalComponent::Directory && !resolved->name.empty())
                {
                    // `name/` opens nothing but a directory, and creates none.
                    if (resolved->kind == EntryKind::Missing && hasFlag(open, OpenFlags::Create))
                        return refuse(FsErrorCode::NotDirectory);
                    request->options |= FILE_DIRECTORY_FILE;
                }
                // Owner-only from the moment NtCreateFile makes it, and read back before it is used.
                // DELETE, so an object that does not read owner-only goes through this handle.
                if (hasFlag(open, OpenFlags::OwnerOnly))
                {
                    request->security = context->ownerSecurity->forFile();
                    request->access |= DELETE;
                }
                auto const name = detail::utf16Of(resolved->name);
                if (!name)
                    return std::unexpected { name.error() };
                auto child = detail::openEntry(*context->ntFiles, resolved->directory(), *name, *request);
                // READ_CONTROL is asked only so stat() can read access; where a descriptor with no
                // right of its own meets a DACL that withholds it, the open is made without it, and
                // the descriptor's access is unknown. Never on a creation: the creator is granted it.
                if (!child && child.error().code == FsErrorCode::Access && !hasFlag(open, OpenFlags::Create)
                    && !hasFlag(childFlags, DescriptorFlags::Read)
                    && !hasFlag(childFlags, DescriptorFlags::Write))
                {
                    request->access &= ~ACCESS_MASK { READ_CONTROL };
                    child = detail::openEntry(*context->ntFiles, resolved->directory(), *name, *request);
                }
                if (!child)
                    return std::unexpected { child.error() };
                auto const opened = child->handle.get();
                if (hasFlag(open, OpenFlags::OwnerOnly))
                    if (auto held = requireOwnerOnly(*context, opened); !held)
                        return std::unexpected { held.error() };
                auto const information = informationOf(opened);
                if (!information)
                    return std::unexpected { information.error() };
                auto const childType = typeOf(*information);
                // The name became a link since the walk looked: the kernel opened the link itself,
                // and changed nothing.
                if (childType == DescriptorType::SymbolicLink)
                    return refuse(FsErrorCode::Loop);
                // Truncated by handle only now that the handle is known to be a file's.
                if (hasFlag(open, OpenFlags::Truncate) && childType == DescriptorType::RegularFile)
                    if (auto truncated = truncateOpened(opened); !truncated)
                        return std::unexpected { truncated.error() };
                auto appendHandle = SharedHandle {};
                if (hasFlag(childFlags, DescriptorFlags::Write) && childType == DescriptorType::RegularFile)
                {
                    // The append handle is the same object opened again by its handle, never by
                    // ReOpenFile, which would follow a reparse point put on the file since; it writes
                    // through as the first does, where a sync mode asks.
                    auto second = detail::openAgain(
                        *context->ntFiles,
                        *child,
                        detail::NtOpenRequest { .access = FILE_APPEND_DATA,
                                                .disposition = FILE_OPEN,
                                                .options = FILE_NON_DIRECTORY_FILE
                                                           | (request->options & FILE_WRITE_THROUGH),
                                                .reparse = detail::ReparseHandling::OpenThePoint });
                    if (!second)
                        return std::unexpected { second.error() };
                    appendHandle = detail::shareHandle(std::move(*second), *context->blocking);
                }
                return std::make_unique<WindowsDescriptor>(
                    context,
                    detail::shareHandle(std::move(child->handle), *context->blocking),
                    std::move(appendHandle),
                    grantedFlags(childType, childFlags),
                    childType);
            });
        }

        async::Task<FsResult<void>> createDirectoryAt(std::string path, CreationAccess access) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           path = std::move(path),
                           access]() {
                return checkRights(flags, Operation::CreateDirectoryAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(),
                                       path,
                                       *context,
                                       SymlinkPolicy::NoFollow,
                                       TrailingSlashOnLink::Follow);
                    })
                    .and_then([](Resolved resolved) {
                        return vacant(std::move(resolved), DescriptorType::Directory);
                    })
                    .and_then([&](Resolved const& resolved) {
                        auto const ownerOnly = access == CreationAccess::OwnerOnly;
                        return openFinal(
                                   *context,
                                   resolved,
                                   detail::NtOpenRequest {
                                       // Read back, and deleted through this handle where it does not
                                       // read owner-only, as openAt's owner-only file is.
                                       .access = FILE_LIST_DIRECTORY
                                                 | (ownerOnly ? ACCESS_MASK { READ_CONTROL | DELETE } : 0),
                                       .disposition = FILE_CREATE,
                                       .options = FILE_DIRECTORY_FILE,
                                       .reparse = detail::ReparseHandling::OpenThePoint,
                                       .security =
                                           ownerOnly ? context->ownerSecurity->forDirectory() : nullptr })
                            .and_then([&](UniqueHandle const& made) -> FsResult<void> {
                                if (!ownerOnly)
                                    return {};
                                return requireOwnerOnly(*context, made.get());
                            });
                    });
            });
        }

        async::Task<FsResult<DescriptorStat>> statAt(SymlinkPolicy policy, std::string path) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           policy,
                           path = std::move(path)]() {
                return checkRights(flags, Operation::StatAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(), path, *context, policy, TrailingSlashOnLink::Follow);
                    })
                    .and_then([&](Resolved const& resolved) { return openForStat(*context, resolved); })
                    .and_then([&](UniqueHandle const& opened) { return statOf(*context, opened.get()); });
            });
        }

        async::Task<FsResult<void>> setTimesAt(SymlinkPolicy policy,
                                               std::string path,
                                               NewTimestamp accessed,
                                               NewTimestamp modified) override
        {
            auto const& clock = *context()->wallClock;
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           policy,
                           path = std::move(path),
                           accessed = resolveNow(accessed, clock),
                           modified = resolveNow(modified, clock)]() {
                return checkRights(flags, Operation::SetTimesAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(), path, *context, policy, TrailingSlashOnLink::Follow);
                    })
                    .and_then([&](Resolved const& resolved) {
                        return openFinal(*context, resolved, attributesRequest(FILE_WRITE_ATTRIBUTES));
                    })
                    .and_then([&](UniqueHandle const& opened) {
                        return setTimesOf(opened.get(), accessed, modified);
                    });
            });
        }

        async::Task<FsResult<void>> linkAt(SymlinkPolicy policy,
                                           std::string oldPath,
                                           IDescriptor* newDirectory,
                                           std::string newPath) override
        {
            auto const target = sameSystemTarget(newDirectory);
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           policy,
                           oldPath = std::move(oldPath),
                           target,
                           newPath = std::move(newPath)]() -> FsResult<void> {
                if (auto checked = checkRights(flags, Operation::LinkAt); !checked)
                    return checked;
                if (policy == SymlinkPolicy::Follow)
                    return refuse(FsErrorCode::Invalid);
                if (!target)
                    return std::unexpected { target.error() };
                if (auto checked = checkRights(target->flags, Operation::LinkAt); !checked)
                    return checked;
                if (auto checked = detail::requireDirectory(type).and_then(
                        [&] { return detail::requireDirectory(target->type); });
                    !checked)
                    return checked;
                auto const from = resolve(
                    handle->get(), oldPath, *context, SymlinkPolicy::NoFollow, TrailingSlashOnLink::Follow);
                if (!from)
                    return std::unexpected { from.error() };
                // A directory is never hard linked, as POSIX refuses it with EPERM.
                if (from->name.empty() || from->kind == EntryKind::Directory)
                    return refuse(FsErrorCode::NotPermitted);
                if (from->kind == EntryKind::Missing)
                    return refuse(FsErrorCode::NoEntry);
                auto const to = resolve(target->handle->get(),
                                        newPath,
                                        *context,
                                        SymlinkPolicy::NoFollow,
                                        TrailingSlashOnLink::Follow)
                                    .and_then([](Resolved resolved) {
                                        return vacant(std::move(resolved), DescriptorType::RegularFile);
                                    });
                if (!to)
                    return std::unexpected { to.error() };
                auto const name = detail::utf16Of(to->name);
                if (!name)
                    return std::unexpected { name.error() };
                return openFinal(*context, *from, attributesRequest(FILE_WRITE_ATTRIBUTES))
                    .and_then([&](UniqueHandle const& opened) {
                        return detail::ntSetName(
                            opened.get(), detail::FileLinkInformationClass, 0, to->directory(), *name);
                    });
            });
        }

        async::Task<FsResult<std::string>> readlinkAt(std::string path) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           path = std::move(path)]() {
                return checkRights(flags, Operation::ReadlinkAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(),
                                       path,
                                       *context,
                                       SymlinkPolicy::NoFollow,
                                       TrailingSlashOnLink::Follow);
                    })
                    .and_then([&](Resolved const& resolved) -> FsResult<UniqueHandle> {
                        if (resolved.name.empty())
                            return refuse(FsErrorCode::Invalid);
                        return openFinal(*context, resolved, attributesRequest(0));
                    })
                    .and_then([](UniqueHandle const& link) { return detail::readReparseData(link.get()); })
                    .and_then([](detail::ReparseData const& data) { return detail::reportedTarget(data); })
                    .and_then([&](std::string target) {
                        return detail::refuseRootedTarget(std::move(target), *context->policy);
                    });
            });
        }

        async::Task<FsResult<void>> removeDirectoryAt(std::string path) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           path = std::move(path)]() {
                return checkRights(flags, Operation::RemoveDirectoryAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(),
                                       path,
                                       *context,
                                       SymlinkPolicy::NoFollow,
                                       TrailingSlashOnLink::Refuse);
                    })
                    .and_then([&](Resolved const& resolved) -> FsResult<void> {
                        if (resolved.name.empty())
                            return refuse(FsErrorCode::Invalid);
                        auto const opened = openFinal(
                            *context,
                            resolved,
                            detail::NtOpenRequest { .access = DELETE | FILE_READ_ATTRIBUTES,
                                                    .disposition = FILE_OPEN,
                                                    .options = FILE_DIRECTORY_FILE,
                                                    .reparse = detail::ReparseHandling::OpenThePoint });
                        if (!opened)
                            return std::unexpected { opened.error() };
                        // A link to a directory carries the directory attribute, but rmdir does not
                        // remove a link.
                        auto const information = informationOf(opened->get());
                        if (!information)
                            return std::unexpected { information.error() };
                        if (typeOf(*information) != DescriptorType::Directory)
                            return refuse(FsErrorCode::NotDirectory);
                        return deleteOpened(opened->get());
                    });
            });
        }

        async::Task<FsResult<void>> renameAt(std::string oldPath,
                                             IDescriptor* newDirectory,
                                             std::string newPath) override
        {
            auto const target = sameSystemTarget(newDirectory);
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           oldPath = std::move(oldPath),
                           target,
                           newPath = std::move(newPath)]() -> FsResult<void> {
                if (auto checked = checkRights(flags, Operation::RenameAt); !checked)
                    return checked;
                if (!target)
                    return std::unexpected { target.error() };
                if (auto checked = checkRights(target->flags, Operation::RenameAt); !checked)
                    return checked;
                if (auto checked = detail::requireDirectory(type).and_then(
                        [&] { return detail::requireDirectory(target->type); });
                    !checked)
                    return checked;
                auto const from = resolve(
                    handle->get(), oldPath, *context, SymlinkPolicy::NoFollow, TrailingSlashOnLink::Refuse);
                if (!from)
                    return std::unexpected { from.error() };
                auto const to = resolve(target->handle->get(),
                                        newPath,
                                        *context,
                                        SymlinkPolicy::NoFollow,
                                        TrailingSlashOnLink::Refuse);
                if (!to)
                    return std::unexpected { to.error() };
                if (from->kind == EntryKind::Missing && !from->name.empty())
                    return refuse(FsErrorCode::NoEntry);
                if (from->name.empty() || to->name.empty())
                    return refuse(FsErrorCode::Invalid);
                // What POSIX's rename answers for a type that cannot replace another, which Windows
                // would answer as Access, or not at all.
                auto const fromDirectory = from->kind == EntryKind::Directory;
                if (!fromDirectory && to->finalComponent == FinalComponent::Directory)
                    return refuse(FsErrorCode::NotDirectory);
                if (fromDirectory && (to->kind == EntryKind::Other || to->kind == EntryKind::SymbolicLink))
                    return refuse(FsErrorCode::NotDirectory);
                if (!fromDirectory && (to->kind == EntryKind::Directory || isDirectoryLink(*context, *to)))
                    return refuse(FsErrorCode::IsDirectory);
                auto const name = detail::utf16Of(to->name);
                if (!name)
                    return std::unexpected { name.error() };
                return openFinal(*context, *from, attributesRequest(DELETE))
                    .and_then([&](UniqueHandle const& opened) {
                        return renameOpened(opened.get(), to->directory(), *name);
                    });
            });
        }

        async::Task<FsResult<void>> symlinkAt(std::string target, std::string newPath) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           target = std::move(target),
                           newPath = std::move(newPath)]() {
                return checkRights(flags, Operation::SymlinkAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] { return detail::checkSymlinkTarget(target, *context->policy); })
                    .and_then([&] {
                        return resolve(handle->get(),
                                       newPath,
                                       *context,
                                       SymlinkPolicy::NoFollow,
                                       TrailingSlashOnLink::Follow);
                    })
                    .and_then([](Resolved resolved) {
                        return vacant(std::move(resolved), DescriptorType::SymbolicLink);
                    })
                    .and_then([&](Resolved const& resolved) {
                        return createSymlink(
                            *context, resolved, target, flavourOf(handle->get(), newPath, target, *context));
                    });
            });
        }

        async::Task<FsResult<void>> unlinkFileAt(std::string path) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           path = std::move(path)]() {
                return checkRights(flags, Operation::UnlinkFileAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(),
                                       path,
                                       *context,
                                       SymlinkPolicy::NoFollow,
                                       TrailingSlashOnLink::Refuse);
                    })
                    .and_then([&](Resolved const& resolved) -> FsResult<void> {
                        if (resolved.name.empty() || resolved.kind == EntryKind::Directory)
                            return refuse(FsErrorCode::IsDirectory);
                        // Not FILE_NON_DIRECTORY_FILE: a directory link is a directory to the
                        // system, and a link to anything is unlinked.
                        auto const opened = openFinal(*context, resolved, attributesRequest(DELETE));
                        if (!opened)
                            return std::unexpected { opened.error() };
                        auto const information = informationOf(opened->get());
                        if (!information)
                            return std::unexpected { information.error() };
                        if (typeOf(*information) == DescriptorType::Directory)
                            return refuse(FsErrorCode::IsDirectory);
                        return deleteOpened(opened->get());
                    });
            });
        }

        async::Task<FsResult<MetadataHash>> metadataHashAt(SymlinkPolicy policy, std::string path) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           policy,
                           path = std::move(path)]() {
                return checkRights(flags, Operation::MetadataHashAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(), path, *context, policy, TrailingSlashOnLink::Follow);
                    })
                    .and_then([&](Resolved const& resolved) {
                        return openFinal(*context, resolved, attributesRequest(0));
                    })
                    .and_then([&](UniqueHandle const& opened) { return hashOf(*context, opened.get()); });
            });
        }

        async::Task<FsResult<bool>> isSameObject(IDescriptor* other) override
        {
            return submit([context = context(),
                           handle = handle(),
                           candidate = sameObjectCandidate(other)]() -> FsResult<bool> {
                if (!candidate)
                    return std::unexpected { candidate.error() };
                if (!*candidate)
                    return false;
                auto const& files = *context->ntFiles;
                return files.identity(handle->get()).and_then([&](detail::FileIdentity const& mine) {
                    return files.identity((*candidate)->handle->get())
                        .transform([&](detail::FileIdentity const& theirs) { return mine == theirs; });
                });
            });
        }

      private:
        SharedHandle _appendHandle; ///< The `FILE_APPEND_DATA` handle `append` writes through.
    };

    class WindowsFileSystem final: public IDescriptorFileSystem
    {
      public:
        explicit WindowsFileSystem(SharedContext context): _context { std::move(context) } {}

        async::Task<FsResult<std::unique_ptr<IDescriptor>>> openPreopen(std::filesystem::path hostDirectory,
                                                                        DescriptorFlags flags) override
        {
            return async::offload(
                _context->home,
                _context->blocking,
                [context = _context,
                 path = std::move(hostDirectory),
                 flags]() -> FsResult<std::unique_ptr<IDescriptor>> {
                    if (hasFlag(flags, DescriptorFlags::Write))
                        return refuse(FsErrorCode::IsDirectory);
                    // READ_CONTROL: the root's stat reads who may reach it from this handle.
                    auto access = DWORD { FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES
                                          | READ_CONTROL | SYNCHRONIZE };
                    if (hasFlag(flags, DescriptorFlags::MutateDirectory))
                        access |= FILE_WRITE_ATTRIBUTES;
                    // The host chose this directory, so the system resolves its path.
                    auto const openWith = [&](DWORD wanted) {
                        return ::CreateFileW(path.c_str(),
                                             wanted,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_FLAG_BACKUP_SEMANTICS | SECURITY_SQOS_PRESENT
                                                 | SECURITY_IDENTIFICATION,
                                             nullptr);
                    };
                    auto opened = openWith(access);
                    // A DACL that withholds READ_CONTROL opens the root without it: its access is unknown.
                    if (opened == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_ACCESS_DENIED)
                        opened = openWith(access & ~DWORD { READ_CONTROL });
                    if (opened == INVALID_HANDLE_VALUE)
                        return lastError("CreateFileW");
                    auto handle = UniqueHandle { opened };
                    auto const information = informationOf(opened);
                    if (!information)
                        return std::unexpected { information.error() };
                    if (typeOf(*information) != DescriptorType::Directory)
                        return refuse(FsErrorCode::NotDirectory);
                    return std::make_unique<WindowsDescriptor>(
                        context,
                        detail::shareHandle(std::move(handle), *context->blocking),
                        SharedHandle {},
                        grantedFlags(DescriptorType::Directory, flags),
                        DescriptorType::Directory);
                });
        }

        async::Task<FsResult<Relation>> relation(IDescriptor const& first, IDescriptor const& second) override
        {
            return detail::relationOf(_context, first, second);
        }

      private:
        SharedContext _context;
    };

} // namespace

FsResult<std::unique_ptr<IDescriptorFileSystem>> makeNativeDescriptorFileSystem(
    async::IExecutor& home,
    async::IExecutor& blocking,
    platform::IWallClock const& wallClock,
    IRandom& random,
    NativeFileSystemOptions options)
{
    return detail::checkOptions(options)
        .and_then([] { return detail::OwnerSecurity::ofProcess(); })
        .transform([&](std::shared_ptr<detail::OwnerSecurity const> security)
                       -> std::unique_ptr<IDescriptorFileSystem> {
            return std::make_unique<WindowsFileSystem>(
                std::make_shared<NativeContext const>(home,
                                                      blocking,
                                                      wallClock,
                                                      random,
                                                      options,
                                                      KernelSeparators,
                                                      &detail::systemNtFiles(),
                                                      std::move(security),
                                                      nullptr,
                                                      nullptr));
        });
}

FsResult<std::unique_ptr<IDescriptorFileSystem>> detail::makeWindowsDescriptorFileSystem(
    async::IExecutor& home,
    async::IExecutor& blocking,
    platform::IWallClock const& wallClock,
    IRandom& random,
    NativeFileSystemOptions options,
    INtFiles const& files)
{
    return detail::checkOptions(options)
        .and_then([] { return detail::OwnerSecurity::ofProcess(); })
        .transform([&](std::shared_ptr<detail::OwnerSecurity const> security)
                       -> std::unique_ptr<IDescriptorFileSystem> {
            return std::make_unique<WindowsFileSystem>(
                std::make_shared<NativeContext const>(home,
                                                      blocking,
                                                      wallClock,
                                                      random,
                                                      options,
                                                      KernelSeparators,
                                                      &files,
                                                      std::move(security),
                                                      nullptr,
                                                      nullptr));
        });
}

} // namespace core::fs
