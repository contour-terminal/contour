// SPDX-License-Identifier: Apache-2.0
#include <contour/AtomicFileWrite.h>

#include <format>
#include <fstream>

namespace contour
{

std::expected<void, std::string> atomicWriteFile(std::filesystem::path const& path, std::string_view content)
{
    auto const tmp = path.string() + ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return std::unexpected(
            std::format("Failed to create {}: {}", path.parent_path().string(), ec.message()));

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
            return std::unexpected(std::format("Failed to write {}", tmp));
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec)
    {
        discardTemp();
        return std::unexpected(std::format("Failed to write {}: {}", path.string(), ec.message()));
    }

    return {};
}

} // namespace contour
