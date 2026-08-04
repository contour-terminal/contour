# SPDX-License-Identifier: Apache-2.0
#
# CPACK_PRE_BUILD_SCRIPTS hook: notarize and staple the staged contour.app before
# CPack hands the directory to hdiutil.
#
# Why here and not after the .dmg is built: stapling attaches the notarization ticket
# to the artifact itself. A ticket on the disk image alone covers the download, but the
# app the user drags to /Applications carries none, so its first launch depends on
# Gatekeeper reaching Apple over the network. Stapling the app *inside* the image makes
# the installed app self-sufficient. The image gets its own ticket in MacOSSignDmg.cmake.
#
# That costs a second round trip to Apple (a minute or more each), which only pays for
# itself on something people download, so it is gated on CONTOUR_MACOS_STAPLE_APP
# rather than on notarization as a whole.
#
# Variables come from the CPack config (see the APPLE branch of src/contour/CMakeLists.txt).

if(NOT CPACK_CONTOUR_STAPLE_APP)
    return()
endif()

# Only the disk image carries an app bundle. Without this guard, `cpack -G TGZ` on macOS
# would submit whatever it staged to Apple.
if(NOT CPACK_GENERATOR STREQUAL "DragNDrop")
    return()
endif()

# CPack stages the bundle one directory deeper than the obvious path: this project defines
# an install component, so the layout is <staging>/ALL_IN_ONE/contour.app rather than
# <staging>/contour.app. Both are checked, because that is a CPack implementation detail
# and not something worth breaking a release over.
set(_candidates "")
foreach(_pattern "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/contour.app"
                 "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*/contour.app")
    file(GLOB _found "${_pattern}")
    list(APPEND _candidates ${_found})
endforeach()
list(REMOVE_DUPLICATES _candidates)
list(LENGTH _candidates _count)

if(_count EQUAL 0)
    message(FATAL_ERROR
        "MacOSNotarizeApp: no contour.app found under "
        "${CPACK_TEMPORARY_INSTALL_DIRECTORY} (searched it and its immediate "
        "subdirectories). The app is notarized before hdiutil wraps it, so this has to "
        "run after the install step has staged the bundle.")
elseif(_count GREATER 1)
    message(FATAL_ERROR
        "MacOSNotarizeApp: ambiguous staging layout, found several bundles: ${_candidates}")
endif()

list(GET _candidates 0 _app)

message(STATUS "Notarizing ${_app}")
execute_process(
    COMMAND "${CPACK_CONTOUR_PYTHON}" "${CPACK_CONTOUR_BUNDLE_SCRIPT}" notarize "${_app}"
            --keychain-profile "${CPACK_CONTOUR_NOTARY_PROFILE}"
    COMMAND_ERROR_IS_FATAL ANY
)
