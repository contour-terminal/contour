// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/GpuInventory.hpp>

#include <filesystem>
#include <memory>
#include <vector>

namespace contour::display
{

/// Lists GPUs from /sys/class/drm. Reads only sysfs attributes and symlinks, which a runtime-suspended
/// GPU answers without being woken; it never opens a DRM node.
class SysfsGpuInventory final: public IGpuInventory
{
  public:
    /// @param reader Filesystem access.
    /// @param drmRoot Usually /sys/class/drm.
    /// @param pciIdsPaths pci.ids locations to try in order, for titles.
    SysfsGpuInventory(std::shared_ptr<ITextFileReader const> reader,
                      std::filesystem::path drmRoot,
                      std::vector<std::filesystem::path> pciIdsPaths);

    [[nodiscard]] std::vector<GpuCandidate> list() const override;

  private:
    std::shared_ptr<ITextFileReader const> _reader;
    std::filesystem::path _drmRoot;
    std::vector<std::filesystem::path> _pciIdsPaths;
};

} // namespace contour::display
