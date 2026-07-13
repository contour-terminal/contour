# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# Escapes are written as \033, not \e: tcsh's builtin echo understands the octal form but not the GNU \e
# extension, and silently prints the latter as literal text. (That is why this file used to emit the
# characters "\e[?2028l..." into the terminal rather than the sequences it meant.)
#
# precmd emits, in order:
#   OSC 133;D  -- the previous command finished, with its exit status,
#   DECRST 2028 -- text reflow off for the prompt,
#   SETMARK + OSC 7 -- the jump-to-prompt mark, and the working directory,
#   OSC 133;A  -- a new prompt starts on this line.
# postcmd emits DECSET 2028 (reflow back on) and OSC 133;C -- the command's output begins here.
#
# Together, ;A/;C/;D are what let the terminal tell a prompt apart from a command's output, which is what
# "copy last command output" reads. Unlike the other shells there is no guard against emitting ;D at the
# very first prompt -- a tcsh alias cannot carry that state without clobbering $status -- so the first
# prompt reports a command block with no text in it. The terminal ignores an empty block.
alias precmd 'echo -n "\033]133;D;$status\033\\\033[?2028l\033[>M\033]7;$PWD\033\\\033]133;A\033\\";'
alias postcmd 'echo -n "\033[?2028h\033]133;C\033\\";'
