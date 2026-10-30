// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A file system whose writes, renames and removals fail where a test says, as a full or read-only disk does.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <vthost/testing/ForwardingFileSystem.hpp>

namespace vthost::testing
{

/// The calls FaultyFileSystem can fail.
enum class FaultOp : std::uint8_t
{
    WriteFile = 0,
    Rename,
    Remove,
};

/// Forwards to the in-memory files, except for the calls its predicate selects, which fail.
class FaultyFileSystem final: public ForwardingFileSystem
{
  public:
    /// Selects the calls to fail: the operation, and the path written, renamed to or removed.
    using Fault = std::function<bool(FaultOp, std::filesystem::path const&)>;

    /// @param fault Which calls fail.
    explicit FaultyFileSystem(Fault fault): _fault { std::move(fault) } {}

    [[nodiscard]] std::expected<void, std::string> writeFile(std::filesystem::path const& path,
                                                             std::string_view content) const override
    {
        if (_fault(FaultOp::WriteFile, path))
            return std::unexpected("No space left on device: " + path.string());
        return ForwardingFileSystem::writeFile(path, content);
    }

    [[nodiscard]] std::expected<void, std::string> rename(std::filesystem::path const& from,
                                                          std::filesystem::path const& to) const override
    {
        if (_fault(FaultOp::Rename, to))
            return std::unexpected("Permission denied: " + to.string());
        return ForwardingFileSystem::rename(from, to);
    }

    [[nodiscard]] std::expected<bool, std::string> remove(std::filesystem::path const& path) const override
    {
        if (_fault(FaultOp::Remove, path))
            return std::unexpected("Permission denied: " + path.string());
        return ForwardingFileSystem::remove(path);
    }

  private:
    Fault _fault;
};

} // namespace vthost::testing
