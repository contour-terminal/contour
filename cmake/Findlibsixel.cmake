# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindLibsixel
------------

Find Libsixel headers and library.

Imported Targets
^^^^^^^^^^^^^^^^

``Libsixel::Libsixel``
  The Libsixel library, if found.

Result Variables
^^^^^^^^^^^^^^^^

This will define the following variables in your project:

``Libsixel_FOUND``
  true if (the requested version of) Fontconfig is available.
``Libsixel_VERSION``
  the version of Fontconfig.
``Libsixel_LIBRARIES``
  the libraries to link against to use Fontconfig.
``Libsixel_INCLUDE_DIRS``
  where to find the Fontconfig headers.
``Libsixel_COMPILE_OPTIONS``
  this should be passed to target_compile_options(), if the
  target is not used for linking

#]=======================================================================]

find_package(PkgConfig QUIET)
pkg_check_modules(PKG_LIBSIXEL QUIET libsixel)
set(libsixel_COMPILE_OPTIONS ${PKG_LIBSIXEL_CFLAGS_OTHER})
#set(libsixel_VERSION ${PKG_LIBSIXEL_VERSION})

find_path(libsixel_INCLUDE_DIR
  NAMES
    sixel.h
  HINTS
    ${PKG_LIBSIXEL_INCLUDE_DIRS}
)

find_library(libsixel_LIBRARY
  NAMES
    sixel
  PATHS
    ${PKG_LIBSIXEL_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libsixel
  FOUND_VAR
    libsixel_FOUND
  REQUIRED_VARS
    libsixel_LIBRARY
    libsixel_INCLUDE_DIR
  VERSION_VAR
    libsixel_VERSION
)

if(libsixel_FOUND AND NOT TARGET Libsixel::Libsixel)
    add_library(Libsixel::Libsixel UNKNOWN IMPORTED)
    set_target_properties(Libsixel::Libsixel PROPERTIES
    IMPORTED_LOCATION "${libsixel_LIBRARY}"
    INTERFACE_COMPILE_OPTIONS "${libsixel_COMPILE_OPTIONS}"
    INTERFACE_INCLUDE_DIRECTORIES "${libsixel_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(libsixel_LIBRARY libsixel_INCLUDE_DIR)

if(libsixel_FOUND)
  set(libsixel_LIBRARIES ${libsixel_LIBRARY})
  set(libsixel_INCLUDE_DIRS ${libsixel_INCLUDE_DIR})
endif()
