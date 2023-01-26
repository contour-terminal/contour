# vim:et:ts=4:sw=4
#
#/**
# * This file is part of the "contour" project
# *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
# *
# * Licensed under the Apache License, Version 2.0 (the "License");
# * you may not use this file except in compliance with the License.
# *
# * Unless required by applicable law or agreed to in writing, software
# * distributed under the License is distributed on an "AS IS" BASIS,
# * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# * See the License for the specific language governing permissions and
# * limitations under the License.
# */

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
    printf '\e[>M'

    # Informs contour terminal about the current working directory, so that e.g. OpenFileManager works.
    printf '\e]7;'$(pwd)'\e\\'

    # Example hook to update configuration profile based on base directory.
    # update_profile
end

function preexec_hook_contour -d "Run after printing prompt" -e fish_preexec
    # Enables text reflow for the main page area again, so that a window resize will reflow again.
    printf "\e[?2028h"
end
