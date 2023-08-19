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
    # Disable text reflow for the command prompt (and below).
    print -n '\e[?2028l' >$TTY

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
}

add-zsh-hook precmd precmd_hook_contour
add-zsh-hook preexec preexec_hook_contour
