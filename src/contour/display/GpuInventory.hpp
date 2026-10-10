// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/GpuSelection.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace contour::display
{

/// Reads small text files, directory listings and symlinks. Injected so tests never touch a filesystem.
class ITextFileReader
{
  public:
    virtual ~ITextFileReader() = default;

    /// @return The file's contents, or nullopt when it cannot be read.
    [[nodiscard]] virtual std::optional<std::string> read(std::filesystem::path const& path) const = 0;
    /// @return The entry names directly inside @p path; empty when it cannot be listed.
    [[nodiscard]] virtual std::vector<std::string> listDirectory(std::filesystem::path const& path) const = 0;
    /// @return The target of the symlink @p path, unresolved; nullopt when it is not one.
    [[nodiscard]] virtual std::optional<std::filesystem::path> readLink(
        std::filesystem::path const& path) const = 0;
};

/// The GPUs this machine has, listed without opening any of them.
class IGpuInventory
{
  public:
    virtual ~IGpuInventory() = default;

    /// @return One candidate per GPU, in a stable order; empty when unknown.
    [[nodiscard]] virtual std::vector<GpuCandidate> list() const = 0;
};

/// @return The reader for the real filesystem.
[[nodiscard]] std::shared_ptr<ITextFileReader const> makeFileSystemTextReader();

/// @return The inventory for this platform (sysfs on Linux, DXGI on Windows, empty elsewhere); never null.
[[nodiscard]] std::shared_ptr<IGpuInventory const> makePlatformGpuInventory();

} // namespace contour::display
