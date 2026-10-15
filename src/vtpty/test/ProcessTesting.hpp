// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PageSize.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vtpty::testing
{

/// @return How to run @p program with @p arguments, and nothing else configured.
inline Process::ExecInfo execInfo(std::string program, std::vector<std::string> arguments = {})
{
    return Process::ExecInfo { .program = std::move(program),
                               .arguments = std::move(arguments),
                               .workingDirectory = {},
                               .env = {},
                               .removedEnvironment = {} };
}

/// @return A Process running @p exec on a PTY of its own, @p pageSize large, placed nowhere; not yet
///         started.
inline Process unplacedProcess(Process::ExecInfo const& exec, PageSize pageSize)
{
    return Process { exec,
                     createPty(pageSize, std::nullopt),
                     /*escapeSandbox=*/false,
                     std::make_shared<NoPlacement>() };
}

} // namespace vtpty::testing
