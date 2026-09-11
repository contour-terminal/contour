# Staging a build-tree executable's runtime dependencies beside it.
#
# Windows has no RPATH: the loader finds a DLL next to the executable, in the system directories, or
# on PATH -- and PATH is machine-global, so a Qt bin directory on it aims every other program on the
# machine at that Qt too. windeployqt covers the application target (see DeployQt.cmake); these
# cover the test executables it is not run for. Both are no-ops off Windows.

include_guard(GLOBAL)

# Copies the DLLs `target` links into its own output directory, after every build of it.
function(contour_stage_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()

    # `cmake -E copy_if_different` with no source is an error rather than a no-op, and a target
    # linking nothing shared has an empty TARGET_RUNTIME_DLLS -- so emptiness selects the command.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E
                "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different,true>"
                "$<TARGET_RUNTIME_DLLS:${target}>"
                "$<TARGET_FILE_DIR:${target}>"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Staging linked DLLs beside ${target} ...")
endfunction()

# Copies imported Qt plugin targets into `<output dir>/<category>`, which is where Qt looks for them
# relative to the executable. Nothing links a plugin, so TARGET_RUNTIME_DLLS cannot see one: which
# are needed follows from how the program is run, and so is named here. One this Qt does not provide
# is reported and skipped.
function(contour_stage_qt_plugins target category)
    if(NOT WIN32)
        return()
    endif()

    foreach(plugin IN LISTS ARGN)
        if(NOT TARGET ${plugin})
            message(STATUS "Qt plugin ${plugin} not provided: not staged beside ${target}")
            continue()
        endif()

        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/${category}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:${plugin}>"
                    "$<TARGET_FILE_DIR:${target}>/${category}"
            VERBATIM
            COMMENT "Staging ${plugin} beside ${target} ...")
    endforeach()
endfunction()
