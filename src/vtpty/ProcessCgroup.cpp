// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessCgroup.hpp>

#include <vtpty/OwnedFd.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <format>
#include <ranges>

#include <fcntl.h>
#include <unistd.h>

namespace vtpty
{

std::expected<std::string, std::error_code> readProcessCgroup(int pid)
{
    // POSIX calls rather than a stream, which would keep the reason to itself.
    auto const lastError = [] {
        return std::unexpected(std::error_code { errno, std::generic_category() });
    };
    auto const file = OwnedFd { ::open(std::format("/proc/{}/cgroup", pid).c_str(), O_RDONLY | O_CLOEXEC) };
    if (!file.isOpen())
        return lastError();
    auto text = std::string {};
    auto chunk = std::array<char, 512> {};
    while (true)
    {
        auto const count = ::read(file.get(), chunk.data(), chunk.size());
        if (count == 0)
            return text;
        if (count > 0)
            text.append(chunk.data(), static_cast<std::size_t>(count));
        else if (errno != EINTR)
            return lastError();
    }
}

bool isInUnit(std::string_view cgroupFile, std::string_view unit) noexcept
{
    if (unit.empty())
        return false;
    return std::ranges::any_of(cgroupFile | std::views::split('\n'), [unit](auto const& lineRange) {
        auto const line = std::string_view { lineRange.begin(), lineRange.end() };
        // hierarchy-ID:controller-list:cgroup-path; the path is everything after the second colon.
        auto const controllers = line.find(':');
        auto const path =
            controllers == std::string_view::npos ? std::string_view::npos : line.find(':', controllers + 1);
        if (path == std::string_view::npos)
            return false;
        auto const cgroup = line.substr(path + 1);
        return cgroup.size() > unit.size() && cgroup.ends_with(unit)
               && cgroup[cgroup.size() - unit.size() - 1] == '/';
    });
}

} // namespace vtpty
