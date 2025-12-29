// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

namespace vtbackend
{

class ShellIntegration
{
  public:
    virtual ~ShellIntegration() = default;

    /**
     * Triggered when the shell starts printing the prompt.
     *
     * This roughly maps to OSC 133 ; A.
     *
     * @param clickEvents boolean indicating if the prompt is clickable/interactive.
     */
    virtual void promptStart(bool clickEvents = false) = 0;

    /**
     * Triggered when the shell finished printing the prompt.
     *
     * This roughly maps to OSC 133 ; B.
     */
    virtual void promptEnd() = 0;

    /**
     * Triggered when the shell is about to execute a command (and thus potential output starts).
     *
     * This roughly maps to OSC 133 ; C.
     *
     * @param commandLine The command line that is being executed.
     */
    virtual void commandOutputStart(std::optional<std::string> const& commandLine = std::nullopt) = 0;

    /**
     * Triggered when the executed command has finished.
     *
     * This roughly maps to OSC 133 ; D.
     *
     * @param exitCode The exit code of the executed command.
     */
    virtual void commandFinished(int exitCode) = 0;
};

class NullShellIntegration: public ShellIntegration
{
  public:
    void promptStart(bool) override {}
    void promptEnd() override {}
    void commandOutputStart(std::optional<std::string> const&) override {}
    void commandFinished(int) override {}
};

} // namespace vtbackend
