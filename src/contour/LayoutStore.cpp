// SPDX-License-Identifier: Apache-2.0
#include <contour/AtomicFileWrite.h>
#include <contour/LayoutBuilder.h>
#include <contour/LayoutStore.h>

namespace contour
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

} // namespace contour
