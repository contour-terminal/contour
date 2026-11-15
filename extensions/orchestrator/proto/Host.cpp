// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Host.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <glaze/glaze.hpp>
#include <orchestrator/proto/Environment.hpp>
#include <orchestrator/proto/HostJson.hpp>

// The process environment: POSIX declares it, no header does.
extern char** environ;

template <>
struct glz::meta<orchestrator::proto::detail::JsonConfigPaths>
{
    using T = orchestrator::proto::detail::JsonConfigPaths;
    // Glaze finds the table by this spelling, as the standard library finds `value_type`.
    // NOLINTNEXTLINE(readability-identifier-naming)
    static constexpr auto value = glz::object("worktree_root", &T::worktreeRoot, "repos", &T::repos);
};

namespace orchestrator::proto
{

namespace fs = std::filesystem;

namespace
{
    constexpr auto ReadOptions = glz::opts { .null_terminated = false, .error_on_unknown_keys = false };

    /// @return @p path with a leading `~/` replaced by @p home.
    [[nodiscard]] std::string expandHome(std::string_view path, std::string_view home)
    {
        if (path == "~" || path.starts_with("~/"))
            return std::format("{}{}", home, path.substr(1));
        return std::string(path);
    }

    /// @return @p path, expanded and resolved; or a message naming it.
    [[nodiscard]] std::expected<std::string, std::string> resolved(std::string_view path,
                                                                   std::string_view home)
    {
        auto error = std::error_code {};
        auto canonical = fs::canonical(expandHome(path, home), error);
        if (error)
            return std::unexpected { std::format("cannot resolve {}: {}", path, error.message()) };
        return canonical.string();
    }
} // namespace

std::vector<std::string> inheritedEnvironment()
{
    auto count = std::size_t { 0 };
    while (environ != nullptr && environ[count] != nullptr)
        ++count;
    auto environment = std::vector<std::string> {};
    for (auto const* const entry: std::span(environ, count))
        environment.emplace_back(entry);
    return withoutWithheld(environment);
}

std::expected<HostInfo, std::string> resolveHost(std::string_view configJson,
                                                 std::span<std::string const> environment)
{
    auto paths = detail::JsonConfigPaths {};
    if (glz::read<ReadOptions>(paths, configJson))
        return std::unexpected { std::string("the configuration is not JSON of the expected shape") };
    auto const home = lookup(environment, "HOME");
    if (!home)
        return std::unexpected { std::string("HOME is not set") };
    auto host = HostInfo {
        .home = *home, .worktreeRoot = {}, .repoPaths = {}, .utcOffsetMinutes = utcOffsetMinutes()
    };
    if (paths.worktreeRoot)
    {
        // Created before anything runs in it: the first @worktrees command needs it to exist.
        auto error = std::error_code {};
        fs::create_directories(expandHome(*paths.worktreeRoot, *home), error);
        auto root = resolved(*paths.worktreeRoot, *home);
        if (!root)
            return std::unexpected { root.error() };
        host.worktreeRoot = *root;
    }
    for (auto const& repo: paths.repos.value_or(std::vector<detail::JsonRepoPath> {}))
    {
        auto path = resolved(repo.path.value_or(""), *home);
        if (!path)
            return std::unexpected { path.error() };
        host.repoPaths.push_back(*path);
    }
    return host;
}

std::expected<std::string, FileReadError> readRegularFile(std::string const& path)
{
    auto error = std::error_code {};
    auto const directory = fs::path(path).parent_path();
    // Only a directory that is not there at all is gone: one replaced by a link or a file is not followed.
    auto const directoryType = fs::symlink_status(directory, error).type();
    if (directoryType == fs::file_type::not_found)
        return std::unexpected { FileReadError::DirectoryMissing };
    if (directoryType != fs::file_type::directory)
        return std::unexpected { FileReadError::NotRegular };
    auto const status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found)
        return std::unexpected { FileReadError::Unreadable };
    if (status.type() != fs::file_type::regular)
        return std::unexpected { FileReadError::NotRegular };
    if (fs::file_size(path, error) > MaxReadFileBytes || error)
        return std::unexpected { error ? FileReadError::Unreadable : FileReadError::TooLarge };
    auto content = readWholeFile(path);
    if (!content)
        return std::unexpected { FileReadError::Unreadable };
    return *content;
}

std::optional<std::string> readWholeFile(std::string const& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    auto content = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (file.bad())
        return std::nullopt;
    return content;
}

std::optional<std::string> newSessionUuid()
{
    auto uuid = readWholeFile("/proc/sys/kernel/random/uuid");
    if (!uuid)
        return std::nullopt;
    while (!uuid->empty() && (uuid->back() == '\n' || uuid->back() == ' '))
        uuid->pop_back();
    return uuid;
}

int32_t utcOffsetMinutes() noexcept
{
    auto const now = std::time(nullptr);
    auto local = std::tm {};
    if (::localtime_r(&now, &local) == nullptr)
        return 0;
    return static_cast<int32_t>(local.tm_gmtoff / 60);
}

uint64_t wallNowMs() noexcept
{
    auto const since = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

} // namespace orchestrator::proto
