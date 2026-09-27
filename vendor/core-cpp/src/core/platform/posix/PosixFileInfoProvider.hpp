// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileInfoProvider.hpp>

namespace core::platform
{

/// The POSIX implementation of FileInfoProvider: std::filesystem lists, lstat(2) describes.
///
/// Uses directory_iterator for non-recursive listing and lstat(2) for each entry's metadata,
/// which every POSIX system has in full: Linux, macOS and the BSDs, and Emscripten over its
/// virtual filesystem. Gracefully skips unreadable entries via std::error_code overloads.
class PosixFileInfoProvider final: public FileInfoProvider
{
  public:
    [[nodiscard]] std::vector<FileEntry> listDirectory(std::string const& path) const override;
};

} // namespace core::platform
