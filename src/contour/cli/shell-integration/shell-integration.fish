# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# Example hook to change profile based on directory.
# function update_profile
#    switch "$PWD"
#        case "$HOME/work"*
#            contour set profile to work
#        case "$HOME/projects"*
#            contour set profile to main
#        case '*'
#            contour set profile to mobile
#    end
# end

# Which OSC 133 marks this fish writes by itself (fish's release notes and its src/terminal.rs):
#   4.0.0 -- A, C and D;   4.0.1 -- C carries cmdline_url=;   4.3.0 -- B.
# `set -Ua fish_features no-mark-prompt` (fish 4.0.6 and later) turns all of them off again. This
# script adds exactly what fish leaves out, and never a second A, C or D.
#
# fish 4.0.0 also reports the working directory -- OSC 7, a file:// URL -- to every terminal, at start-up
# and whenever $PWD changes (__fish_update_cwd_osc); before, only to the few it knew. no-mark-prompt
# leaves that alone.
#
# fish_version:       fish's $version, e.g. 4.0.2, 4.3.0-12-gabcdef0 or 4.0b1.
# mark_prompt_status: the exit status of `status test-feature mark-prompt` -- 0 on, 1 off, and 2 for
#                     a fish older than 4.0.6, which had no way to turn marking off.
# Prints one line per mark fish sends itself: A, C, D, cmdline (C carries the command line), B, and cwd
# (OSC 7).
function __contour_native_marks --argument-names fish_version mark_prompt_status
    set -l parts (string match -ar '\d+' -- (string replace -r '[^0-9.].*$' '' -- $fish_version)) 0 0 0
    set -l number (math "$parts[1] * 10000 + $parts[2] * 100 + $parts[3]")
    test $number -ge 40000; or return
    if test "$mark_prompt_status" != 1
        printf '%s\n' A C D
        test $number -ge 40001; and echo cmdline
        test $number -ge 40300; and echo B
    end
    echo cwd
end

status test-feature mark-prompt
set -l __contour_mark_prompt_status $status
set -g __contour_native (__contour_native_marks $version $__contour_mark_prompt_status)

# OSC 133;B -- the prompt ends here and typed input begins.
function __contour_prompt_end
    printf "\e]133;B\e\\"
end

# Wraps fish_prompt so that the prompt-end mark is the last thing it prints; fish counts an escape
# sequence in a prompt as taking no room, so the mark lands where fish leaves the cursor for typing.
# Checked before every prompt rather than once: a prompt chosen later (`fish_config prompt choose`,
# starship, tide, a fish_prompt defined after this file was sourced) replaces the wrapper.
function __contour_wrap_fish_prompt
    functions -q fish_prompt; or return
    functions fish_prompt | string match -q '*__contour_prompt_end*'; and return
    functions -e __contour_original_fish_prompt
    functions -c fish_prompt __contour_original_fish_prompt
    function fish_prompt
        __contour_original_fish_prompt
        __contour_prompt_end
    end
end

function precmd_hook_contour -d "Shell Integration hook to be invoked before each prompt" -e fish_prompt
    # Must come first: anything else would clobber the exit status we are about to report.
    set -l exit_status $status

    # OSC 133;D -- the command that just ran has finished. Only emitted once a command actually HAS run:
    # an unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed. Left to fish when fish sends it itself.
    if set -q _contour_command_running; and not contains -- D $__contour_native
        printf "\e]133;D;%s\e\\" $exit_status
    end
    set -e _contour_command_running

    # Disable text reflow for the command prompt (and below).
    printf '\e[?2028l'

    # OSC 133;A -- a new prompt starts on this line -- unless fish says so itself.
    contains -- A $__contour_native; or printf "\e]133;A\e\\"

    # Informs contour terminal about the current working directory, so that e.g. OpenFileManager works --
    # unless fish says so itself. A file:// URL, its path encoded as a command line is, so that no ESC or
    # BEL in a directory name ends the sequence early.
    contains -- cwd $__contour_native
    or printf "\e]7;file://%s%s\e\\" $hostname (string escape --style=url -- $PWD)

    # OSC 133;B belongs at the very end of the prompt, which only fish_prompt itself reaches.
    contains -- B $__contour_native; or __contour_wrap_fish_prompt

    # Example hook to update configuration profile based on base directory.
    # update_profile
end

function preexec_hook_contour -d "Run after printing prompt" -e fish_preexec
    # Enables text reflow for the main page area again, so that a window resize will reflow again.
    printf "\e[?2028h"

    # OSC 133;C -- the command's output begins on the line the cursor is on now -- with the command line,
    # percent-encoded byte by byte as kitty's cmdline_url= wants it. Left to fish when fish sends it.
    contains -- C $__contour_native
    or printf "\e]133;C;cmdline_url=%s\e\\" (string escape --style=url -- $argv[1])

    set -g _contour_command_running 1
end
