// SPDX-License-Identifier: Apache-2.0
#include <contour/LayoutBuilder.h>
#include <contour/LayoutStore.h>

#include <fstream>

namespace contour
{

std::expected<LayoutMap, std::string> FileLayoutStore::load(std::filesystem::path const& path) const
{
    return config::loadLayoutsFile(path);
}

std::expected<void, std::string> FileLayoutStore::save(std::filesystem::path const& path,
                                                       LayoutMap const& layouts)
{
    auto const yaml = emitLayoutsYaml(layouts);
    auto const tmp = path.string() + ".tmp";

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec)
        return std::unexpected(
            std::format("Failed to create {}: {}", path.parent_path().string(), ec.message()));

    // Drops the temp file unless the write completes: a leftover .tmp would otherwise accumulate
    // on every failed save.
    auto const discardTemp = [&tmp] {
        std::error_code removeError;
        std::filesystem::remove(tmp, removeError);
    };

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << yaml;
        // close() flushes the buffer and reports write errors (disk full, quota) as failbit.
        // Checking BEFORE the flush would miss them, and the rename below would then replace a
        // good layouts.yml with a truncated one — the very loss this atomic dance exists to avoid.
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
