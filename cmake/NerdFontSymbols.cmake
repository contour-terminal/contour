# SPDX-License-Identifier: Apache-2.0
#
# Bundles the Nerd Fonts "Symbols Only" font, so that the glyphs of Nerd Fonts -- Powerline, Font Awesome,
# Material Design, Devicons, Codicons and the rest, all in the Unicode private use areas -- render whatever
# font the user has chosen. The terminal falls back to it for exactly those code points (see
# text::NerdFontSymbolsLocator), after the user's own fallbacks and before the system's.
#
# The font is NOT kept in the repository: it is 2.6 MB of binary that changes with every Nerd Fonts release.
# It comes from the release archive on GitHub, pinned by version and SHA-256, and there are three ways to
# get it, chosen with CONTOUR_NERD_FONT_SYMBOLS:
#
#   FETCH    (default) Download the archive at configure time. Nothing else to do.
#   ARCHIVE  Use the archive CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE names, which must be the one the pinned
#            version and hash describe. For builds with no network access: Flatpak/Flathub, distribution
#            build roots and the like fetch the archive themselves, as a checksummed source, and hand it in:
#
#                -DCONTOUR_NERD_FONT_SYMBOLS=ARCHIVE
#                -DCONTOUR_NERD_FONT_SYMBOLS_ARCHIVE=/path/to/NerdFontsSymbolsOnly.zip
#
#   OFF      Bundle nothing. Glyphs of Nerd Fonts then render only where the chosen font, or one the system
#            offers, has them.
#
# Upgrading is two values, VERSION and SHA256 below, and the code point table the script
# scripts/generate-nerd-font-ranges.py prints for the new font (src/text_shaper/NerdFontSymbolRanges.hpp);
# a test in text_shaper_test fails until that table matches what the font covers.
#
# Sets, for the directories that include this one:
#   CONTOUR_NERD_FONT_SYMBOLS_DIR    The directory holding the font and its licence, or empty when none is
#                                    bundled. A cache variable, so every subdirectory sees it.

set(CONTOUR_NERD_FONT_SYMBOLS "FETCH" CACHE STRING
    "How the Nerd Fonts symbols font is bundled: FETCH (download it), ARCHIVE (use CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE) or OFF")
set_property(CACHE CONTOUR_NERD_FONT_SYMBOLS PROPERTY STRINGS FETCH ARCHIVE OFF)

# The release to bundle. To upgrade, change these two (the hash is that of NerdFontsSymbolsOnly.zip on the
# release page) and regenerate the code point table.
set(CONTOUR_NERD_FONT_SYMBOLS_VERSION "v3.5.1" CACHE STRING "The Nerd Fonts release the symbols font is taken from")
set(CONTOUR_NERD_FONT_SYMBOLS_SHA256
    "fdca3682534f6f65e1ccb2345b0362ccf67d9b8eca7c8025330946e93e2473bc"
    CACHE STRING "SHA-256 of NerdFontsSymbolsOnly.zip of that release")
set(CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE "" CACHE FILEPATH
    "With CONTOUR_NERD_FONT_SYMBOLS=ARCHIVE: the local NerdFontsSymbolsOnly.zip to use")

set(CONTOUR_NERD_FONT_SYMBOLS_DIR "" CACHE INTERNAL "Where the bundled Nerd Fonts symbols font was staged")

set(_nerd_font_file "SymbolsNerdFontMono-Regular.ttf")
set(_nerd_font_license "LICENSE-NerdFontsSymbols.txt")

if(NOT CONTOUR_NERD_FONT_SYMBOLS MATCHES "^(FETCH|ARCHIVE|OFF)$")
    message(FATAL_ERROR
        "CONTOUR_NERD_FONT_SYMBOLS is '${CONTOUR_NERD_FONT_SYMBOLS}'; it must be FETCH, ARCHIVE or OFF.")
endif()

if(CONTOUR_NERD_FONT_SYMBOLS STREQUAL "OFF")
    set(CONTOUR_NERD_FONT_SYMBOLS_DIR "" CACHE INTERNAL "")
    message(STATUS "Nerd Fonts symbols: not bundled (CONTOUR_NERD_FONT_SYMBOLS=OFF)")
    return()
endif()

set(_nerd_font_stage "${CMAKE_BINARY_DIR}/bundled-fonts/nerd-fonts-symbols-${CONTOUR_NERD_FONT_SYMBOLS_VERSION}")

if(NOT EXISTS "${_nerd_font_stage}/${_nerd_font_file}" OR NOT EXISTS "${_nerd_font_stage}/${_nerd_font_license}")
    if(CONTOUR_NERD_FONT_SYMBOLS STREQUAL "FETCH")
        set(_nerd_font_archive "${CMAKE_BINARY_DIR}/_downloads/NerdFontsSymbolsOnly-${CONTOUR_NERD_FONT_SYMBOLS_VERSION}.zip")
        set(_nerd_font_url
            "https://github.com/ryanoasis/nerd-fonts/releases/download/${CONTOUR_NERD_FONT_SYMBOLS_VERSION}/NerdFontsSymbolsOnly.zip")
        message(STATUS "Nerd Fonts symbols: downloading ${_nerd_font_url}")
        # EXPECTED_HASH skips the download when the file is already there and matches, and fails the
        # download when it does not: what is bundled is what the hash above names, never what a mirror
        # happened to serve.
        file(DOWNLOAD "${_nerd_font_url}" "${_nerd_font_archive}"
            EXPECTED_HASH "SHA256=${CONTOUR_NERD_FONT_SYMBOLS_SHA256}"
            STATUS _nerd_font_status)
        list(GET _nerd_font_status 0 _nerd_font_code)
        if(NOT _nerd_font_code EQUAL 0)
            file(REMOVE "${_nerd_font_archive}")
            message(FATAL_ERROR
                "Could not fetch the Nerd Fonts symbols font: ${_nerd_font_status}\n"
                "Without network access, pass the archive in:\n"
                "  -DCONTOUR_NERD_FONT_SYMBOLS=ARCHIVE -DCONTOUR_NERD_FONT_SYMBOLS_ARCHIVE=/path/to/NerdFontsSymbolsOnly.zip\n"
                "or build without it: -DCONTOUR_NERD_FONT_SYMBOLS=OFF")
        endif()
    else()
        if(NOT CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE OR NOT EXISTS "${CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE}")
            message(FATAL_ERROR
                "CONTOUR_NERD_FONT_SYMBOLS=ARCHIVE needs CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE to name an existing "
                "NerdFontsSymbolsOnly.zip (it is '${CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE}').")
        endif()
        set(_nerd_font_archive "${CONTOUR_NERD_FONT_SYMBOLS_ARCHIVE}")
        file(SHA256 "${_nerd_font_archive}" _nerd_font_actual)
        if(NOT _nerd_font_actual STREQUAL CONTOUR_NERD_FONT_SYMBOLS_SHA256)
            message(FATAL_ERROR
                "${_nerd_font_archive} has SHA-256 ${_nerd_font_actual}, but Nerd Fonts "
                "${CONTOUR_NERD_FONT_SYMBOLS_VERSION} is pinned to ${CONTOUR_NERD_FONT_SYMBOLS_SHA256}. Hand in "
                "that release's archive, or set CONTOUR_NERD_FONT_SYMBOLS_VERSION and "
                "CONTOUR_NERD_FONT_SYMBOLS_SHA256 to the one you have.")
        endif()
    endif()

    file(REMOVE_RECURSE "${_nerd_font_stage}")
    file(MAKE_DIRECTORY "${_nerd_font_stage}")
    file(ARCHIVE_EXTRACT INPUT "${_nerd_font_archive}" DESTINATION "${_nerd_font_stage}"
        PATTERNS "${_nerd_font_file}" "LICENSE")
    if(NOT EXISTS "${_nerd_font_stage}/${_nerd_font_file}" OR NOT EXISTS "${_nerd_font_stage}/LICENSE")
        message(FATAL_ERROR "${_nerd_font_archive} holds no ${_nerd_font_file} and LICENSE.")
    endif()
    file(RENAME "${_nerd_font_stage}/LICENSE" "${_nerd_font_stage}/${_nerd_font_license}")
endif()

set(CONTOUR_NERD_FONT_SYMBOLS_DIR "${_nerd_font_stage}" CACHE INTERNAL "")
message(STATUS "Nerd Fonts symbols: bundling ${CONTOUR_NERD_FONT_SYMBOLS_VERSION} from ${_nerd_font_stage}")
