// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtpty
{

/// What the terminal does with the text typed into it: the line discipline's echo and line editing, as the
/// program running in it set them.
///
/// Hidden is deliberately narrow -- echo off with line editing still on -- because every full-screen
/// program and every shell line editor clears both, and must not read as a password prompt. The bound
/// this draws is known: a password reader that clears both and draws its own feedback (sudo with
/// `pwfeedback`, sudo-rs's feedback, pinentry-curses, a sudo prompt reached through ssh, whose local
/// terminal is in the remote shell's raw mode) reads Raw, and gets no lock.
enum class InputEcho : std::uint8_t
{
    Unknown = 0, ///< The device cannot tell: a tmux pane, a ConPTY, a daemon too old to report it.
    Echoed,      ///< The terminal echoes what is typed.
    Raw,    ///< Neither echo nor line editing: the program draws its own echo (an editor, a line editor).
    Hidden, ///< Line editing without echo: a password prompt.
};

} // namespace vtpty
