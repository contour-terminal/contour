# Applies the Windows installer scope selected by CPACK_CONTOUR_WIX_SCOPE (default perMachine), for the WiX
# generator, from the table src/contour/CMakeLists.txt writes into CPackConfig.cmake, and names the Windows
# packages.
#
# CPack includes this file through CPACK_PROJECT_CONFIG_FILE once per generator, at package time, after
# CPackConfig.cmake and the command line's -D options: the scope can therefore be chosen when packaging
# (`cpack -D CPACK_CONTOUR_WIX_SCOPE=perUser`, or the package preset msvc-release-user) without reconfiguring.

set(_contour_file_suffix "")

if(CPACK_GENERATOR STREQUAL "WIX")
    # CPACK_WIX_VERSION, which this project sets to 4, is unknown to older CMake: CPack would run WiX 3 on
    # sources written for WiX 4, and fail with errors that do not say why.
    if(CMAKE_VERSION VERSION_LESS 3.30)
        message(FATAL_ERROR "the Windows installers need CMake 3.30 or newer: CPACK_WIX_VERSION 4, which their "
                            "WiX sources are written for, is ignored by CPack before that. Found ${CMAKE_VERSION}")
    endif()
    if(NOT CPACK_CONTOUR_WIX_SCOPE)
        set(CPACK_CONTOUR_WIX_SCOPE perMachine)
    endif()
    # list(FIND), not IN_LIST: policy CMP0057 is not set in a CPack project config, and without it CMake
    # before 4.0 rejects IN_LIST as an unknown argument.
    list(FIND CPACK_CONTOUR_WIX_SCOPES "${CPACK_CONTOUR_WIX_SCOPE}" _contour_scope_index)
    if(_contour_scope_index EQUAL -1)
        string(REPLACE ";" ", " _known_scopes "${CPACK_CONTOUR_WIX_SCOPES}")
        message(FATAL_ERROR "unknown Windows installer scope '${CPACK_CONTOUR_WIX_SCOPE}'; "
                            "the known ones are: ${_known_scopes}")
    endif()
    # Per-user installs need CPack's perUser support, which arrived with 4.3.
    if(CPACK_CONTOUR_WIX_SCOPE STREQUAL "perUser" AND CMAKE_VERSION VERSION_LESS 4.3)
        message(FATAL_ERROR "the per-user MSI needs CPack 4.3 or newer for CPACK_WIX_INSTALL_SCOPE=perUser, "
                            "found ${CMAKE_VERSION}")
    endif()

    set(CPACK_WIX_INSTALL_SCOPE "${CPACK_CONTOUR_WIX_SCOPE}")
    set(CPACK_WIX_UPGRADE_GUID "${CPACK_CONTOUR_WIX_${CPACK_CONTOUR_WIX_SCOPE}_UPGRADE_GUID}")
    set(CPACK_WIX_EXTRA_SOURCES "${CPACK_CONTOUR_WIX_SOURCE}")
    # The source branches on the scope, by this variable of the WiX preprocessor.
    set(CPACK_WIX_BUILD_EXTRA_FLAGS "-d;ContourScope=${CPACK_CONTOUR_WIX_SCOPE}")
    # A '/' makes CPack nest the folders (Programs/Contour is Programs below the scope's base, then Contour).
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_CONTOUR_WIX_${CPACK_CONTOUR_WIX_SCOPE}_INSTALL_DIRECTORY}")
    set(_contour_file_suffix "${CPACK_CONTOUR_WIX_${CPACK_CONTOUR_WIX_SCOPE}_FILE_SUFFIX}")
endif()

# Every Windows package is named here, at package time, so that a version given on the command line
# (-D CPACK_PACKAGE_VERSION=...) names the file too, and so that the zip and the MSIs agree on the spelling.
set(CPACK_PACKAGE_FILE_NAME "contour-${CPACK_PACKAGE_VERSION}-win64${_contour_file_suffix}")
