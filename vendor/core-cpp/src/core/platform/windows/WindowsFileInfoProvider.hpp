// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/FileInfoProvider.hpp>

namespace core::platform
{

/// Windows implementation of FileInfoProvider using std::filesystem.
///
/// Lists like PosixFileInfoProvider, since both use std::filesystem for that.
/// The only difference is that Windows permissions are limited (read-only flag only,
/// no owner/group/others distinction).
class WindowsFileInfoProvider final: public FileInfoProvider
{
  public:
    [[nodiscard]] std::vector<FileEntry> listDirectory(std::string const& path) const override;
};

} // namespace core::platform
