# Packaging check for the contour_e2e_packaging_rules test: asserts that a build configuration
# installs, and declares for CPack, exactly what its Debian package is supposed to carry.
#
# Run by ctest as
#   cmake -D FRONTEND=gui|headless -D INSTALL_SCRIPT=<cmake_install.cmake> -D CPACK_CONFIG=<CPackConfig.cmake> -P PackagingRules.cmake
#
# It reads the generated install rules and CPack configuration rather than running `cpack`, so it
# needs no built tree and no dpkg: a GUI build tree cannot produce the daemon package anyway, and the
# real install-and-list check of the GUI-less package runs in CI's daemon-package job.
#
# NB: `cmake -P` exits 0 even after message(FATAL_ERROR) (CMake < 3.31); the verdict is the marker
# printed last, which the test's PASS_REGULAR_EXPRESSION requires.

if(NOT FRONTEND STREQUAL "gui" AND NOT FRONTEND STREQUAL "headless")
    message(FATAL_ERROR "FRONTEND must be 'gui' or 'headless', got '${FRONTEND}'")
endif()
foreach(_file IN ITEMS INSTALL_SCRIPT CPACK_CONFIG)
    if(NOT EXISTS "${${_file}}")
        message(FATAL_ERROR "${_file} does not name an existing file: '${${_file}}'")
    endif()
endforeach()

file(READ "${INSTALL_SCRIPT}" installRules)
include("${CPACK_CONFIG}")

# Install rules addressed at a desktop environment, and the OOM drop-ins for the app-* units.
set(DESKTOP_ONLY_RULES ".desktop" "kio/servicemenus" "icons/hicolor" "/metainfo" "oom-policy")
# What both packages ship.
set(ALWAYS_RULES "shell-integration" "contour-daemon.service" "share/contour/fonts")

foreach(_rule IN LISTS ALWAYS_RULES)
    string(FIND "${installRules}" "${_rule}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "${FRONTEND}: install rules lack '${_rule}'")
    endif()
endforeach()

foreach(_rule IN LISTS DESKTOP_ONLY_RULES)
    string(FIND "${installRules}" "${_rule}" _at)
    if(FRONTEND STREQUAL "gui" AND _at EQUAL -1)
        message(FATAL_ERROR "gui: install rules lack the desktop file '${_rule}'")
    elseif(FRONTEND STREQUAL "headless" AND NOT _at EQUAL -1)
        message(FATAL_ERROR "headless: install rules carry the desktop file '${_rule}'")
    endif()
endforeach()

if(FRONTEND STREQUAL "gui")
    set(expectedName "contour")
    set(expectedPeer "contour-daemon")
    set(expectedSection "x11")
else()
    set(expectedName "contour-daemon")
    set(expectedPeer "contour")
    set(expectedSection "admin")
endif()

foreach(_check
        "CPACK_DEBIAN_CONTOUR_PACKAGE_NAME|${expectedName}"
        "CPACK_DEBIAN_CONTOUR_PACKAGE_CONFLICTS|${expectedPeer}"
        "CPACK_DEBIAN_CONTOUR_PACKAGE_REPLACES|${expectedPeer}"
        "CPACK_DEBIAN_PACKAGE_SECTION|${expectedSection}")
    string(REPLACE "|" ";" _check "${_check}")
    list(GET _check 0 _var)
    list(GET _check 1 _want)
    if(NOT "${${_var}}" STREQUAL "${_want}")
        message(FATAL_ERROR "${FRONTEND}: ${_var} is '${${_var}}', expected '${_want}'")
    endif()
endforeach()

string(TOLOWER "${CPACK_DEBIAN_PACKAGE_DEPENDS}" dependsLower)
string(REGEX MATCH "qt|qml" qtDepends "${dependsLower}")
if(FRONTEND STREQUAL "headless")
    if(NOT qtDepends STREQUAL "")
        message(FATAL_ERROR "headless: Depends names Qt: ${CPACK_DEBIAN_PACKAGE_DEPENDS}")
    endif()
    if(NOT "${CPACK_DEBIAN_PACKAGE_RECOMMENDS}" STREQUAL "")
        message(FATAL_ERROR "headless: Recommends is '${CPACK_DEBIAN_PACKAGE_RECOMMENDS}', expected none")
    endif()
    if(NOT CPACK_DEBIAN_CONTOUR_FILE_NAME STREQUAL "DEB-DEFAULT")
        message(FATAL_ERROR "headless: the .deb file name is not derived from the package name")
    endif()
elseif(qtDepends STREQUAL "")
    message(FATAL_ERROR "gui: Depends names no Qt: ${CPACK_DEBIAN_PACKAGE_DEPENDS}")
endif()

message(STATUS "packaging-rules: OK")
