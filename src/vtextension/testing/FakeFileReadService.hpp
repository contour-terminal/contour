// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IFileReadService that records every read and answers what the test sets.
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/DaemonWorld.hpp>
#include <vtextension/FileReadService.hpp>

namespace vtextension::testing
{

/// Records each `file-read` by root and path, and answers @ref answer; `not-supported-yet` unless set, as
/// a host that reads no file.
class FakeFileReadService final: public IFileReadService
{
  public:
    [[nodiscard]] std::expected<FileContent, FileError> read(std::string_view root,
                                                             std::string_view path) override
    {
        reads.emplace_back(std::string(root), std::string(path));
        return answer;
    }

    std::vector<std::pair<std::string, std::string>> reads; ///< Every read(): root and path.
    std::expected<FileContent, FileError> answer { std::unexpect,
                                                   FileError::NotSupportedYet }; ///< What it answers.
};

} // namespace vtextension::testing
