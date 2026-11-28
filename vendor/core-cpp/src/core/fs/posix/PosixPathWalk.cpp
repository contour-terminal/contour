// SPDX-License-Identifier: Apache-2.0
//
// The component walker of every POSIX system: each directory is opened beneath the one before it
// with O_NOFOLLOW and held by its descriptor, so the walk never names a directory by a path again.
// A component the open refuses as a link (an errno of noFollowSymlinkErrnos(), or ENOTDIR where
// O_PATH opened the link and O_DIRECTORY refused it) is read with readlinkat and spliced by the
// shared walk, which also owns the hop limit and `..` at the root.
#include <core/fs/posix/PosixPathWalk.hpp>

#include <core/fs/FsError.hpp>
#include <core/fs/detail/OwnedHandle.hpp>
#include <core/fs/detail/PathWalk.hpp>
#include <core/fs/detail/ResolveBeneath.hpp>
#include <core/fs/posix/PosixSystem.hpp>
#include <core/platform/Types.hpp>

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace core::fs::detail
{

namespace
{

    /// @return Whether @p error is how this system refuses to open a link it was told not to follow.
    bool refusedAsLink(int error) noexcept
    {
        return error == ENOTDIR
               || std::ranges::find(noFollowSymlinkErrnos(), error) != noFollowSymlinkErrnos().end();
    }

    EntryKind kindOf(::mode_t mode) noexcept
    {
        if (S_ISDIR(mode))
            return EntryKind::Directory;
        if (S_ISLNK(mode))
            return EntryKind::SymbolicLink;
        return EntryKind::Other;
    }

} // namespace

FsResult<std::string> readLinkAt(int directory, std::string const& name)
{
    auto target = std::string(256, '\0');
    while (true)
    {
        auto const length = ::readlinkat(directory, name.c_str(), target.data(), target.size());
        if (length < 0)
            return errnoError("readlinkat");
        if (static_cast<std::size_t>(length) < target.size())
        {
            target.resize(static_cast<std::size_t>(length));
            return target;
        }
        target.resize(target.size() * 2);
    }
}

PosixCursor::PosixCursor(int root, IBeneathOpener const* opener) noexcept: _root { root }, _opener { opener }
{
}

bool PosixCursor::atRoot() const noexcept
{
    return _entered.empty();
}

int PosixCursor::current() const noexcept
{
    return _entered.empty() ? _root : _entered.back().get();
}

FsResult<EntryKind> PosixCursor::enter(std::string const& name)
{
    auto const opened =
        retryOnInterrupt([&] { return ::openat(current(), name.c_str(), walkDirectoryOpenFlags()); });
    if (opened >= 0)
    {
        _entered.emplace_back(opened);
        return EntryKind::Directory;
    }
    auto const failure = errno;
    if (!refusedAsLink(failure))
        return std::unexpected { fsErrorFromSystemCode(failure, "openat") };
    // ENOTDIR is a link only where it is one: a file in the way stays NotDirectory.
    return inspect(name).and_then([failure](EntryKind kind) -> FsResult<EntryKind> {
        switch (kind)
        {
            case EntryKind::Missing: return std::unexpected { fsErrorFromSystemCode(ENOENT, "openat") };
            // A directory here changed under the walk since the open refused it: reported as a
            // link, whose read answers Invalid, so the walk looks at the name again.
            case EntryKind::Directory:
            case EntryKind::SymbolicLink: return EntryKind::SymbolicLink;
            case EntryKind::Other: break;
        }
        return std::unexpected { fsErrorFromSystemCode(failure, "openat") };
    });
}

void PosixCursor::leave()
{
    _entered.pop_back();
}

FsResult<EntryKind> PosixCursor::inspect(std::string const& name)
{
    struct ::stat status {};
    if (::fstatat(current(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
    {
        if (errno == ENOENT)
            return EntryKind::Missing;
        return errnoError("fstatat");
    }
    return kindOf(status.st_mode);
}

FsResult<std::string> PosixCursor::readLink(std::string const& name)
{
    return readLinkAt(current(), name);
}

FsResult<Shortcut> PosixCursor::shortcut(std::span<std::string const> components)
{
    if (_opener == nullptr)
        return Shortcut::Declined;
    auto path = std::string {};
    for (auto const& component: components)
    {
        if (!path.empty())
            path += '/';
        path += component;
    }
    return _opener->openDirectoryBeneath(_root, path).transform([this](UniqueHandle opened) {
        if (opened.get() == platform::InvalidHandle)
            return Shortcut::Declined;
        _entered.clear();
        _entered.push_back(std::move(opened));
        return Shortcut::Taken;
    });
}

void PosixCursor::restart()
{
    _entered.clear();
}

UniqueHandle PosixCursor::release() noexcept
{
    if (_entered.empty())
        return UniqueHandle { platform::InvalidHandle };
    auto top = std::move(_entered.back());
    _entered.clear();
    return top;
}

FsResult<Resolved> resolveBeneath(platform::NativeHandle root,
                                  PathPlan plan,
                                  WalkRules const& rules,
                                  IBeneathOpener const* opener)
{
    auto cursor = PosixCursor { root, opener };
    return walkBeneath(cursor, std::move(plan), rules).transform([&cursor, root](WalkEnd end) {
        return Resolved { .held = cursor.release(),
                          .root = root,
                          .name = std::move(end.name),
                          .kind = end.kind,
                          .finalComponent = end.finalComponent };
    });
}

} // namespace core::fs::detail
