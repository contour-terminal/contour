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


function precmd_hook_contour -d "Shell Integration hook to be invoked before each prompt" -e fish_prompt
    # Must come first: anything else would clobber the exit status we are about to report.
    set -l exit_status $status

    # OSC 133;D -- the command that just ran has finished. Only emitted once a command actually HAS run:
    # an unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed.
    if set -q _contour_command_running
        printf "\e]133;D;$exit_status\e\\"
        set -e _contour_command_running
    end

    # Disable text reflow for the command prompt (and below).
    printf '\e[?2028l'

    # OSC 133;A -- a new prompt starts on this line. Together with ;C below this is what lets the terminal
    # tell a prompt apart from a command's output, which is what "copy last command output" reads.
    printf "\e]133;A\e\\"

    # Marks the current line (command prompt) so that you can jump to it via key bindings.
    #printf '\e[>M'
    printf "\e[>M"

    # Informs contour terminal about the current working directory, so that e.g. OpenFileManager works.
    printf "\e]7;$PWD\e\\"

    # Example hook to update configuration profile based on base directory.
    # update_profile
end

function preexec_hook_contour -d "Run after printing prompt" -e fish_preexec
    # Enables text reflow for the main page area again, so that a window resize will reflow again.
    printf "\e[?2028h"

    # OSC 133;C -- the command's output begins on the line the cursor is on now.
    printf "\e]133;C\e\\"

    set -g _contour_command_running 1
end
