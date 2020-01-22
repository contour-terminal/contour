/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#if defined(_MSC_VER)
#include <Windows.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace terminal {

class PseudoTerminal;

/**
 * Spawns and manages a child process with a pseudo terminal attached to it.
 */
class [[nodiscard]] Process {
  public:
#if defined(__unix__) || defined(__APPLE__)
	using NativeHandle = pid_t;
#else
	using NativeHandle = HANDLE;
#endif

    struct NormalExit { int exitCode; };
    struct SignalExit { int signum; };

    using ExitStatus = std::variant<NormalExit, SignalExit>;
	using Environment = std::map<std::string, std::string>;

    //! Returns login shell of current user.
    static std::string loginShell();

    Process(
		const std::string& path,
		std::vector<std::string> const& args,
		Environment const& env,
		PseudoTerminal& pty
	);

    Process(
		const std::string& _path,
		std::vector<std::string> const& _args,
		Environment const& _env,
		std::string const& _cwd,
		bool _detached
	);

	~Process();

	[[nodiscard]] NativeHandle nativeHandle() const noexcept { return pid_; }
    [[nodiscard]] bool alive() const noexcept;

    [[nodiscard]] std::optional<ExitStatus> checkStatus() const;
	[[nodiscard]] ExitStatus wait();

	[[nodiscard]] std::string workingDirectory() const;

    enum class TerminationHint {
        Normal,
        Hangup
    };
    void terminate(TerminationHint _terminationHint);

private:
    [[nodiscard]] std::optional<ExitStatus> checkStatus(bool _waitForExit) const;
	mutable NativeHandle pid_{};
	bool detached_ = false;

#if defined(_MSC_VER)
	PROCESS_INFORMATION processInfo_{};
	STARTUPINFOEX startupInfo_{};
#endif

    mutable std::optional<Process::ExitStatus> exitStatus_{};
};

}  // namespace terminal
