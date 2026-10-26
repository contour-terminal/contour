// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/screen/GitHead.hpp>

#include <vtbackend/shell/CommandLineSanitizer.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

    /// The contents of @p path when it is a regular file of at most MaxGitMetadataSize bytes, else nullopt.
    [[nodiscard]] std::optional<std::string> readGitMetadata(core::platform::FileSystem const& fileSystem,
                                                             std::filesystem::path const& path)
    {
        auto const size = fileSystem.fileSize(path);
        if (!size || *size > MaxGitMetadataSize)
            return std::nullopt;
        auto contents = fileSystem.readFile(path);
        if (!contents)
            return std::nullopt;
        return std::move(*contents);
    }

    /// The HEAD a `.git` FILE points at (a worktree's or a submodule's), or nullopt when it says no
    /// `gitdir:`.
    [[nodiscard]] std::optional<std::filesystem::path> headOfGitFile(
        core::platform::FileSystem const& fileSystem, std::filesystem::path const& dotGit)
    {
        constexpr auto GitDirPrefix = std::string_view { "gitdir:" };

        auto const contents = readGitMetadata(fileSystem, dotGit);
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
        for (auto const& form: RefForms)
            if (ref.starts_with(form.prefix) && ref.size() > form.prefix.size())
                return GitHead { .kind = form.kind, .name = std::string { ref.substr(form.prefix.size()) } };
        return std::nullopt;
    }

    if (isObjectId(text))
        return GitHead { .kind = GitHeadKind::Detached,
                         .name = std::string { text.substr(0, DetachedHeadDigits) } };

    return std::nullopt;
}

std::optional<std::filesystem::path> findGitHeadFile(core::platform::FileSystem const& fileSystem,
                                                     std::filesystem::path const& directory)
{
    // A relative path would be looked up against this process's directory, which is not the shell's.
    if (!directory.has_root_directory())
        return std::nullopt;

    auto current = directory.lexically_normal();
    while (true)
    {
        auto const dotGit = current / ".git";
        if (fileSystem.isDirectory(dotGit))
        {
            auto head = dotGit / "HEAD";
            if (!fileSystem.isRegularFile(head))
                return std::nullopt;
            return head;
        }
        if (fileSystem.isRegularFile(dotGit))
            return headOfGitFile(fileSystem, dotGit);
        // Anything else named .git stops the walk, as git's own discovery does.
        if (fileSystem.exists(dotGit))
            return std::nullopt;

        auto parent = current.parent_path();
        if (parent == current || parent.empty())
            return std::nullopt;
        current = std::move(parent);
    }
}

GitHeadCache::GitHeadCache(core::platform::FileSystem const& fileSystem): _fileSystem { fileSystem }
{
}

void GitHeadCache::invalidate() noexcept
{
    _freshness = GitHeadFreshness::Stale;
}

void GitHeadCache::refresh(std::optional<std::filesystem::path> const& directory)
{
    if (_freshness == GitHeadFreshness::Fresh && directory == _directory)
        return;
    _freshness = GitHeadFreshness::Fresh;
    _directory = directory;

    auto key = directory ? keyFor(*directory) : std::nullopt;
    if (key == _key)
        return; // The same HEAD, unchanged since it was read: nothing to read.

    _branch = key ? branchIn(key->head) : std::string {};
    _key = std::move(key);
}

std::optional<GitHeadCache::HeadKey> GitHeadCache::keyFor(std::filesystem::path const& directory) const
{
    auto head = findGitHeadFile(_fileSystem, directory);
    if (!head)
        return std::nullopt;
    auto const mtime = _fileSystem.lastWriteTime(*head);
    if (!mtime)
        return std::nullopt;
    return HeadKey { .head = *std::move(head), .mtime = *mtime };
}

std::string GitHeadCache::branchIn(std::filesystem::path const& head) const
{
    auto const contents = readGitMetadata(_fileSystem, head);
    if (!contents)
        return {};
    auto const parsed = parseGitHead(*contents);
    if (!parsed)
        return {};
    // A ref name may hold any byte git allows in a path, an escape included, and this text is written to
    // the status screen, which parses it.
    return sanitizeCommandLine(parsed->name, SanitizePurpose::Display);
}

} // namespace vtbackend
