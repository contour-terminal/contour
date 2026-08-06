// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/config/LayoutBuilder.hpp>
#include <contour/config/LayoutStore.hpp>

namespace contour::config
{

std::expected<LayoutMap, std::string> FileLayoutStore::load(std::filesystem::path const& path) const
{
    return config::loadLayoutsFile(path);
}

std::expected<void, std::string> FileLayoutStore::save(std::filesystem::path const& path,
                                                       LayoutMap const& layouts)
{
    // Atomically, so an interrupted or failing write can never leave a truncated `layouts.yml` behind:
    // losing a truncated store means losing every layout the user ever saved. See atomicWriteFile().
    return atomicWriteFile(path, emitLayoutsYaml(layouts));
}

} // namespace contour::config
