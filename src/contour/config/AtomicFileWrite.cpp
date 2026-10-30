// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>

#include <core/Utf8Path.hpp>

#include <format>
#include <fstream>
#include <tuple>

namespace contour::config
{

std::expected<void, std::string> atomicWriteFile(std::filesystem::path const& path, std::string_view content)
{
    auto tmp = path;
    tmp += ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return std::unexpected(
            std::format("Failed to create {}: {}", core::pathToUtf8(path.parent_path()), ec.message()));

    // Drops the temp file unless the write completes: a leftover .tmp would otherwise accumulate on
    // every failed save.
    auto const discardTemp = [&tmp] {
        std::error_code removeError;
        std::filesystem::remove(tmp, removeError);
    };

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        // close() flushes the buffer and reports write errors (disk full, quota) as failbit. Checking
        // BEFORE the flush would miss them, and the rename below would then replace a good file with a
        // truncated one — the very loss this atomic dance exists to avoid.
        out.close();
        if (out.fail())
        {
            discardTemp();
            return std::unexpected(std::format("Failed to write {}", core::pathToUtf8(tmp)));
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        discardTemp();
        return std::unexpected(std::format("Failed to write {}: {}", core::pathToUtf8(path), ec.message()));
    }

    return {};
}

std::expected<void, std::string> writeFileDurably(core::platform::FileSystem const& fileSystem,
                                                  std::filesystem::path const& path,
                                                  std::string_view content)
{
    auto const directory = path.parent_path();
    auto temporaryName = std::filesystem::path { "." };
    temporaryName += path.filename();
    temporaryName += ".tmp";
    auto const temporary = directory / temporaryName;

    if (auto const created = fileSystem.createDirectories(directory); !created)
        return created;

    // What a crash left there would make the exclusive create below fail on every later save.
    std::ignore = fileSystem.remove(temporary);
    auto const written = fileSystem.createNewFile(temporary)
                             .and_then([&] { return fileSystem.writeFile(temporary, content); })
                             .and_then([&] { return fileSystem.syncFile(temporary); })
                             .and_then([&] { return fileSystem.replaceFile(temporary, path); })
                             .and_then([&] { return fileSystem.syncDirectory(directory); });
    if (!written)
        std::ignore = fileSystem.remove(temporary);
    return written;
}

} // namespace contour::config
