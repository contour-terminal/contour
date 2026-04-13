# FindQt.cmake — Auto-detect Qt6 installation on Windows and macOS.
#
# On Windows, scans QT_ROOT_DIR (default: C:/Qt) for the newest Qt 6.x
# version, preferring msvc2022_64 over msvc2019_64. Appends the found
# cmake directory to CMAKE_PREFIX_PATH so that subsequent
# find_package(Qt6 ...) calls succeed.
#
# On macOS, checks common Homebrew install locations for Qt.
#
# Handles stale caches gracefully: if Qt6_DIR points to a removed
# Qt version, the detection re-runs automatically.
#
# This module is a no-op when Qt6_DIR already points to a valid directory
# (e.g., set by VCPKG, presets, or CI).

# Only run on Windows and macOS — Linux and *BSD install Qt system-wide.
if(NOT WIN32 AND NOT APPLE)
    return()
endif()

# Re-detect only if Qt6_DIR is unset, NOTFOUND, or points to a removed directory.
if(Qt6_DIR AND EXISTS "${Qt6_DIR}")
    message(STATUS "[FindQt] Qt6_DIR already set to ${Qt6_DIR}, skipping auto-detection.")
    return()
endif()

if(WIN32)
    # Determine QT_ROOT_DIR: explicit cache variable > environment > well-known paths
    if(NOT QT_ROOT_DIR)
        if(DEFINED ENV{QT_ROOT_DIR} AND EXISTS "$ENV{QT_ROOT_DIR}")
            set(QT_ROOT_DIR "$ENV{QT_ROOT_DIR}")
        else()
            foreach(_candidate "C:/Qt" "D:/Qt")
                if(EXISTS "${_candidate}")
                    set(QT_ROOT_DIR "${_candidate}")
                    break()
                endif()
            endforeach()
        endif()
    endif()
    set(QT_ROOT_DIR "${QT_ROOT_DIR}" CACHE PATH "Root directory of Qt installations (e.g. C:/Qt)")

    if(NOT QT_ROOT_DIR OR NOT EXISTS "${QT_ROOT_DIR}")
        message(STATUS "[FindQt] Qt root directory not found. Set QT_ROOT_DIR or CMAKE_PREFIX_PATH manually.")
        return()
    endif()

    message(STATUS "[FindQt] Searching for Qt6 in ${QT_ROOT_DIR} ...")

    # Strip stale Qt paths from CMAKE_PREFIX_PATH to prevent accumulation
    set(_qt_cleaned_prefix_path "")
    foreach(_path IN LISTS CMAKE_PREFIX_PATH)
        string(FIND "${_path}" "${QT_ROOT_DIR}/" _qt_path_match)
        if(_qt_path_match LESS 0)
            list(APPEND _qt_cleaned_prefix_path "${_path}")
        endif()
    endforeach()
    set(CMAKE_PREFIX_PATH "${_qt_cleaned_prefix_path}")

    # Glob for 6.* version directories, sort naturally (newest first)
    file(GLOB _qt_versions RELATIVE "${QT_ROOT_DIR}" "${QT_ROOT_DIR}/6.*")
    list(SORT _qt_versions COMPARE NATURAL)
    list(REVERSE _qt_versions)

    set(_qt_found FALSE)
    foreach(_qt_ver IN LISTS _qt_versions)
        set(_qt_ver_path "${QT_ROOT_DIR}/${_qt_ver}")
        if(IS_DIRECTORY "${_qt_ver_path}")
            foreach(_compiler IN ITEMS msvc2022_64 msvc2019_64)
                set(_qt_cmake_path "${_qt_ver_path}/${_compiler}/lib/cmake")
                if(EXISTS "${_qt_cmake_path}")
                    list(APPEND CMAKE_PREFIX_PATH "${_qt_cmake_path}")
                    message(STATUS "[FindQt] Found Qt ${_qt_ver} (${_compiler})")
                    set(_qt_found TRUE)
                    break()
                endif()
            endforeach()
            if(_qt_found)
                break()
            endif()
        endif()
    endforeach()

    if(NOT _qt_found)
        message(STATUS "[FindQt] No Qt 6.x installation found in ${QT_ROOT_DIR}.")
    endif()

    # Force update the cache so subsequent find_package picks up the new path.
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE STRING "" FORCE)
    unset(Qt6_DIR CACHE)

elseif(APPLE)
    # Check common Homebrew paths (Apple Silicon, then Intel)
    set(_qt_found FALSE)
    foreach(_candidate "/opt/homebrew/opt/qt" "/usr/local/opt/qt")
        if(EXISTS "${_candidate}/lib/cmake/Qt6")
            list(APPEND CMAKE_PREFIX_PATH "${_candidate}")
            message(STATUS "[FindQt] Found Qt via Homebrew at ${_candidate}")
            set(_qt_found TRUE)
            break()
        endif()
    endforeach()

    if(NOT _qt_found)
        message(STATUS "[FindQt] No Homebrew Qt installation found. Set CMAKE_PREFIX_PATH manually.")
    endif()

    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE STRING "" FORCE)
    unset(Qt6_DIR CACHE)
endif()
