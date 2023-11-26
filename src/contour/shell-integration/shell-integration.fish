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
    # Disable text reflow for the command prompt (and below).
    printf '\e[?2028l'

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
end
