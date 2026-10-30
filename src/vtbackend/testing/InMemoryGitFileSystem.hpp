// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/GitHead.hpp>

#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace vtbackend::test
{

/// Repository discovery over an InMemoryFileSystem, with mount points a test names and a count of every
/// call, so that a test can say which filesystem a path is on and when it was looked at.
class InMemoryGitFileSystem final: public GitFileSystem
{
  public:
    /// @param files What is read. Borrowed: it must outlive this.
    explicit InMemoryGitFileSystem(core::platform::testing::InMemoryFileSystem const& files): _files { files }
    {
    }

    /// Puts @p at, and everything below it, on filesystem @p device. Everything else is on filesystem 0.
    /// @param at The mount point.
    /// @param device Its device id.
    void mount(std::filesystem::path const& at, std::uint64_t device)
    {
        _mounts[at.lexically_normal()] = device;
    }

    /// @return How many probes and reads were made so far.
    [[nodiscard]] std::size_t calls() const noexcept { return _calls.load(); }

    [[nodiscard]] GitPathStatus status(std::filesystem::path const& path) const override
    {
        ++_calls;
        return GitPathStatus { .kind = kindOf(path), .device = deviceOf(path) };
    }

    [[nodiscard]] std::optional<std::string> readMetadata(std::filesystem::path const& path) const override
    {
        ++_calls;
        return readGitMetadata(_files, path);
    }

  private:
    /// What @p path names in the files.
    [[nodiscard]] GitPathKind kindOf(std::filesystem::path const& path) const
    {
        if (_files.isDirectory(path))
            return GitPathKind::Directory;
        if (_files.isRegularFile(path))
            return GitPathKind::RegularFile;
        if (_files.exists(path))
            return GitPathKind::Other;
        return GitPathKind::Absent;
    }

    /// The device of the deepest mount @p path is under; 0 for none.
    [[nodiscard]] std::uint64_t deviceOf(std::filesystem::path const& path) const
    {
        auto device = std::uint64_t { 0 };
        auto depth = std::ptrdiff_t { -1 };
        auto const normal = path.lexically_normal();
        for (auto const& [at, id]: _mounts)
        {
            auto const relative = normal.lexically_relative(at);
            if (relative.empty() || *relative.begin() == "..")
                continue;
            if (auto const here = std::distance(at.begin(), at.end()); here > depth)
            {
                depth = here;
                device = id;
            }
        }
        return device;
    }

    core::platform::testing::InMemoryFileSystem const& _files;
    std::map<std::filesystem::path, std::uint64_t> _mounts;
    mutable std::atomic<std::size_t> _calls { 0 };
};

} // namespace vtbackend::test
