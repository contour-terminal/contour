// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A process image replacer that replaces nothing, for the tests of what re-executes the daemon.

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <vthost/DaemonBinary.hpp>

namespace vthost::testing
{

/// A ProcessImageReplacer that records what it was asked to run, and fails as an execv would.
///
/// A test that wants the exec to succeed -- never to return -- throws from @ref onReplace, which unwinds out
/// of the caller the way the exec ends it.
class FakeProcessImageReplacer final: public ProcessImageReplacer
{
  public:
    [[nodiscard]] std::error_code replaceWith(std::filesystem::path const& image,
                                              std::span<std::string const> argv) override
    {
        replacements.emplace_back(image, std::vector<std::string>(argv.begin(), argv.end()));
        if (onReplace)
            onReplace();
        return failure;
    }

    /// Runs inside replaceWith(), where the exec would happen.
    std::function<void()> onReplace;

    /// What replaceWith() fails with.
    std::error_code failure = std::make_error_code(std::errc::no_such_file_or_directory);

    /// Every replacement asked for: the program, and its argument vector.
    std::vector<std::pair<std::filesystem::path, std::vector<std::string>>> replacements;
};

} // namespace vthost::testing
