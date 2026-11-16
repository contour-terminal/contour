// SPDX-License-Identifier: Apache-2.0
#include <core/tui/TerminalOutput.hpp>

#include <cstdio>

/// @file
/// What @c TerminalOutput does on a platform that is neither a native POSIX system nor Windows --
/// a WebAssembly guest -- with the C++ standard library alone: the write goes to the C standard
/// output stream, and there is no terminal to ask anything. The size stays at its 80x24 default,
/// nothing is probed, and the destination does not count as a terminal, so no synchronized-output
/// bracket is written. A guest that renders for a terminal on its host gives @c Screen an output
/// that overrides @c writeToDestination() and @c isTerminal(), and never reaches this file.

namespace core::tui
{

auto TerminalOutput::initialize() -> VoidResult
{
    updateDimensions();
    return {};
}

void TerminalOutput::detectCapabilities()
{
    // No terminal to probe: unscroll support stays off.
}

void TerminalOutput::writeToDestination(std::string_view bytes)
{
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

bool TerminalOutput::isTerminal() const noexcept
{
    return false;
}

void TerminalOutput::updateDimensions()
{
    // No terminal to measure: the size stays what it was, 80x24 unless a subclass says otherwise.
}

} // namespace core::tui
