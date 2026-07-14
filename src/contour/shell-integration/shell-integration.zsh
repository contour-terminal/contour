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

precmd_hook_contour()
{
    # Must be the very first line: anything else would clobber the exit status we are about to report.
    local exit_status=$?

    # OSC 133;D -- the command that just ran has finished. Only emitted once a command actually HAS run:
    # an unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed.
    if [[ -n "${_contour_command_running:-}" ]]; then
        print -n "\e]133;D;${exit_status}\e\\" >$TTY
        unset _contour_command_running
    fi

    # Disable text reflow for the command prompt (and below).
    print -n '\e[?2028l' >$TTY

    # OSC 133;A -- a new prompt starts on this line. Together with ;C below this is what lets the terminal
    # tell a prompt apart from a command's output, which is what "copy last command output" reads.
    print -n '\e]133;A\e\\' >$TTY

    # Marks the current line (command prompt) so that you can jump to it via key bindings.
    echo -n '\e[>M' >$TTY

    # Informs contour terminal about the current working directory, so that e.g. OpenFileManager works.
    echo -ne '\e]7;'$(pwd)'\e\\' >$TTY

    # Example hook to update configuration profile based on base directory.
    # update_profile >$TTY
}

preexec_hook_contour()
{
    # Enables text reflow for the main page area again, so that a window resize will reflow again.
    print -n "\e[?2028h" >$TTY

    # OSC 133;C -- the command's output begins on the line the cursor is on now.
    print -n '\e]133;C\e\\' >$TTY

    _contour_command_running=1
}

add-zsh-hook precmd precmd_hook_contour
add-zsh-hook preexec preexec_hook_contour
