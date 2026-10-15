# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# What precmd and postcmd echo carries the ESC and BEL bytes themselves, made once below, rather than
# \033 for tcsh's builtin echo to expand: echo expands escapes only where echo_style is "both" or "sysv",
# as Linux builds of tcsh set it, and prints them as literal text where it is "bsd", as macOS's tcsh is
# built. A byte held in a variable reads the same under every style. For the same reason the sequences
# echo writes end in BEL rather than ST, whose backslash an expanding echo would read as an escape.
#
# precmd emits, in order:
#   OSC 133;D  -- the previous command finished, with its exit status,
#   DECRST 2028 -- text reflow off for the prompt,
#   OSC 7      -- the working directory, as a file:// URL,
#   OSC 133;A  -- a new prompt starts on this line.
# The prompt itself ends in OSC 133;B -- typed input begins here (see the end of this file).
# postcmd emits DECSET 2028 (reflow back on) and OSC 133;C -- the command's output begins here. No tcsh
# hook sees the command line, so ;C carries none: the terminal reads the command back off the screen,
# from the ;B column on. postcmd stays silent until the first prompt: tcsh runs it before every line of
# a file it sources -- the rest of this one, and of the .tcshrc sourcing it -- and none of those is a
# command typed at a prompt.
#
# Together, ;A/;C/;D are what let the terminal tell a prompt apart from a command's output, which is what
# "copy last command output" reads. Unlike the other shells there is no guard against emitting ;D at the
# very first prompt -- a tcsh alias cannot carry that state without clobbering $status -- so the first
# prompt reports a command block with no text in it. The terminal ignores an empty block.

# An awk program that prints OSC 7 -- the working directory as a file:// URL, its path percent-encoded
# byte by byte as a command line is (A-Z a-z 0-9 / . _ ~ - kept) -- for precmd: one program, reading no
# input, as the only process a prompt starts. tcsh keeps PWD and HOST in the environment, newlines and
# all; run in the C locale, awk counts bytes rather than characters.
set __contour_osc7 = 'BEGIN { for (i = 1; i < 256; i++) ord[sprintf("%c", i)] = i; path = ENVIRON["PWD"]; printf "\033]7;file://%s", ENVIRON["HOST"]; for (i = 1; i <= length(path); i++) { c = substr(path, i, 1); if (c ~ /[A-Za-z0-9\/._~-]/) printf "%s", c; else printf "%%%02X", ord[c] } printf "\033\\" }'

set __contour_esc = "`printf '\033'`"
set __contour_bel = "`printf '\007'`"

alias precmd 'echo -n "${__contour_esc}]133;D;${status}${__contour_bel}${__contour_esc}[?2028l"; env LC_ALL=C awk "$__contour_osc7"; echo -n "${__contour_esc}]133;A${__contour_bel}"; set __contour_prompted'
alias postcmd 'if ( $?__contour_prompted ) echo -n "${__contour_esc}[?2028h${__contour_esc}]133;C${__contour_bel}"'

# OSC 133;B, as a %{...%} literal at the end of the prompt.
#
# tcsh draws a literal together with the character after it, and drops one that nothing follows ("we
# lose the last literal", RefreshPromptpart() in tcsh's ed.refresh.c). So, as iTerm2's integration does,
# the prompt's trailing space comes off and goes back on after the mark -- which then reaches the
# terminal just before that space is drawn, one column short of where typing begins. DECSC, CUF and
# DECRC around it step over the space and back: the mark lands where typing begins, and the cursor ends
# where tcsh expects it. A prompt that does not end in a space gains one.
#
# No tcsh hook runs after the prompt is drawn, so `prompt` is rewritten once, here: source this file
# after setting your prompt. Sourcing it again changes nothing. The prompt travels through the
# environment and sed rather than through quotes, which no csh quoting would let it survive intact.
if ( $?prompt ) then
    if ( "$prompt" !~ *133?B* ) then
        setenv CONTOUR_PROMPT_TEXT "${prompt}@@contour@@"
        set prompt = "`printenv CONTOUR_PROMPT_TEXT | sed -e 's/ @@contour@@/@@contour@@/' -e 's/@@contour@@//'`%{\0337\033[C\033]133;B\007\0338%} "
        unsetenv CONTOUR_PROMPT_TEXT
    endif
endif
