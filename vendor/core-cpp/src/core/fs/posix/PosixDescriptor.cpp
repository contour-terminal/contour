// SPDX-License-Identifier: Apache-2.0
//
// The native descriptors of a POSIX system. Every system call is made inside async::offload, on the
// blocking executor, by a job that holds the descriptor's handle by shared ownership (decision C2),
// and the caller resumes on its home executor. Rights are checked from rightsTable() before
// anything else, inside the job. A path is resolved beneath the descriptor by detail::resolveBeneath
// (openat2 or the component walk), and the final name is acted on without following, in the
// directory the walk holds. What differs between POSIX systems is behind posix/PosixSystem.hpp.
//
// A directory stream opens the directory again (`.`, beneath its handle), so its position is its
// own, and reads it a batch per offload through posix/DirectoryRead.hpp; an entry whose type the
// system does not report is stat'ed without following, in the same job.
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
#include <core/fs/posix/DirectoryRead.hpp>
#include <core/fs/posix/MountBoundaries.hpp>
#include <core/fs/posix/OwnerOnlyCreation.hpp>
#include <core/fs/posix/PosixPathWalk.hpp>
#include <core/fs/posix/PosixSystem.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs
{

void detail::closeNativeHandle(platform::NativeHandle handle) noexcept
{
    // Not retried on EINTR: POSIX leaves the descriptor's state unspecified then, and Linux has
    // already closed it, so a retry could close a number another thread just opened.
    ::close(handle);
}

namespace
{

    using detail::NativeContext;
    using detail::refuse;
    using detail::SharedContext;
    using detail::SharedHandle;
    using detail::UniqueHandle;

    /// What the kernel splits a name on; never part of a name handed to it, whatever the policy.
    constexpr std::string_view KernelSeparators = "/";

    /// The largest offset a system call takes.
    constexpr auto OffsetMax = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());

    /// What an O_NONBLOCK open answers where it would wait: EAGAIN and EWOULDBLOCK, one value on
    /// some systems and two on others.
    constexpr auto WouldBlockErrnos = std::to_array({ EAGAIN, EWOULDBLOCK });

    /// The modes a new file and a new directory are created with, given to `openat` and `mkdirat`
    /// themselves: the umask can only take bits away, so an owner-only object is never anyone
    /// else's, not even between its creation and a `chmod`.
    struct CreationModeRow
    {
        CreationAccess access;
        ::mode_t file;
        ::mode_t directory;
    };

    constexpr auto CreationModes = std::to_array<CreationModeRow>({
        { .access = CreationAccess::Default, .file = 0666, .directory = 0777 },
        { .access = CreationAccess::OwnerOnly, .file = 0600, .directory = 0700 },
    });

    /// @return The row of @p access; the owner-only one for a value no row names, which fails closed.
    [[nodiscard]] constexpr CreationModeRow const& creationModesOf(CreationAccess access) noexcept
    {
        auto const row = std::ranges::find(CreationModes, access, &CreationModeRow::access);
        return row != CreationModes.end() ? *row : CreationModes.back();
    }

    using detail::typeOfMode;

    /// @param handle What @p status was read through, or @c platform::InvalidHandle for a stat by name.
    DescriptorStat statOf(int handle, struct ::stat const& status)
    {
        auto const times = detail::timesOf(status);
        return DescriptorStat { .type = typeOfMode(status.st_mode),
                                .linkCount = static_cast<std::uint64_t>(status.st_nlink),
                                .size = static_cast<std::uint64_t>(status.st_size),
                                .accessed = times.accessed,
                                .modified = times.modified,
                                .statusChanged = times.statusChanged,
                                .access = detail::accessFactsOf(handle, status).transform(classifyAccess) };
    }

    MetadataHash hashOf(NativeContext const& context, struct ::stat const& status) noexcept
    {
        auto const times = detail::timesOf(status);
        return detail::hashMetadata(
            context.secret,
            detail::MetadataFields { .device = static_cast<std::uint64_t>(status.st_dev),
                                     .fileIdLow = static_cast<std::uint64_t>(status.st_ino),
                                     .fileIdHigh = 0,
                                     .size = static_cast<std::uint64_t>(status.st_size),
                                     .modified = times.modified,
                                     .statusChanged = times.statusChanged });
    }

    FsResult<struct ::stat> statHandle(int handle)
    {
        struct ::stat status {};
        if (::fstat(handle, &status) != 0)
            return detail::errnoError("fstat");
        return status;
    }

    /// `fstat` of @p directory itself for an empty @p name, `fstatat` without following otherwise.
    FsResult<struct ::stat> statEntry(int directory, std::string const& name)
    {
        if (name.empty())
            return statHandle(directory);
        struct ::stat status {};
        if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
            return detail::errnoError("fstatat");
        return status;
    }

    using detail::EntryKind;
    using detail::FinalComponent;
    using detail::Resolved;
    using detail::TrailingSlashOnLink;

    /// Resolves @p path beneath @p root, following a final link where @p finalPolicy, or a trailing
    /// `/` under @p onLink, says so.
    FsResult<Resolved> resolve(int root,
                               std::string_view path,
                               NativeContext const& context,
                               SymlinkPolicy finalPolicy,
                               TrailingSlashOnLink onLink)
    {
        return detail::planRelativePath(path, *context.policy).and_then([&](detail::PathPlan plan) {
            return detail::resolveBeneath(root,
                                          std::move(plan),
                                          detail::walkRulesOf(context, finalPolicy, onLink),
                                          context.beneathOpener.get());
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

    struct OpenFlagRow
    {
        OpenFlags flag;
        int openFlag;
    };

    constexpr auto OpenFlagRows = std::to_array<OpenFlagRow>({
        { .flag = OpenFlags::Create, .openFlag = O_CREAT },
        { .flag = OpenFlags::Directory, .openFlag = O_DIRECTORY },
        { .flag = OpenFlags::Exclusive, .openFlag = O_EXCL },
        { .flag = OpenFlags::Truncate, .openFlag = O_TRUNC },
    });

    /// The `openat` flags for @p open and @p flags; the final component is never followed.
    FsResult<int> openFlagsOf(OpenFlags open, DescriptorFlags flags)
    {
        auto const reads = hasFlag(flags, DescriptorFlags::Read);
        auto const writes = hasFlag(flags, DescriptorFlags::Write);
        auto openFlags = O_NOFOLLOW | O_CLOEXEC | O_NOCTTY;
        if (writes)
            openFlags |= reads ? O_RDWR : O_WRONLY;
        else
            openFlags |= O_RDONLY;
        for (auto const& row: OpenFlagRows)
            if (hasFlag(open, row.flag))
                openFlags |= row.openFlag;
        return detail::syncOpenFlags(flags).transform([openFlags](int sync) { return openFlags | sync; });
    }

    ::timespec timespecOf(NewTimestamp const& change) noexcept
    {
        if (std::holds_alternative<NoChange>(change))
            return ::timespec { .tv_sec = 0, .tv_nsec = UTIME_OMIT };
        if (std::holds_alternative<SetToNow>(change))
            return ::timespec { .tv_sec = 0, .tv_nsec = UTIME_NOW };
        auto const time = std::get<FileTime>(change);
        return ::timespec { .tv_sec = static_cast<::time_t>(time.seconds),
                            .tv_nsec = static_cast<long>(time.nanoseconds) };
    }

    FsResult<std::array<::timespec, 2>> timespecsOf(NewTimestamp const& accessed,
                                                    NewTimestamp const& modified)
    {
        for (auto const& change: { accessed, modified })
            if (auto const* time = std::get_if<FileTime>(&change);
                time != nullptr && time->nanoseconds >= 1'000'000'000)
                return refuse(FsErrorCode::Invalid);
        return std::array { timespecOf(accessed), timespecOf(modified) };
    }

    /// Clears the O_NONBLOCK an open was made with, so reads and writes on a FIFO or a device wait
    /// as the descriptor's other calls do, on the blocking executor, where a guest asked them to.
    FsResult<void> clearNonBlocking(int handle)
    {
        auto const statusFlags = ::fcntl(handle, F_GETFL);
        if (statusFlags == -1 || ::fcntl(handle, F_SETFL, statusFlags & ~O_NONBLOCK) == -1)
            return detail::errnoError("fcntl(F_SETFL)");
        return {};
    }

    /// Opens @p name beneath @p directory without ever waiting on the way in: with O_NONBLOCK, so a
    /// FIFO opens at once whatever was renamed over the name since the walk looked at it, and the
    /// flag cleared once open. A regular file under a lease answers EWOULDBLOCK then; it is opened
    /// again, waiting for the lease, through @c detail::openAfterWouldBlock, which never opens
    /// anything but a regular file blocking.
    FsResult<UniqueHandle> openEntry(int directory, std::string const& name, int flags, ::mode_t mode)
    {
        auto const* const path = name.empty() ? "." : name.c_str();
        auto const opened =
            detail::retryOnInterrupt([&] { return ::openat(directory, path, flags | O_NONBLOCK, mode); });
        if (opened >= 0)
        {
            auto handle = UniqueHandle { opened };
            return clearNonBlocking(opened).transform([&] { return std::move(handle); });
        }
        auto const error = errno;
        if (std::ranges::find(WouldBlockErrnos, error) != WouldBlockErrnos.end())
            return detail::openAfterWouldBlock(directory, path, flags);
        return std::unexpected { detail::openFailure(directory, path, flags, error) };
    }

    FsResult<void> succeededOr(int result, char const* call)
    {
        if (result != 0)
            return detail::errnoError(call);
        return {};
    }

    class PosixDirectoryStream final: public detail::BatchedDirectoryStream
    {
      public:
        PosixDirectoryStream(SharedContext context, std::shared_ptr<detail::DirectoryReader> reader):
            _context { std::move(context) }, _reader { std::move(reader) }
        {
        }

      protected:
        async::Task<FsResult<detail::DirectoryBatch>> fetch() override
        {
            return async::offload(_context->home, _context->blocking, [reader = _reader]() {
                return detail::readDirectoryBatch(*reader);
            });
        }

      private:
        SharedContext _context;
        std::shared_ptr<detail::DirectoryReader> _reader;
    };

    class PosixDescriptor final: public detail::NativeDescriptorBase
    {
      public:
        PosixDescriptor(SharedContext context,
                        SharedHandle handle,
                        SharedHandle appendHandle,
                        DescriptorFlags flags,
                        DescriptorType type):
            NativeDescriptorBase { std::move(context), std::move(handle), flags, type },
            _appendHandle { std::move(appendHandle) },
            _appendLock { std::make_shared<std::mutex>() }
        {
        }

        async::Task<FsResult<ReadChunk>> read(std::size_t maxBytes, std::uint64_t offset) override
        {
            return submit([handle = handle(),
                           flags = flags(),
                           want = std::min(maxBytes, context()->options.maxReadChunk),
                           offset]() -> FsResult<ReadChunk> {
                return checkRights(flags, Operation::Read).and_then([&]() -> FsResult<ReadChunk> {
                    // Nothing can lie beyond the largest offset a file can have.
                    if (offset > OffsetMax)
                        return ReadChunk { .bytes = {}, .end = StreamEnd::Reached };
                    auto bytes = std::vector<std::byte>(want);
                    auto const count = detail::retryOnInterrupt([&] {
                        return ::pread(handle->get(), bytes.data(), bytes.size(), static_cast<off_t>(offset));
                    });
                    if (count < 0)
                        return detail::errnoError("pread");
                    bytes.resize(static_cast<std::size_t>(count));
                    auto const end = bytes.size() < want ? StreamEnd::Reached : StreamEnd::More;
                    return ReadChunk { .bytes = std::move(bytes), .end = end };
                });
            });
        }

        async::Task<FsResult<std::size_t>> write(std::vector<std::byte> bytes, std::uint64_t offset) override
        {
            return submit([handle = handle(), flags = flags(), bytes = std::move(bytes), offset]()
                              -> FsResult<std::size_t> {
                return checkRights(flags, Operation::Write).and_then([&]() -> FsResult<std::size_t> {
                    if (offset > OffsetMax || bytes.size() > OffsetMax - offset)
                        return refuse(FsErrorCode::FileTooLarge);
                    auto written = std::size_t { 0 };
                    while (written < bytes.size())
                    {
                        auto const count = detail::retryOnInterrupt([&] {
                            return ::pwrite(handle->get(),
                                            bytes.data() + written,
                                            bytes.size() - written,
                                            static_cast<off_t>(offset + written));
                        });
                        if (count < 0)
                        {
                            if (written > 0)
                                break;
                            return detail::errnoError("pwrite");
                        }
                        if (count == 0)
                            break;
                        written += static_cast<std::size_t>(count);
                    }
                    return written;
                });
            });
        }

        async::Task<FsResult<std::size_t>> append(std::vector<std::byte> bytes) override
        {
            return submit([handle = handle(),
                           appendHandle = _appendHandle,
                           appendLock = _appendLock,
                           flags = flags(),
                           bytes = std::move(bytes)]() {
                return checkRights(flags, Operation::Append).and_then([&] {
                    return detail::appendBytes(handle->get(), appendHandle.get(), *appendLock, bytes);
                });
            });
        }

        async::Task<FsResult<void>> advise(std::uint64_t offset, std::uint64_t length, Advice advice) override
        {
            return submit([handle = handle(), flags = flags(), offset, length, advice]() {
                return checkRights(flags, Operation::Advise).and_then([&] {
                    return detail::adviseRange(handle->get(), offset, length, advice);
                });
            });
        }

        async::Task<FsResult<void>> syncData() override
        {
            return submit([handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Sync).and_then([&] {
                    return detail::syncData(handle->get());
                });
            });
        }

        async::Task<FsResult<void>> sync() override
        {
            return submit([handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Sync).and_then([&] {
                    return detail::syncAll(handle->get());
                });
            });
        }

        async::Task<FsResult<DescriptorType>> type() override
        {
            return submit([handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Stat)
                    .and_then([&] { return statHandle(handle->get()); })
                    .transform([](struct ::stat const& status) { return typeOfMode(status.st_mode); });
            });
        }

        async::Task<FsResult<void>> setSize(std::uint64_t size) override
        {
            return submit([handle = handle(), flags = flags(), size]() {
                return checkRights(flags, Operation::SetSize).and_then([&]() -> FsResult<void> {
                    if (size > OffsetMax)
                        return refuse(FsErrorCode::FileTooLarge);
                    return succeededOr(detail::retryOnInterrupt([&] {
                                           return ::ftruncate(handle->get(), static_cast<off_t>(size));
                                       }),
                                       "ftruncate");
                });
            });
        }

        async::Task<FsResult<void>> setTimes(NewTimestamp accessed, NewTimestamp modified) override
        {
            return submit([handle = handle(), flags = flags(), accessed, modified]() {
                return checkRights(flags, Operation::SetTimes)
                    .and_then([&] { return timespecsOf(accessed, modified); })
                    .and_then([&](std::array<::timespec, 2> const& times) {
                        return succeededOr(::futimens(handle->get(), times.data()), "futimens");
                    });
            });
        }

        async::Task<FsResult<DescriptorStat>> stat() override
        {
            return submit([handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::Stat)
                    .and_then([&] { return statHandle(handle->get()); })
                    .transform([&](struct ::stat const& status) { return statOf(handle->get(), status); });
            });
        }

        async::Task<FsResult<MetadataHash>> metadataHash() override
        {
            return submit([context = context(), handle = handle(), flags = flags()]() {
                return checkRights(flags, Operation::MetadataHash)
                    .and_then([&] { return statHandle(handle->get()); })
                    .transform([&](struct ::stat const& status) { return hashOf(*context, status); });
            });
        }

        async::Task<FsResult<std::unique_ptr<IDirectoryStream>>> readDirectory() override
        {
            return submit([context = context(), handle = handle(), flags = flags(), type = objectType()]() {
                return checkRights(flags, Operation::ReadDirectory)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] { return detail::openDirectoryReader(handle->get(), *context->blocking); })
                    .transform([&](std::shared_ptr<detail::DirectoryReader> reader)
                                   -> std::unique_ptr<IDirectoryStream> {
                        return std::make_unique<PosixDirectoryStream>(context, std::move(reader));
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
                // O_CREAT | O_EXCL never follows a final link: a dangling one is Exist rather than
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
                // A link not followed is Loop, decided here as Windows decides it, before the kernel
                // answers in its own words: FreeBSD's O_NOFOLLOW says EMLINK, which is TooManyLinks.
                if (!exclusive && resolved->kind == EntryKind::SymbolicLink)
                    return refuse(FsErrorCode::Loop);
                auto openFlags = openFlagsOf(open, childFlags);
                if (!openFlags)
                    return std::unexpected { openFlags.error() };
                if (resolved->finalComponent == FinalComponent::Directory && !resolved->name.empty())
                {
                    // `name/` opens nothing but a directory, and creates none: the kernel is handed
                    // the name without its `/`, so the walk's answer is enforced here.
                    if (resolved->kind == EntryKind::Missing && hasFlag(open, OpenFlags::Create))
                        return refuse(FsErrorCode::NotDirectory);
                    *openFlags |= O_DIRECTORY;
                }
                auto const directory = resolved->directory();
                auto const& name = resolved->name;
                auto const creating =
                    hasFlag(open, OpenFlags::OwnerOnly) ? CreationAccess::OwnerOnly : CreationAccess::Default;
                // OwnerOnly comes with Create | Exclusive (checkOpenRights): this call makes the file.
                auto child = creating == CreationAccess::OwnerOnly
                                 ? context->ownerOnlyCreator->createFile(directory, name, *openFlags)
                                 : openEntry(directory, name, *openFlags, creationModesOf(creating).file);
                if (!child)
                    return std::unexpected { child.error() };
                auto const opened = child->get();
                auto const status = statHandle(opened);
                if (!status)
                    return std::unexpected { status.error() };
                auto const childType = typeOfMode(status->st_mode);
                // Create opens no directory: Linux refuses it with EISDIR, but macOS opens one
                // `O_CREAT | O_RDONLY` without a word. The handle closes here, unshared.
                if (hasFlag(open, OpenFlags::Create) && !hasFlag(open, OpenFlags::Directory)
                    && childType == DescriptorType::Directory)
                    return refuse(FsErrorCode::IsDirectory);
                auto appendHandle = SharedHandle {};
                if (hasFlag(childFlags, DescriptorFlags::Write) && childType == DescriptorType::RegularFile)
                {
                    // The append handle writes with the same integrity as the first.
                    auto second = detail::openAppendHandle(
                        directory,
                        name,
                        opened,
                        *openFlags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_DIRECTORY));
                    if (!second)
                        return std::unexpected { second.error() };
                    if (second->get() != platform::InvalidHandle)
                        appendHandle = detail::shareHandle(std::move(*second), *context->blocking);
                }
                return std::make_unique<PosixDescriptor>(
                    context,
                    detail::shareHandle(std::move(*child), *context->blocking),
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
                    .and_then([&context, access](Resolved const& resolved) -> FsResult<void> {
                        if (access == CreationAccess::OwnerOnly)
                            return context->ownerOnlyCreator->createDirectory(resolved.directory(),
                                                                              resolved.name);
                        return succeededOr(::mkdirat(resolved.directory(),
                                                     resolved.name.c_str(),
                                                     creationModesOf(access).directory),
                                           "mkdirat");
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
                    .and_then([](Resolved const& resolved) {
                        // The directory itself is stat'ed through its handle; a name, by name.
                        auto const through =
                            resolved.name.empty() ? resolved.directory() : platform::InvalidHandle;
                        return statEntry(resolved.directory(), resolved.name)
                            .transform(
                                [through](struct ::stat const& status) { return statOf(through, status); });
                    });
            });
        }

        async::Task<FsResult<void>> setTimesAt(SymlinkPolicy policy,
                                               std::string path,
                                               NewTimestamp accessed,
                                               NewTimestamp modified) override
        {
            return submit([context = context(),
                           handle = handle(),
                           flags = flags(),
                           type = objectType(),
                           policy,
                           path = std::move(path),
                           accessed,
                           modified]() {
                return checkRights(flags, Operation::SetTimesAt)
                    .and_then([&] { return detail::requireDirectory(type); })
                    .and_then([&] {
                        return resolve(handle->get(), path, *context, policy, TrailingSlashOnLink::Follow);
                    })
                    .and_then([&](Resolved const& resolved) -> FsResult<void> {
                        auto const times = timespecsOf(accessed, modified);
                        if (!times)
                            return std::unexpected { times.error() };
                        // The directory itself is named by `.` beneath its own handle, which may be a
                        // path handle (O_PATH) that futimens refuses; `.` is never a link.
                        auto const* const name = resolved.name.empty() ? "." : resolved.name.c_str();
                        return succeededOr(
                            ::utimensat(resolved.directory(), name, times->data(), AT_SYMLINK_NOFOLLOW),
                            "utimensat");
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
                if (from->name.empty())
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
                return succeededOr(
                    ::linkat(from->directory(), from->name.c_str(), to->directory(), to->name.c_str(), 0),
                    "linkat");
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
                    .and_then([](Resolved const& resolved) -> FsResult<std::string> {
                        if (resolved.name.empty())
                            return refuse(FsErrorCode::Invalid);
                        return detail::readLinkAt(resolved.directory(), resolved.name);
                    })
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
                    .and_then([](Resolved const& resolved) -> FsResult<void> {
                        if (resolved.name.empty())
                            return refuse(FsErrorCode::Invalid);
                        return succeededOr(
                            ::unlinkat(resolved.directory(), resolved.name.c_str(), AT_REMOVEDIR),
                            "unlinkat");
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
                // `to/` names a directory: the kernel sees the name without its `/`.
                if (from->kind != EntryKind::Directory && to->finalComponent == FinalComponent::Directory)
                    return refuse(FsErrorCode::NotDirectory);
                return succeededOr(
                    ::renameat(from->directory(), from->name.c_str(), to->directory(), to->name.c_str()),
                    "renameat");
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
                        return succeededOr(
                            ::symlinkat(target.c_str(), resolved.directory(), resolved.name.c_str()),
                            "symlinkat");
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
                    .and_then([](Resolved const& resolved) -> FsResult<void> {
                        // A directory is IsDirectory on every system: macOS and the BSDs would answer
                        // EPERM for it, where Linux says EISDIR.
                        if (resolved.name.empty() || resolved.kind == EntryKind::Directory)
                            return refuse(FsErrorCode::IsDirectory);
                        return succeededOr(::unlinkat(resolved.directory(), resolved.name.c_str(), 0),
                                           "unlinkat");
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
                    .and_then([](Resolved const& resolved) {
                        return statEntry(resolved.directory(), resolved.name);
                    })
                    .transform([&](struct ::stat const& status) { return hashOf(*context, status); });
            });
        }

        async::Task<FsResult<bool>> isSameObject(IDescriptor* other) override
        {
            return submit([handle = handle(), candidate = sameObjectCandidate(other)]() -> FsResult<bool> {
                if (!candidate)
                    return std::unexpected { candidate.error() };
                if (!*candidate)
                    return false;
                return statHandle(handle->get()).and_then([&](struct ::stat const& mine) {
                    return statHandle((*candidate)->handle->get())
                        .transform([&](struct ::stat const& theirs) {
                            return mine.st_dev == theirs.st_dev && mine.st_ino == theirs.st_ino;
                        });
                });
            });
        }

      private:
        SharedHandle _appendHandle; ///< The second handle `append` writes through, where one is needed.
        /// Held by an append that finds the end itself; shared with the jobs, which may outlive the
        /// descriptor.
        std::shared_ptr<std::mutex> _appendLock;
    };

    class PosixFileSystem final: public IDescriptorFileSystem
    {
      public:
        explicit PosixFileSystem(SharedContext context): _context { std::move(context) } {}

        async::Task<FsResult<std::unique_ptr<IDescriptor>>> openPreopen(std::filesystem::path hostDirectory,
                                                                        DescriptorFlags flags) override
        {
            return async::offload(_context->home,
                                  _context->blocking,
                                  [context = _context,
                                   path = std::move(hostDirectory),
                                   flags]() -> FsResult<std::unique_ptr<IDescriptor>> {
                                      if (hasFlag(flags, DescriptorFlags::Write))
                                          return refuse(FsErrorCode::IsDirectory);
                                      // The host chose this directory, so the kernel resolves its path;
                                      // O_PATH is not used, because the preopen itself is read and stat'ed.
                                      auto const opened = detail::retryOnInterrupt([&] {
                                          return ::open(path.c_str(),
                                                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
                                      });
                                      if (opened < 0)
                                          return detail::errnoError("open");
                                      return std::make_unique<PosixDescriptor>(
                                          context,
                                          detail::shareHandle(UniqueHandle { opened }, *context->blocking),
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
    return detail::checkOptions(options).transform([&] -> std::unique_ptr<IDescriptorFileSystem> {
        return std::make_unique<PosixFileSystem>(std::make_shared<NativeContext const>(
            home,
            blocking,
            wallClock,
            random,
            options,
            KernelSeparators,
            nullptr,
            nullptr,
            std::make_shared<detail::OwnerOnlyCreator const>(random, detail::nativeOwnerOnlyCreation()),
            detail::makeMountBoundaries()));
    });
}

} // namespace core::fs
