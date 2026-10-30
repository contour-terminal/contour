# vim:et:ts=4:sw=4
#
#// SPDX-License-Identifier: Apache-2.0

# Example hook to change profile based on directory.
# update_profile()
# {
#     case "$PWD" in
#         "$HOME"/work*) contour set profile to work ;;
#         "$HOME"/projects*) contour set profile to main ;;
#         *) contour set profile to mobile ;;
#     esac
# }

autoload -Uz add-zsh-hook

# The longest command line sent, in bytes: a little more than vtbackend::MaxRecordedCommandLineBytes.
typeset -g _contour_cmdline_max_bytes=4100

# The machine OSC 7 names: gethostname(2), as the terminal names itself when it judges a directory local.
# Not $HOST, which zsh takes from the environment when one is inherited, however stale (a login carried
# over from another machine, a container's image), and which %M reads too; uname -n, once, as the file
# is sourced.
typeset -g _contour_hostname=${$(uname -n 2>/dev/null):-$HOST}

# OSC 133;B as it sits at the end of PROMPT. %{ %} tell zsh it takes no room on the line; BEL ends
# it, so that no backslash has to survive prompt expansion.
typeset -g _contour_prompt_end_mark=$'%{\e]133;B\a%}'

# Percent-encodes $1 -- at most $2 bytes of it, when given -- into REPLY, byte by byte as kitty's
# cmdline_url= (A-Z a-z 0-9 / . _ ~ - kept).
_contour_percent_encode()
{
    emulate -L zsh
    # Bytes, not characters: without MULTIBYTE, $#text and $text[i] count bytes.
    setopt no_multibyte
    local text=$1 kept hex
    (( $# > 1 )) && text=${text[1,$2]}
    REPLY=
    while [[ -n $text ]]; do
        # A run of bytes kept as they are, in one step; then the byte that ended it, as %XX.
        kept=${text%%[^A-Za-z0-9/._~-]*}
        REPLY+=$kept
        text=${text[$#kept+1,-1]}
        [[ -n $text ]] || break
        printf -v hex '%%%02X' $(( #text & 255 ))
        REPLY+=$hex
        text=${text[2,-1]}
    done
}

precmd_hook_contour()
{
    # Must be the very first line: anything else would clobber the exit status we are about to report.
    local exit_status=$?

    # One open of the terminal for all of it.
    {
        # OSC 133;D -- the command that just ran has finished. Only emitted once a command actually HAS
        # run: an unconditional D would put a CommandEnd on the very first prompt and invent a command
        # block that never existed.
        if [[ -n "${_contour_command_running:-}" ]]; then
            print -n "\e]133;D;${exit_status}\e\\"
            unset _contour_command_running
        fi

        # Disable text reflow for the command prompt (and below).
        print -n '\e[?2028l'

        # OSC 133;A -- a new prompt starts on this line. Together with ;C below this is what lets the
        # terminal tell a prompt apart from a command's output, which is what "copy last command output"
        # reads.
        print -n '\e]133;A\e\\'

        # Informs contour terminal about the current working directory, so that e.g. OpenFileManager
        # works: a file:// URL, its path encoded as a command line is, so that no ESC or BEL in a
        # directory name ends the sequence early.
        _contour_percent_encode "$PWD"
        printf '\e]7;file://%s%s\e\\' "$_contour_hostname" "$REPLY"
    } >$TTY

    # Keep the prompt-end mark the LAST precmd hook: a theme that rebuilds PROMPT in a precmd hook of its
    # own (powerlevel10k, most oh-my-zsh themes), registered after this file was sourced, would otherwise
    # build over it. zsh runs each prompt's hooks from a copy of the list, so the move counts from the
    # next prompt on.
    precmd_functions=(${precmd_functions:#_contour_mark_prompt_end} _contour_mark_prompt_end)

    # Example hook to update configuration profile based on base directory.
    # update_profile >$TTY
}

preexec_hook_contour()
{
    # One open of the terminal for all of it.
    {
        # Enables text reflow for the main page area again, so that a window resize will reflow again.
        print -n "\e[?2028h"

        # OSC 133;C -- the command's output begins on the line the cursor is on now -- with the command
        # line as it was typed (preexec's $1), percent-encoded as kitty's cmdline_url=. $1 is empty while
        # the history mechanism is off; the terminal then reads the command off the screen instead.
        if [[ -n $1 ]]; then
            _contour_percent_encode "$1" $_contour_cmdline_max_bytes
            printf '\e]133;C;cmdline_url=%s\e\\' "$REPLY"
        else
            print -n '\e]133;C\e\\'
        fi
    } >$TTY

    _contour_command_running=1
}

# Appends the prompt-end mark to PROMPT -- exactly once. PROMPT outlives each prompt, and themes rebuild
# it from scratch or around its old value, so any copy already in it is taken out first and the mark put
# back at the very end. Sourcing this file again changes nothing.
_contour_mark_prompt_end()
{
    PROMPT=${PROMPT//"$_contour_prompt_end_mark"/}$_contour_prompt_end_mark
}

add-zsh-hook precmd precmd_hook_contour
add-zsh-hook preexec preexec_hook_contour
add-zsh-hook precmd _contour_mark_prompt_end
