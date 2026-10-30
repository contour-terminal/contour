# Contour shell integration for bash.
#
# Source it at the END of ~/.bashrc, after anything that sets PROMPT_COMMAND. bash-preexec installs
# itself by appending to PROMPT_COMMAND, so a later `PROMPT_COMMAND=...` throws the install away: no
# marks at all, and no error. bash-preexec's own header asks the same.
#
# It carries its own copy of bash-preexec
# (https://github.com/rcaloras/bash-preexec), vendored at the end of this file, for the preexec/precmd
# hooks that bash itself does not have: verbatim, but for one change, marked "Contour:".
#
# Contour's hooks come FIRST and bash-preexec last. bash-preexec's duplicate-inclusion guard returns
# from the whole sourced file, so with the hooks after it, a bash-preexec loaded earlier by something
# else (atuin, starship and others bring their own) -- or this file sourced a second time -- left
# Contour's hooks silently uninstalled.
#
# shellcheck shell=bash

# Not bash: nothing below parses anywhere else. (POSIX syntax for exactly that reason.)
if [ -z "${BASH_VERSION-}" ]; then
    return 1
fi

# The longest command line sent, in bytes: a little more than vtbackend::MaxRecordedCommandLineBytes.
__contour_cmdline_max_bytes=4100

# The machine OSC 7 names: gethostname(2), as the terminal names itself when it judges a directory local.
# Not $HOSTNAME, which bash takes from the environment when one is inherited, however stale (a login
# carried over from another machine, a container's image); its \H prompt escape is gethostname(2) as
# bash read it at start-up. ${…@P} is bash 4.4; before it, uname -n, once, as the file is sourced.
if (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
    __contour_hostname='\H'
    __contour_hostname=${__contour_hostname@P}
else
    __contour_hostname=$(uname -n 2>/dev/null) || __contour_hostname=$HOSTNAME
fi

# OSC 133;B as it sits at the end of PS1: \[ \] tell readline it takes no room on the line.
__contour_prompt_end_mark='\[\e]133;B\e\\\]'

# Percent-encodes $1 -- at most $2 bytes of it, when given -- into __contour_encoded, byte by byte as
# kitty's cmdline_url= (A-Z a-z 0-9 / . _ ~ - kept). In-process: no subshell on the prompt's path.
__contour_percent_encode() {
    # Bytes, not characters: in the C locale, which ends with this function, expansions count bytes.
    local LC_ALL=C
    local text=$1 kept code
    [[ -n ${2-} ]] && text=${text:0:$2}
    __contour_encoded=
    while [[ -n $text ]]; do
        # A run of bytes kept as they are, in one step; then the byte that ended it, as %XX.
        kept=${text%%[!A-Za-z0-9/._~-]*}
        __contour_encoded+=$kept
        text=${text:${#kept}}
        [[ -n $text ]] || break
        # "'x" is the byte's code in the C locale -- or, in musl's, 0xDF00 plus the byte for one above
        # 0x7F. Its low eight bits are the byte either way.
        printf -v code '%d' "'${text:0:1}"
        printf -v code '%%%02X' $(( code & 255 ))
        __contour_encoded+=$code
        text=${text:1}
    done
}

__contour_preexec() {
    # Text reflow back on for the command's output.
    printf '\e[?2028h'

    # OSC 133;C -- the command's output begins on the line the cursor is on now -- with the command
    # line, which bash-preexec hands over as $1: the last one history holds. That is this command's only
    # if history recorded it, which HISTIGNORE, HISTCONTROL, `set +o history` and HISTSIZE=0 each
    # prevent: history then still ends where it did at the prompt, and C goes out without a command
    # line, which the terminal then reads off the screen. (So does a command typed twice in a row.)
    if [[ -n $1 && $1 != "${__contour_history_at_prompt-}" ]]; then
        __contour_percent_encode "$1" "$__contour_cmdline_max_bytes"
        printf '\e]133;C;cmdline_url=%s\e\\' "$__contour_encoded"
    else
        printf '\e]133;C\e\\'
    fi

    __contour_command_running=1
}

__contour_precmd() {
    # Must be the very first line: anything else would clobber the exit status we are about to report.
    local exit_status=$?

    # OSC 133;D -- the command that just ran has finished. Only once a command actually HAS run: an
    # unconditional D would put a CommandEnd on the very first prompt and invent a command block that
    # never existed.
    if [[ -n "${__contour_command_running:-}" ]]; then
        printf '\e]133;D;%s\e\\' "$exit_status"
        unset __contour_command_running
    fi

    # The last command history holds, as bash-preexec will read it for the next one -- whose own it is
    # only if history has moved on by then.
    __contour_history_at_prompt=$(HISTTIMEFORMAT='' builtin history 1)
    __contour_history_at_prompt=${__contour_history_at_prompt#*[0-9][* ] }

    # Text reflow off for the prompt; the working directory (OSC 7) as a file:// URL, its path encoded
    # as a command line is -- so that no ESC or BEL in a directory name ends the sequence early -- once
    # per directory; OSC 133;A -- a new prompt starts on this line.
    if [[ $PWD != "${__contour_encoded_pwd_of-}" ]]; then
        __contour_percent_encode "$PWD"
        __contour_encoded_pwd=$__contour_encoded
        __contour_encoded_pwd_of=$PWD
    fi
    printf '\e[?2028l\e]7;file://%s%s\e\\\e]133;A\e\\' "$__contour_hostname" "$__contour_encoded_pwd"

    # Keep the prompt-end mark the LAST precmd hook, so that a prompt framework which registered a
    # PS1-building hook of its own after this file was sourced (starship, oh-my-bash) cannot build over
    # it. bash-preexec expanded the list before running it, so this counts from the next prompt on.
    local hook hooks=()
    for hook in "${precmd_functions[@]}"; do
        [[ $hook == __contour_mark_prompt_end ]] || hooks+=("$hook")
    done
    precmd_functions=("${hooks[@]}" __contour_mark_prompt_end)
}

# Appends the prompt-end mark to PS1 -- exactly once. PS1 outlives each prompt, and prompt frameworks
# rebuild it from scratch or around its old value, so any copy already in it is taken out first and the
# mark put back at the very end. A framework that rebuilds PS1 from PROMPT_COMMAND rather than from a
# precmd hook still runs after this and drops the mark: that prompt then has no prompt-end column,
# which costs nothing else -- the command line travels with ;C.
__contour_mark_prompt_end() {
    PS1=${PS1//"$__contour_prompt_end_mark"/}$__contour_prompt_end_mark
}

# Registered by name, and once: sourcing this file again must not run every hook twice.
[[ " ${preexec_functions[*]-} " == *" __contour_preexec "* ]] || preexec_functions+=(__contour_preexec)
[[ " ${precmd_functions[*]-} " == *" __contour_precmd "* ]] || precmd_functions+=(__contour_precmd)
[[ " ${precmd_functions[*]-} " == *" __contour_mark_prompt_end "* ]] || precmd_functions+=(__contour_mark_prompt_end)

# bash-preexec.sh -- Bash support for ZSH-like 'preexec' and 'precmd' functions.
# https://github.com/rcaloras/bash-preexec
#
#
# 'preexec' functions are executed before each interactive command is
# executed, with the interactive command as its argument. The 'precmd'
# function is executed before each prompt is displayed.
#
# Author: Ryan Caloras (ryan@bashhub.com)
# Forked from Original Author: Glyph Lefkowitz
#
# V0.5.0
#

# General Usage:
#
#  1. Source this file at the end of your bash profile so as not to interfere
#     with anything else that's using PROMPT_COMMAND.
#
#  2. Add any precmd or preexec functions by appending them to their arrays:
#       e.g.
#       precmd_functions+=(my_precmd_function)
#       precmd_functions+=(some_other_precmd_function)
#
#       preexec_functions+=(my_preexec_function)
#
#  3. Consider changing anything using the DEBUG trap or PROMPT_COMMAND
#     to use preexec and precmd instead. Preexisting usages will be
#     preserved, but doing so manually may be less surprising.
#
#  Note: This module requires two Bash features which you must not otherwise be
#  using: the "DEBUG" trap, and the "PROMPT_COMMAND" variable. If you override
#  either of these after bash-preexec has been installed it will most likely break.

# Tell shellcheck what kind of file this is.
# shellcheck shell=bash

# Make sure this is bash that's running and return otherwise.
# Use POSIX syntax for this line:
if [ -z "${BASH_VERSION-}" ]; then
    return 1;
fi

# We only support Bash 3.1+.
# Note: BASH_VERSINFO is first available in Bash-2.0.
if [[ -z "${BASH_VERSINFO-}" ]] || (( BASH_VERSINFO[0] < 3 || (BASH_VERSINFO[0] == 3 && BASH_VERSINFO[1] < 1) )); then
    return 1
fi

# Avoid duplicate inclusion
if [[ -n "${bash_preexec_imported:-}" || -n "${__bp_imported:-}" ]]; then
    return 0
fi
bash_preexec_imported="defined"

# WARNING: This variable is no longer used and should not be relied upon.
# Use ${bash_preexec_imported} instead.
# shellcheck disable=SC2034
__bp_imported="${bash_preexec_imported}"

# Should be available to each precmd and preexec
# functions, should they want it. $? and $_ are available as $? and $_, but
# $PIPESTATUS is available only in a copy, $BP_PIPESTATUS.
# TODO: Figure out how to restore PIPESTATUS before each precmd or preexec
# function.
__bp_last_ret_value="$?"
BP_PIPESTATUS=("${PIPESTATUS[@]}")
__bp_last_argument_prev_command="$_"

__bp_inside_precmd=0
__bp_inside_preexec=0

# Initial PROMPT_COMMAND string that is removed from PROMPT_COMMAND post __bp_install
__bp_install_string=$'__bp_trap_string="$(trap -p DEBUG)"\ntrap - DEBUG\n__bp_install'

# Fails if any of the given variables are readonly
# Reference https://stackoverflow.com/a/4441178
__bp_require_not_readonly() {
  local var
  for var; do
    if ! ( unset "$var" 2> /dev/null ); then
      echo "bash-preexec requires write access to ${var}" >&2
      return 1
    fi
  done
}

# Remove ignorespace and or replace ignoreboth from HISTCONTROL
# so we can accurately invoke preexec with a command from our
# history even if it starts with a space.
__bp_adjust_histcontrol() {
    local histcontrol
    histcontrol="${HISTCONTROL:-}"
    histcontrol="${histcontrol//ignorespace}"
    # Replace ignoreboth with ignoredups
    if [[ "$histcontrol" == *"ignoreboth"* ]]; then
        histcontrol="ignoredups:${histcontrol//ignoreboth}"
    fi;
    export HISTCONTROL="$histcontrol"
}

# This variable describes whether we are currently in "interactive mode";
# i.e. whether this shell has just executed a prompt and is waiting for user
# input.  It documents whether the current command invoked by the trace hook is
# run interactively by the user; it's set immediately after the prompt hook,
# and unset as soon as the trace hook is run.
__bp_preexec_interactive_mode=""

# These arrays are used to add functions to be run before, or after, prompts.
declare -a precmd_functions
declare -a preexec_functions

# Trims leading and trailing whitespace from $2 and writes it to the variable
# name passed as $1
__bp_trim_whitespace() {
    local var=${1:?} text=${2:-}
    text="${text#"${text%%[![:space:]]*}"}"   # remove leading whitespace characters
    text="${text%"${text##*[![:space:]]}"}"   # remove trailing whitespace characters
    printf -v "$var" '%s' "$text"
}


# Trims whitespace and removes any leading or trailing semicolons from $2 and
# writes the resulting string to the variable name passed as $1. Used for
# manipulating substrings in PROMPT_COMMAND
__bp_sanitize_string() {
    local var=${1:?} text=${2:-} sanitized
    __bp_trim_whitespace sanitized "$text"
    sanitized=${sanitized%;}
    sanitized=${sanitized#;}
    __bp_trim_whitespace sanitized "$sanitized"
    printf -v "$var" '%s' "$sanitized"
}

# This function is installed as part of the PROMPT_COMMAND;
# It sets a variable to indicate that the prompt was just displayed,
# to allow the DEBUG trap to know that the next command is likely interactive.
__bp_interactive_mode() {
    __bp_preexec_interactive_mode="on";
}


# This function is installed as part of the PROMPT_COMMAND.
# It will invoke any functions defined in the precmd_functions array.
__bp_precmd_invoke_cmd() {
    # Save the returned value from our last command, and from each process in
    # its pipeline. Note: this MUST be the first thing done in this function.
    # BP_PIPESTATUS may be unused, ignore
    # shellcheck disable=SC2034

    __bp_last_ret_value="$?" BP_PIPESTATUS=("${PIPESTATUS[@]}")

    # Don't invoke precmds if we are inside an execution of an "original
    # prompt command" by another precmd execution loop. This avoids infinite
    # recursion.
    if (( __bp_inside_precmd > 0 )); then
      return
    fi
    local __bp_inside_precmd=1

    # Invoke every function defined in our function array.
    local precmd_function
    for precmd_function in "${precmd_functions[@]}"; do

        # Only execute this function if it actually exists.
        # Test existence of functions with: declare -[Ff]
        if type -t "$precmd_function" 1>/dev/null; then
            __bp_set_ret_value "$__bp_last_ret_value" "$__bp_last_argument_prev_command"
            # Quote our function invocation to prevent issues with IFS
            "$precmd_function"
        fi
    done

    __bp_set_ret_value "$__bp_last_ret_value"
}

# Sets a return value in $?. We may want to get access to the $? variable in our
# precmd functions. This is available for instance in zsh. We can simulate it in bash
# by setting the value here.
__bp_set_ret_value() {
    return ${1:+"$1"}
}

__bp_in_prompt_command() {

    local prompt_command_array IFS=$'\n;'
    read -rd '' -a prompt_command_array <<< "${PROMPT_COMMAND[*]:-}"

    local trimmed_arg
    __bp_trim_whitespace trimmed_arg "${1:-}"

    local command trimmed_command
    for command in "${prompt_command_array[@]:-}"; do
        __bp_trim_whitespace trimmed_command "$command"
        if [[ "$trimmed_command" == "$trimmed_arg" ]]; then
            return 0
        fi
    done

    return 1
}

# This function is installed as the DEBUG trap.  It is invoked before each
# interactive prompt display.  Its purpose is to inspect the current
# environment to attempt to detect if the current command is being invoked
# interactively, and invoke 'preexec' if so.
__bp_preexec_invoke_exec() {

    # Save the contents of $_ so that it can be restored later on.
    # https://stackoverflow.com/questions/40944532/bash-preserve-in-a-debug-trap#40944702
    __bp_last_argument_prev_command="${1:-}"
    # Don't invoke preexecs if we are inside of another preexec.
    if (( __bp_inside_preexec > 0 )); then
      return
    fi
    local __bp_inside_preexec=1

    # Checks if the file descriptor is not standard out (i.e. '1')
    # __bp_delay_install checks if we're in test. Needed for bats to run.
    # Prevents preexec from being invoked for functions in PS1
    if [[ ! -t 1 && -z "${__bp_delay_install:-}" ]]; then
        return
    fi

    if [[ -n "${COMP_POINT:-}" || -n "${READLINE_POINT:-}" ]]; then
        # We're in the middle of a completer or a keybinding set up by "bind
        # -x".  This obviously can't be an interactively issued command.
        return
    fi
    if [[ -z "${__bp_preexec_interactive_mode:-}" ]]; then
        # We're doing something related to displaying the prompt.  Let the
        # prompt set the title instead of me.
        return
    else
        # If we're in a subshell, then the prompt won't be re-displayed to put
        # us back into interactive mode, so let's not set the variable back.
        # In other words, if you have a subshell like
        #   (sleep 1; sleep 2)
        # You want to see the 'sleep 2' as a set_command_title as well.
        if [[ 0 -eq "${BASH_SUBSHELL:-}" ]]; then
            __bp_preexec_interactive_mode=""
        fi
    fi

    if  __bp_in_prompt_command "${BASH_COMMAND:-}"; then
        # If we're executing something inside our prompt_command then we don't
        # want to call preexec. Bash prior to 3.1 can't detect this at all :/
        __bp_preexec_interactive_mode=""
        return
    fi

    local this_command
    this_command=$(
        export LC_ALL=C
        HISTTIMEFORMAT='' builtin history 1 | sed '1 s/^ *[0-9][0-9]*[* ] //'
    )

    # Contour: with nothing in the history (HISTSIZE=0) preexec still runs, with an empty command line, as
    # zsh's does while its history is off. Upstream returns here: no preexec, and so no OSC 133;C or D.

    # Invoke every function defined in our function array.
    local preexec_function
    local preexec_function_ret_value
    local preexec_ret_value=0
    for preexec_function in "${preexec_functions[@]:-}"; do

        # Only execute each function if it actually exists.
        # Test existence of function with: declare -[fF]
        if type -t "$preexec_function" 1>/dev/null; then
            __bp_set_ret_value "${__bp_last_ret_value:-}"
            # Quote our function invocation to prevent issues with IFS
            "$preexec_function" "$this_command"
            preexec_function_ret_value="$?"
            if [[ "$preexec_function_ret_value" != 0 ]]; then
                preexec_ret_value="$preexec_function_ret_value"
            fi
        fi
    done

    # Restore the last argument of the last executed command, and set the return
    # value of the DEBUG trap to be the return code of the last preexec function
    # to return an error.
    # If `extdebug` is enabled a non-zero return value from any preexec function
    # will cause the user's command not to execute.
    # Run `shopt -s extdebug` to enable
    __bp_set_ret_value "$preexec_ret_value" "$__bp_last_argument_prev_command"
}

__bp_install() {
    # Exit if we already have this installed.
    if [[ "${PROMPT_COMMAND[*]:-}" == *"__bp_precmd_invoke_cmd"* ]]; then
        return 1;
    fi

    trap '__bp_preexec_invoke_exec "$_"' DEBUG

    # Preserve any prior DEBUG trap as a preexec function
    local prior_trap
    # we can't easily do this with variable expansion. Leaving as sed command.
    # shellcheck disable=SC2001
    prior_trap=$(sed "s/[^']*'\(.*\)'[^']*/\1/" <<<"${__bp_trap_string:-}")
    unset __bp_trap_string
    if [[ -n "$prior_trap" ]]; then
        eval '__bp_original_debug_trap() {
          '"$prior_trap"'
        }'
        preexec_functions+=(__bp_original_debug_trap)
    fi

    # Adjust our HISTCONTROL Variable if needed.
    __bp_adjust_histcontrol

    # Issue #25. Setting debug trap for subshells causes sessions to exit for
    # backgrounded subshell commands (e.g. (pwd)& ). Believe this is a bug in Bash.
    #
    # Disabling this by default. It can be enabled by setting this variable.
    if [[ -n "${__bp_enable_subshells:-}" ]]; then

        # Set so debug trap will work be invoked in subshells.
        set -o functrace > /dev/null 2>&1
        shopt -s extdebug > /dev/null 2>&1
    fi;

    local existing_prompt_command
    # Remove setting our trap install string and sanitize the existing prompt command string
    existing_prompt_command="${PROMPT_COMMAND:-}"
    # Edge case of appending to PROMPT_COMMAND
    existing_prompt_command="${existing_prompt_command//$__bp_install_string/:}" # no-op
    existing_prompt_command="${existing_prompt_command//$'\n':$'\n'/$'\n'}" # remove known-token only
    existing_prompt_command="${existing_prompt_command//$'\n':;/$'\n'}" # remove known-token only
    __bp_sanitize_string existing_prompt_command "$existing_prompt_command"
    if [[ "${existing_prompt_command:-:}" == ":" ]]; then
        existing_prompt_command=
    fi

    # Install our hooks in PROMPT_COMMAND to allow our trap to know when we've
    # actually entered something.
    PROMPT_COMMAND='__bp_precmd_invoke_cmd'
    PROMPT_COMMAND+=${existing_prompt_command:+$'\n'$existing_prompt_command}
    if (( BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1) )); then
        PROMPT_COMMAND+=('__bp_interactive_mode')
    else
        # shellcheck disable=SC2179 # PROMPT_COMMAND is not an array in bash <= 5.0
        PROMPT_COMMAND+=$'\n__bp_interactive_mode'
    fi

    # Add two functions to our arrays for convenience
    # of definition.
    precmd_functions+=(precmd)
    preexec_functions+=(preexec)

    # Invoke our two functions manually that were added to $PROMPT_COMMAND
    __bp_precmd_invoke_cmd
    __bp_interactive_mode
}

# Sets an installation string as part of our PROMPT_COMMAND to install
# after our session has started. This allows bash-preexec to be included
# at any point in our bash profile.
__bp_install_after_session_init() {
    # bash-preexec needs to modify these variables in order to work correctly
    # if it can't, just stop the installation
    __bp_require_not_readonly PROMPT_COMMAND HISTCONTROL HISTTIMEFORMAT || return

    local sanitized_prompt_command
    __bp_sanitize_string sanitized_prompt_command "${PROMPT_COMMAND:-}"
    if [[ -n "$sanitized_prompt_command" ]]; then
        # shellcheck disable=SC2178 # PROMPT_COMMAND is not an array in bash <= 5.0
        PROMPT_COMMAND=${sanitized_prompt_command}$'\n'
    fi;
    # shellcheck disable=SC2179 # PROMPT_COMMAND is not an array in bash <= 5.0
    PROMPT_COMMAND+=${__bp_install_string}
}

# Run our install so long as we're not delaying it.
if [[ -z "${__bp_delay_install:-}" ]]; then
    __bp_install_after_session_init
fi;
