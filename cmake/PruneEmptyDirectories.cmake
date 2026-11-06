# Removes the directories below DIRECTORY that hold nothing, the ones that held nothing but other empty
# directories included. Run as a post-build step after windeployqt, which leaves the directories of the Qt plugins
# it found no file for (qml/QtQuick/VirtualKeyboard/Plugins/*, ...): an installer would carry each as a component
# of its own, and the per-user MSI cannot give those a valid key path.
#
#   cmake -D DIRECTORY=<dir> -P PruneEmptyDirectories.cmake

if(NOT IS_DIRECTORY "${DIRECTORY}")
    message(FATAL_ERROR "DIRECTORY does not name a directory: '${DIRECTORY}'")
endif()

file(GLOB_RECURSE _entries LIST_DIRECTORIES true "${DIRECTORY}/*")
# A directory sorts before what is inside it, so the reverse order empties the deepest ones first.
list(SORT _entries COMPARE NATURAL ORDER DESCENDING)
foreach(_entry IN LISTS _entries)
    if(IS_DIRECTORY "${_entry}")
        file(GLOB _children "${_entry}/*")
        if(NOT _children)
            file(REMOVE_RECURSE "${_entry}")
        endif()
    endif()
endforeach()
