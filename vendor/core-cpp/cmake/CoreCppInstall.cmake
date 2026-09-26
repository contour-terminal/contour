# SPDX-License-Identifier: Apache-2.0
#
# core_cpp_install()
#
# Installs core-cpp's module targets and exports them as the CMake package `core-cpp`, so that
#
#   find_package(core-cpp 0.3 CONFIG REQUIRED)
#   target_link_libraries(app PRIVATE core::net)
#
# links the same names a source build's aliases have (core-cpp#5). Every rule is in the install
# component `core-cpp`, so `cmake --install <build> --component core-cpp` installs core-cpp and none
# of the install rules of a dependency CPM fetched beside it.
#
# CORE_CPP_INSTALL decides whether any of this exists. It defaults to PROJECT_IS_TOP_LEVEL: a
# distribution building core-cpp by itself gets the rules, and a consumer that vendors it or adds it
# through CPM gets none, so its own `cmake --install` is unchanged. A parent that exports targets of
# its own linking core-cpp's -- morph -- turns it on: CMake refuses an export set whose targets link
# a target in no export set, and with it on core-cpp's are in `core-cppTargets`.
#
# WHAT IS INSTALLED. Every target core_cpp_add_module() created (the global property
# CORE_CPP_MODULE_TARGETS, in the table's order), with its HEADERS file set -- which includes the
# generated core/Config.hpp -- except a target that links something an installed package could not
# re-find:
#   - a dependency CPM FETCHED (Catch2, libunicode, Tracy when find_package() did not provide them):
#     it is a target of this build, not an imported one, so it is in no export set of ours and no
#     find_dependency() could bring it back;
#   - an imported target the dependency table (cmake/CoreCppDependencies.cmake) has no
#     find_package() row for;
#   - a core-cpp target left out for either reason.
# A target left out is reported with its reason, never dropped silently, and the reasons are kept in
# the global property CORE_CPP_INSTALL_SKIPPED for the check that installs the build
# (tests/cmake/check-install.cmake). A header-only dependency used only while compiling is linked as
# $<BUILD_INTERFACE:...> by its module (stb_image in core::tui), so it is no link dependency at all.
#
# WHAT THE PACKAGE RE-FINDS. core-cppConfig.cmake calls find_dependency() once per dependency-table
# row whose targets an installed target links, with that row's FIND_PACKAGE arguments. So Threads
# is re-found wherever core::base links it, OpenSSL only for a build with CORE_CPP_WITH_TLS, and
# nothing that the installed targets do not use.
#
# VERSIONING. SameMinorVersion while core-cpp is 0.x, because a minor release may break the API
# (CHANGELOG.md). At 1.0 this becomes SameMajorVersion.
#
# No global state: CMAKE_INSTALL_* come from GNUInstallDirs, included only when CORE_CPP_INSTALL is
# on, and nothing here sets a CMAKE_ variable.

include_guard(GLOBAL)

if(CORE_CPP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
endif()

set(CORE_CPP_INSTALL_COMPONENT core-cpp)
set(CORE_CPP_INSTALL_PACKAGE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/core-cpp")

## @brief Sets @p outVar to the dependency-table row that provides the imported target @p target
## and names a find_package() for it, or to "" if no row does.
function(core_cpp_install_dependency_of target outVar)
    set(row "")
    foreach(name IN LISTS CORE_CPP_DEPENDENCIES)
        if(target IN_LIST CORE_CPP_DEPENDENCY_${name}_TARGETS AND CORE_CPP_DEPENDENCY_${name}_FIND_PACKAGE)
            set(row "${name}")
            break()
        endif()
    endforeach()
    set(${outVar} "${row}" PARENT_SCOPE)
endfunction()

## @brief Decides whether @p target can be installed, given the core-cpp targets in @p installable.
## Sets @p reasonVar to why it cannot, or to "" if it can, and @p rowsVar to the dependency-table
## rows its links need re-found.
function(core_cpp_install_classify target installable reasonVar rowsVar)
    set(reason "")
    set(rows "")
    set(items "")
    foreach(property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(linked ${target} ${property})
        if(linked)
            list(APPEND items ${linked})
        endif()
    endforeach()
    foreach(item IN LISTS items)
        # Used only while building, so it never reaches the export.
        if(item MATCHES "^\\$<BUILD_INTERFACE:")
            continue()
        endif()
        if(item MATCHES "^\\$<(LINK_ONLY|TARGET_OBJECTS):(.+)>$")
            set(item "${CMAKE_MATCH_2}")
        endif()
        # A flag, a system library by name, or a generator expression of another kind.
        if(NOT TARGET "${item}")
            continue()
        endif()
        get_target_property(aliased "${item}" ALIASED_TARGET)
        if(aliased)
            set(item "${aliased}")
        endif()
        if(item MATCHES "^core-cpp-")
            if(NOT item IN_LIST installable)
                set(reason "it links ${item}, which is not installed")
                break()
            endif()
            continue()
        endif()
        get_target_property(imported "${item}" IMPORTED)
        if(NOT imported)
            set(reason
                "it links ${item}, which this build fetched rather than found, so an installed package could not re-find it")
            break()
        endif()
        core_cpp_install_dependency_of("${item}" row)
        if(NOT row)
            set(reason "it links ${item}, which no find_package() row of cmake/CoreCppDependencies.cmake provides")
            break()
        endif()
        list(APPEND rows ${row})
    endforeach()
    list(REMOVE_DUPLICATES rows)
    set(${reasonVar} "${reason}" PARENT_SCOPE)
    set(${rowsVar} "${rows}" PARENT_SCOPE)
endfunction()

function(core_cpp_install)
    if(NOT CORE_CPP_INSTALL)
        return()
    endif()
    get_property(targets GLOBAL PROPERTY CORE_CPP_MODULE_TARGETS)
    set(installable "")
    set(skipped "")
    set(rows "")
    # In the table's order, so a target's core-cpp dependencies are classified before it is.
    foreach(target IN LISTS targets)
        core_cpp_install_classify(${target} "${installable}" reason targetRows)
        if(reason)
            message(STATUS "[core-cpp] install: ${target} is not installed: ${reason}")
            list(APPEND skipped "${target}: ${reason}")
            continue()
        endif()
        list(APPEND installable ${target})
        list(APPEND rows ${targetRows})
    endforeach()
    set_property(GLOBAL PROPERTY CORE_CPP_INSTALLED_TARGETS ${installable})
    set_property(GLOBAL PROPERTY CORE_CPP_INSTALL_SKIPPED ${skipped})
    if(NOT installable)
        message(STATUS "[core-cpp] install: no target can be installed")
        return()
    endif()

    install(TARGETS ${installable}
        EXPORT core-cppTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${CORE_CPP_INSTALL_COMPONENT}
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT ${CORE_CPP_INSTALL_COMPONENT}
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT ${CORE_CPP_INSTALL_COMPONENT}
        OBJECTS DESTINATION "${CMAKE_INSTALL_LIBDIR}/core-cpp-objects" COMPONENT ${CORE_CPP_INSTALL_COMPONENT}
        FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT ${CORE_CPP_INSTALL_COMPONENT})
    install(EXPORT core-cppTargets
        NAMESPACE core::
        DESTINATION "${CORE_CPP_INSTALL_PACKAGE_DIR}"
        COMPONENT ${CORE_CPP_INSTALL_COMPONENT})

    list(REMOVE_DUPLICATES rows)
    set(findDependencies "")
    foreach(row IN LISTS rows)
        list(JOIN CORE_CPP_DEPENDENCY_${row}_FIND_PACKAGE " " arguments)
        string(APPEND findDependencies "find_dependency(${arguments})\n")
    endforeach()
    set(CORE_CPP_INSTALL_FIND_DEPENDENCIES "${findDependencies}")
    set(generated "${CORE_CPP_BINARY_DIR}/core-cpp-package")
    configure_package_config_file("${CORE_CPP_SOURCE_DIR}/cmake/core-cppConfig.cmake.in"
        "${generated}/core-cppConfig.cmake"
        INSTALL_DESTINATION "${CORE_CPP_INSTALL_PACKAGE_DIR}")
    write_basic_package_version_file("${generated}/core-cppConfigVersion.cmake"
        VERSION "${core-cpp_VERSION}"
        COMPATIBILITY SameMinorVersion)
    install(FILES "${generated}/core-cppConfig.cmake" "${generated}/core-cppConfigVersion.cmake"
        DESTINATION "${CORE_CPP_INSTALL_PACKAGE_DIR}"
        COMPONENT ${CORE_CPP_INSTALL_COMPONENT})

    list(LENGTH installable installedCount)
    message(STATUS "[core-cpp] install: ${installedCount} target(s) exported as the package core-cpp")
endfunction()
