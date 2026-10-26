// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/GitHead.hpp>

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <core/Utf8Path.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/StopToken.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
    #include <sys/stat.h>
#endif

namespace vtbackend
{

namespace
{
    /// One form of `ref: <ref>` a HEAD may hold: the prefix it starts with, and what it then names.
    struct RefForm
    {
        std::string_view prefix; ///< Removed from the shown name.
        GitHeadKind kind;        ///< What the rest names.
    };

    /// The ref forms, most specific first: the first whose prefix matches decides.
    constexpr auto RefForms = std::array {
        RefForm { .prefix = "refs/heads/", .kind = GitHeadKind::Branch },
        RefForm { .prefix = "refs/", .kind = GitHeadKind::OtherRef },
    };

    /// The lengths of an object id in hex digits: SHA-1, and SHA-256 repositories.
    constexpr auto ObjectIdLengths = std::array<std::size_t, 2> { 40, 64 };

    constexpr auto Whitespace = std::string_view { " \t\r\n" };

    /// @p text without its trailing whitespace.
    [[nodiscard]] std::string_view trimEnd(std::string_view text) noexcept
    {
        auto const end = text.find_last_not_of(Whitespace);
        return end == std::string_view::npos ? std::string_view {} : text.substr(0, end + 1);
    }

    /// @p text without its leading whitespace.
    [[nodiscard]] std::string_view trimStart(std::string_view text) noexcept
    {
        auto const start = text.find_first_not_of(Whitespace);
        return start == std::string_view::npos ? std::string_view {} : text.substr(start);
    }

    /// Whether @p text is an object id: 40 or 64 hex digits.
    [[nodiscard]] bool isObjectId(std::string_view text) noexcept
    {
        return std::ranges::contains(ObjectIdLengths, text.size()) && std::ranges::all_of(text, [](char c) {
                   return std::isxdigit(static_cast<unsigned char>(c));
               });
    }

    /// The HEAD a `.git` FILE points at (a worktree's or a submodule's), or nullopt when it says no
    /// `gitdir:`.
    [[nodiscard]] std::optional<std::filesystem::path> headOfGitFile(GitFileSystem const& fileSystem,
                                                                     std::filesystem::path const& dotGit)
    {
        constexpr auto GitDirPrefix = std::string_view { "gitdir:" };

        auto const contents = fileSystem.readMetadata(dotGit);
        if (!contents)
            return std::nullopt;
        auto const firstLine = trimEnd(std::string_view { *contents }.substr(0, contents->find('\n')));
        if (!firstLine.starts_with(GitDirPrefix))
            return std::nullopt;
        auto const target = trimStart(firstLine.substr(GitDirPrefix.size()));
        if (target.empty())
            return std::nullopt;
        auto gitDir = core::pathFromUtf8(target);
        if (!gitDir)
            return std::nullopt;
        // Relative to the directory holding the .git file, as git reads it: a submodule's
        // "../.git/modules/s".
        return (dotGit.parent_path() / *gitDir).lexically_normal() / "HEAD";
    }
} // namespace

std::optional<GitHead> parseGitHead(std::string_view contents)
{
    constexpr auto RefPrefix = std::string_view { "ref:" };

    auto const text = trimEnd(contents);
    if (text.starts_with(RefPrefix))
    {
        auto const ref = trimStart(text.substr(RefPrefix.size()));
        // The first form whose prefix matches decides: "refs/heads/" with no name is no branch, not a ref.
        if (auto const form =
                std::ranges::find_if(RefForms, [&](RefForm const& f) { return ref.starts_with(f.prefix); });
            form != RefForms.end() && ref.size() > form->prefix.size())
        {
            auto name = ref.substr(form->prefix.size());
            // reftable's stub (see ReftableStubBranch): the branch is elsewhere, so there is none here.
            if (form->kind == GitHeadKind::Branch && name == ReftableStubBranch)
                return std::nullopt;
            return GitHead { .kind = form->kind, .name = std::string { name } };
        }
        return std::nullopt;
    }

    if (isObjectId(text))
        return GitHead { .kind = GitHeadKind::Detached,
                         .name = std::string { text.substr(0, DetachedHeadDigits) } };

    return std::nullopt;
}

GitPathStatus NativeGitFileSystem::status(std::filesystem::path const& path) const
{
#ifdef _WIN32
    // One query, as stat(2) is one; Windows names no device through it, so no walk stops at a volume.
    auto error = std::error_code {};
    auto const found = std::filesystem::status(path, error);
    switch (found.type())
    {
        case std::filesystem::file_type::not_found: return GitPathStatus { .kind = GitPathKind::Absent };
        case std::filesystem::file_type::directory: return GitPathStatus { .kind = GitPathKind::Directory };
        case std::filesystem::file_type::regular: return GitPathStatus { .kind = GitPathKind::RegularFile };
        default: return GitPathStatus { .kind = GitPathKind::Other };
    }
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0)
        // Nothing there, or a component that is no directory: the walk goes on. Any other failure -- a
        // permission, an I/O error -- is something there that cannot be read, which ends it as git's does.
        return GitPathStatus { .kind = errno == ENOENT || errno == ENOTDIR ? GitPathKind::Absent
                                                                           : GitPathKind::Other };
    auto const device = static_cast<std::uint64_t>(info.st_dev);
    if (S_ISDIR(info.st_mode))
        return GitPathStatus { .kind = GitPathKind::Directory, .device = device };
    if (S_ISREG(info.st_mode))
        return GitPathStatus { .kind = GitPathKind::RegularFile, .device = device };
    return GitPathStatus { .kind = GitPathKind::Other, .device = device };
#endif
}

std::optional<std::string> NativeGitFileSystem::readMetadata(std::filesystem::path const& path) const
{
    auto const size = _files.fileSize(path);
    if (!size || *size > MaxGitMetadataSize || !_files.isRegularFile(path))
        return std::nullopt;
    auto contents = _files.readFile(path);
    if (!contents)
        return std::nullopt;
    return *std::move(contents);
}

std::optional<std::filesystem::path> findGitHeadFile(GitFileSystem const& fileSystem,
                                                     std::filesystem::path const& directory)
{
    // A relative path would be looked up against this process's directory, which is not the shell's.
    if (!directory.has_root_directory())
        return std::nullopt;

    auto current = directory.lexically_normal();
    auto const start = fileSystem.status(current);
    if (start.kind != GitPathKind::Directory)
        return std::nullopt;

    while (true)
    {
        auto const dotGit = current / ".git";
        switch (fileSystem.status(dotGit).kind)
        {
            case GitPathKind::Directory: {
                auto head = dotGit / "HEAD";
                if (fileSystem.status(head).kind != GitPathKind::RegularFile)
                    return std::nullopt;
                return head;
            }
            case GitPathKind::RegularFile: return headOfGitFile(fileSystem, dotGit);
            // Anything else named .git stops the walk, as git's own discovery does.
            case GitPathKind::Other: return std::nullopt;
            case GitPathKind::Absent: break;
        }

        auto parent = current.parent_path();
        if (parent == current || parent.empty())
            return std::nullopt;
        // Not into another filesystem: a parent on a network or automount root is where a stat can hang, or
        // (`/net/.git`) ask a name service about a host called ".git".
        if (fileSystem.status(parent).device != start.device)
            return std::nullopt;
        current = std::move(parent);
    }
}

std::string describeGitHead(GitFileSystem const& fileSystem, std::filesystem::path const& directory)
{
    auto const head = findGitHeadFile(fileSystem, directory);
    if (!head)
        return {};
    // A reftable repository keeps its refs, HEAD's target included, in <gitdir>/reftable/; its HEAD file is a
    // stub that never changes. Decoding reftable is left for later: such a repository shows no branch.
    if (fileSystem.status(head->parent_path() / "reftable").kind == GitPathKind::Directory)
        return {};
    auto const contents = fileSystem.readMetadata(*head);
    if (!contents)
        return {};
    auto const parsed = parseGitHead(*contents);
    if (!parsed)
        return {};
    // A ref name may hold any byte git allows in a path, an escape included, and this text is written to
    // the status screen, which parses it.
    auto name = sanitizeCommandLine(parsed->name, SanitizePurpose::Display);
    switch (parsed->kind)
    {
        case GitHeadKind::Branch:
        case GitHeadKind::OtherRef: return name;
        // In parentheses, as git's own prompt shows one, so a commit does not read as a branch of that name.
        case GitHeadKind::Detached: return std::format("({})", name);
    }
    return name;
}

namespace
{
    /// Whether the cache may answer for its directory without looking.
    enum class GitHeadFreshness : uint8_t
    {
        Stale = 0, ///< Look on the next refresh().
        Fresh,     ///< Nothing invalidated it since a lookup for its directory started.
    };

    /// Whether a lookup is running.
    enum class GitLookup : uint8_t
    {
        Idle = 0, ///< None: a refresh that needs one starts it.
        InFlight, ///< One is queued or running, and looks again if what it was asked changed meanwhile.
    };
} // namespace

struct GitHeadCache::State
{
    State(GitHeadServices services, std::function<void()> onBranchChanged):
        services { services }, onBranchChanged { std::move(onBranchChanged) }
    {
    }

    /// Looks up what was last asked, on the executor and without any lock while it reads, until what it
    /// answered is still what is asked. A static member with a by-value parameter: the frame outlives the
    /// call that started it, and maybe the cache.
    static core::async::DetachedTask lookUp(std::shared_ptr<State> state)
    {
        co_await core::async::ResumeOn { state->services.executor };
        while (!state->stop.stop_requested())
        {
            auto asked = std::optional<std::filesystem::path> {};
            auto generation = std::uint64_t {};
            {
                auto const lock = std::scoped_lock { state->mutex };
                asked = state->directory;
                generation = state->generation;
            }
            auto answer = asked ? describeGitHead(state->services.fileSystem, *asked) : std::string {};

            auto const lock = std::scoped_lock { state->mutex };
            if (generation != state->generation)
                continue; // Asked again meanwhile: look again.
            state->lookup = GitLookup::Idle;
            if (answer != state->branch)
            {
                state->branch = std::move(answer);
                // Under the lock on purpose: the destructor clears the callback under it, so it waits for
                // this.
                if (state->onBranchChanged)
                    state->onBranchChanged();
            }
            co_return;
        }
        auto const lock = std::scoped_lock { state->mutex };
        state->lookup = GitLookup::Idle;
    }

    GitHeadServices services;
    std::mutex mutex;
    std::function<void()> onBranchChanged;
    core::async::StopSource stop;
    GitHeadFreshness freshness = GitHeadFreshness::Stale;
    GitLookup lookup = GitLookup::Idle;
    std::optional<std::filesystem::path> directory;
    std::uint64_t generation = 0; ///< Moves with every question asked of the lookup.
    std::string branch;
};

GitHeadCache::GitHeadCache(GitHeadServices services, std::function<void()> onBranchChanged):
    _state { std::make_shared<State>(services, std::move(onBranchChanged)) }
{
}

GitHeadCache::~GitHeadCache()
{
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->onBranchChanged = nullptr;
    }
    _state->stop.request_stop();
}

void GitHeadCache::invalidate() noexcept
{
    auto const lock = std::scoped_lock { _state->mutex };
    _state->freshness = GitHeadFreshness::Stale;
}

void GitHeadCache::refresh(std::optional<std::filesystem::path> const& directory)
{
    {
        auto const lock = std::scoped_lock { _state->mutex };
        if (_state->freshness == GitHeadFreshness::Fresh && directory == _state->directory)
            return;
        _state->freshness = GitHeadFreshness::Fresh;
        _state->directory = directory;
        ++_state->generation;
        if (!directory)
        {
            // Nothing to look at, so nothing to wait for. A lookup in flight sees the new question and stops.
            _state->branch.clear();
            return;
        }
        if (_state->lookup == GitLookup::InFlight)
            return; // It sees the new generation, and looks again.
        _state->lookup = GitLookup::InFlight;
    }
    (void) State::lookUp(_state);
}

std::string GitHeadCache::branch() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    return _state->branch;
}

} // namespace vtbackend
