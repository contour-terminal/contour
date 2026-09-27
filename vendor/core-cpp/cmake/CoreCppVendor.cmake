# SPDX-License-Identifier: Apache-2.0
#
# The vendoring tool of Part I §5 of the design spec: it exports a verbatim copy of core-cpp into
# a consumer's tree, and it verifies that such a copy is still verbatim.
#
#   cmake -DMODE=sync -DREF=<tag or full SHA> -DDEST=<dir> [-DREPO=<url or path>]
#         ["-DMODULES=base;log;cli;platform;async;net;testing"]
#         -P <core-cpp checkout>/cmake/CoreCppVendor.cmake
#
#   cmake -DMODE=check -DDEST=<dir> -P <dir>/cmake/CoreCppVendor.cmake
#
# MODULES is a CMake list, so its -D argument is quoted: an unquoted semicolon is the shell's.
#
# sync reads git BLOBS, never a working tree: `git -c core.autocrlf=false -c core.eol=lf cat-file
# blob` hands over the bytes the commit records, whatever the machine's line-ending configuration
# is. That is the whole point of the exercise -- a copy that a checkout's `core.autocrlf` rewrote
# is not the tree that was reviewed, tested and tagged, and its hashes would differ per machine.
# For the same reason a CR byte, a symbolic link and a submodule are refused: the first means a
# file git treated as binary (a text import never is), and neither of the other two survives being
# copied into another repository as the bytes it names.
#
# sync also refuses what it cannot copy correctly: a REF that is not a tag or a full commit SHA, a
# REPO that is not the root of its own repository, a ref whose tree is not core-cpp's, and a
# MODULES list that leaves out a module the build enters unconditionally. The first three are one
# mistake seen from three sides -- running sync with a VENDORED COPY's own script, where REPO
# defaults to the copy's directory and git reads the CONSUMER's repository instead.
#
# check needs no git at all, because a consumer runs it as a test in its own CI, where core-cpp is
# a directory of files and nothing else. It re-hashes every file the MANIFEST lists and refuses a
# hash mismatch, a file that is missing and a file the manifest does not list -- so a local edit,
# the one thing a vendored copy may never carry, fails the consumer's own test suite. It also
# refuses a manifest that says nothing: an emptied copy beside an emptied manifest would otherwise
# have no file to disagree about and would pass.
#
# sync assembles the whole new copy in <DEST>.core-cpp-vendor-new and puts it in place with two
# directory renames, through <DEST>.core-cpp-vendor-old: see cmake/CoreCppVendorReplace.cmake for
# why, and for what happens when one of those renames fails. DEST ends up holding the previous copy
# or the new one, never a mixture of the two.
#
# Every refusal that applies is reported, not only the first.
# tests/cmake/check-vendor-selftest.cmake proves each of them by name.

cmake_minimum_required(VERSION 3.25)

# --- the file set (Part I §5) --------------------------------------------------
#
# The files taken by name. Everything else is selected by directory: cmake/**, the base module
# (every file directly in src/core/) and src/core/<module>/** for each module asked for.
#
# The spec says "src/core/*.hpp|cpp" for base; the whole directory is taken instead, because
# src/core/CMakeLists.txt is the base module's own CMakeLists and src/core/Config.hpp.in is what
# the top-level configure_file() generates core/Config.hpp from. Without those two the copy does
# not configure, so the narrower reading would ship a tree that cannot be built.
set(CORE_CPP_VENDOR_NAMED_FILES
    CMakeLists.txt
    LICENSE
    NOTICE
    README.md
    CHANGELOG.md
    .clang-format
    .clang-tidy)

# The manifest's name inside the copy, and the two SIBLINGS of DEST a sync uses: it assembles the
# new copy in one and moves the previous copy into the other, so that putting the new copy in place
# is two directory renames with a restore between them rather than a file-by-file move into an
# emptied DEST. See cmake/CoreCppVendorReplace.cmake, which does that part.
#
# Siblings rather than children of DEST, because DEST itself is what gets renamed. They are on the
# same filesystem as DEST for the same reason, and they are named after it so that one surviving a
# crash says which copy it belongs to. Neither outlives a run of this script.
set(CORE_CPP_VENDOR_MANIFEST "MANIFEST")
set(CORE_CPP_VENDOR_NEW_SUFFIX ".core-cpp-vendor-new")
set(CORE_CPP_VENDOR_OLD_SUFFIX ".core-cpp-vendor-old")

# This script's own directory, captured before any include() moves CMAKE_CURRENT_LIST_DIR.
set(CORE_CPP_VENDOR_SCRIPT_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The directory holding the copy this run is assembling, empty until MODE=sync creates one.
set(CORE_CPP_VENDOR_STAGING_DIR "")

## @brief Removes the directory of the copy this run was assembling, if it has one.
##
## Every message(FATAL_ERROR) of MODE=sync is preceded by a call to this, bar the one that reports
## that a restore failed and says where both trees are -- that one must keep what it names. A
## refusal that left the directory behind would leave a `<DEST>.core-cpp-vendor-new` beside a copy
## the run never replaced, and the consumer would find it in their next `git status`. "A refusal
## leaves the previous copy exactly as it was, and nothing beside it" is a promise
## docs/vendoring.md makes, so it is this macro's job to keep it on every path, not only on the
## ones that happen to remember.
macro(core_cpp_vendor_unstage)
    if(CORE_CPP_VENDOR_STAGING_DIR)
        file(REMOVE_RECURSE "${CORE_CPP_VENDOR_STAGING_DIR}")
    endif()
endmacro()

## @brief Sets @p outVar to ON when @p path belongs to the file set for @p modules.
##
## @param path A repository-relative path, as git ls-tree reports it.
## @param modules The module names the copy carries; `base` is src/core/ itself.
## @param outVar Receives ON or OFF.
function(core_cpp_vendor_selects path modules outVar)
    set(selected OFF)
    if(path IN_LIST CORE_CPP_VENDOR_NAMED_FILES)
        set(selected ON)
    elseif(path MATCHES "^cmake/")
        set(selected ON)
    elseif(path MATCHES "^src/core/[^/]+$")
        if("base" IN_LIST modules)
            set(selected ON)
        endif()
    elseif(path MATCHES "^src/core/([^/]+)/")
        if("${CMAKE_MATCH_1}" IN_LIST modules)
            set(selected ON)
        endif()
    endif()
    set(${outVar} ${selected} PARENT_SCOPE)
endfunction()

## @brief Runs git in the repository @p repo and sets @p outVar to its standard output.
##
## Stops the script when git fails, saying what it was doing. The two -c options are on every
## invocation, including the ones that only read metadata: one place to state them is one place
## for them to be wrong.
##
## @param outVar Receives the output, with trailing newlines removed.
## @param repo The repository to run in.
## @param what What the call was for, for the message when it fails.
## @param ARGN The git arguments.
function(core_cpp_vendor_git outVar repo what)
    execute_process(
        COMMAND "${CORE_CPP_VENDOR_GIT}" -c core.autocrlf=false -c core.eol=lf -C "${repo}" ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        core_cpp_vendor_unstage()
        message(FATAL_ERROR "core-cpp-vendor: ${what} failed (git exited ${rc}): ${out}${err}")
    endif()
    string(REGEX REPLACE "[\r\n]+$" "" out "${out}")
    set(${outVar} "${out}" PARENT_SCOPE)
endfunction()

## @brief Sets @p outVar to whether @p left and @p right name the same directory.
##
## Both are resolved through their symbolic links first, and on a Windows host the comparison is
## case-insensitive: git and CMake spell a drive letter differently often enough to matter, and a
## path comparison that says "different" there would refuse a correct invocation.
function(core_cpp_vendor_same_directory left right outVar)
    get_filename_component(left "${left}" REALPATH)
    get_filename_component(right "${right}" REALPATH)
    set(same OFF)
    if(left STREQUAL right)
        set(same ON)
    elseif(CMAKE_HOST_WIN32)
        string(TOLOWER "${left}" left)
        string(TOLOWER "${right}" right)
        if(left STREQUAL right)
            set(same ON)
        endif()
    endif()
    set(${outVar} ${same} PARENT_SCOPE)
endfunction()

## @brief Lists the files of @p dir, relative to it, with @p ARGN excluded.
##
## file(GLOB_RECURSE) matches a leading dot, so .clang-format and .clang-tidy are found too.
function(core_cpp_vendor_list_files dir outVar)
    file(GLOB_RECURSE found LIST_DIRECTORIES false RELATIVE "${dir}" "${dir}/*")
    foreach(excluded IN LISTS ARGN)
        list(REMOVE_ITEM found "${excluded}")
    endforeach()
    list(SORT found)
    set(${outVar} "${found}" PARENT_SCOPE)
endfunction()

# --- arguments -----------------------------------------------------------------

if(NOT DEFINED MODE OR NOT MODE MATCHES "^(sync|check)$")
    message(FATAL_ERROR
        "core-cpp-vendor: MODE must be sync or check, not '${MODE}'.\n"
        "  cmake -DMODE=sync -DREF=<tag> -DDEST=<dir> [-DREPO=<url or path>] [\"-DMODULES=<a;b>\"] "
        "-P <core-cpp checkout>/cmake/CoreCppVendor.cmake\n"
        "  cmake -DMODE=check -DDEST=<dir> -P <dir>/cmake/CoreCppVendor.cmake")
endif()
if(NOT DEFINED DEST OR DEST STREQUAL "")
    message(FATAL_ERROR "core-cpp-vendor: DEST is not set; it names the directory the copy lives in.")
endif()
get_filename_component(DEST "${DEST}" ABSOLUTE)

# --- MODE=check ----------------------------------------------------------------
#
# No git, no network, no REF: the copy and its manifest are all this needs.
if(MODE STREQUAL "check")
    set(manifestPath "${DEST}/${CORE_CPP_VENDOR_MANIFEST}")
    if(NOT EXISTS "${manifestPath}")
        message(FATAL_ERROR
            "core-cpp-vendor: ${manifestPath} does not exist, so ${DEST} is not a core-cpp vendored "
            "copy. Re-create it with MODE=sync.")
    endif()

    file(STRINGS "${manifestPath}" manifestLines)
    set(refusals "")
    set(listed "")
    set(statedCount "")
    set(statedRepository "")
    set(statedRef "")
    set(commit "")
    foreach(line IN LISTS manifestLines)
        if(line MATCHES "^# files (.+)$")
            set(statedCount "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^# commit (.+)$")
            set(commit "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^# repository (.+)$")
            set(statedRepository "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^# ref (.+)$")
            set(statedRef "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^#")
            continue()
        elseif(line MATCHES "^([0-9a-f]+)  (.+)$")
            set(wanted "${CMAKE_MATCH_1}")
            set(path "${CMAKE_MATCH_2}")
            list(APPEND listed "${path}")
            if(NOT EXISTS "${DEST}/${path}")
                string(APPEND refusals "\n  ${path}: is missing")
                continue()
            endif()
            file(SHA256 "${DEST}/${path}" got)
            if(NOT got STREQUAL wanted)
                string(APPEND refusals
                    "\n  ${path}: differs from the manifest (expected ${wanted}, found ${got})")
            endif()
        elseif(NOT line STREQUAL "")
            string(APPEND refusals "\n  ${manifestPath}: line '${line}' is neither a header nor '<sha256>  <path>'")
        endif()
    endforeach()

    core_cpp_vendor_list_files("${DEST}" present "${CORE_CPP_VENDOR_MANIFEST}")
    foreach(path IN LISTS present)
        if(NOT path IN_LIST listed)
            string(APPEND refusals "\n  ${path}: is not in the manifest")
        endif()
    endforeach()

    # The header is required, and required to say something. Without this, a manifest that is gone
    # or truncated beside a copy that is gone has nothing left to disagree about: no file is listed,
    # so no hash is compared, none is missing and none is unlisted, and the tool would report "0
    # file(s)" and exit 0 -- passing the consumer's gate over an empty directory, which is the one
    # state it exists to catch.
    string(LENGTH "${commit}" commitLength)
    if(statedRepository STREQUAL "")
        string(APPEND refusals
            "\n  ${CORE_CPP_VENDOR_MANIFEST}: has no '# repository <url or path>' header line")
    endif()
    if(statedRef STREQUAL "")
        string(APPEND refusals "\n  ${CORE_CPP_VENDOR_MANIFEST}: has no '# ref <tag or SHA>' header line")
    endif()
    if(NOT commitLength EQUAL 40 OR NOT commit MATCHES "^[0-9a-f]+$")
        string(APPEND refusals
            "\n  ${CORE_CPP_VENDOR_MANIFEST}: has no '# commit <40-character SHA>' header line "
            "(it says '${commit}')")
    endif()

    list(LENGTH listed listedCount)
    if(statedCount STREQUAL "")
        string(APPEND refusals "\n  ${CORE_CPP_VENDOR_MANIFEST}: has no '# files <count>' header line")
    elseif(NOT statedCount MATCHES "^[0-9]+$")
        string(APPEND refusals
            "\n  ${CORE_CPP_VENDOR_MANIFEST}: says '# files ${statedCount}', which is not a count")
    elseif(statedCount EQUAL 0)
        string(APPEND refusals
            "\n  ${CORE_CPP_VENDOR_MANIFEST}: says '# files 0', and a core-cpp copy is never empty")
    elseif(NOT statedCount STREQUAL "${listedCount}")
        string(APPEND refusals
            "\n  ${CORE_CPP_VENDOR_MANIFEST}: says '# files ${statedCount}' but lists ${listedCount}")
    endif()

    if(refusals)
        message(FATAL_ERROR
            "core-cpp-vendor: ${DEST} is not the verbatim copy its manifest describes:${refusals}\n"
            "A vendored copy carries no local change: fix it in core-cpp, release, and re-run "
            "MODE=sync. To restore this copy, re-run MODE=sync with the manifest's ref.")
    endif()
    message(STATUS
        "core-cpp-vendor: ${DEST} matches its manifest: ${listedCount} file(s), commit ${commit}")
    return()
endif()

# --- MODE=sync -----------------------------------------------------------------
#
# From the point the staging directory exists, every message(FATAL_ERROR) below -- and the one in
# core_cpp_vendor_git() -- is preceded by core_cpp_vendor_unstage(). See its comment for why.

if(NOT DEFINED REF OR REF STREQUAL "")
    message(FATAL_ERROR
        "core-cpp-vendor: REF is not set; it names the tag or full commit SHA to copy. "
        "A branch is not a valid source: it changes under the consumer without a commit of its own.")
endif()

find_program(CORE_CPP_VENDOR_GIT NAMES git)
if(NOT CORE_CPP_VENDOR_GIT)
    message(FATAL_ERROR "core-cpp-vendor: MODE=sync needs git, which is not on PATH. (MODE=check does not.)")
endif()

# REPO defaults to the repository this script is part of, which is what a core-cpp checkout's own
# `cmake -DMODE=sync ...` means. Anything that is not a directory is a remote and is cloned, bare
# and once, into the staging area; a local path (a checkout or a bare repository) is read in place.
if(NOT DEFINED REPO OR REPO STREQUAL "")
    get_filename_component(REPO "${CORE_CPP_VENDOR_SCRIPT_DIR}/.." ABSOLUTE)
elseif(IS_DIRECTORY "${REPO}")
    # One spelling in the messages, in the comparison below and in the manifest's own header.
    get_filename_component(REPO "${REPO}" ABSOLUTE)
endif()

# A local REPO must be the ROOT of its own repository, because `git -C` ascends out of a directory
# that is not one. This is the guard against the one mis-invocation that is otherwise silent and
# destructive: running sync with a VENDORED COPY's own script. REPO then defaults to the copy's
# directory, git reads the repository that CONTAINS the copy -- the consumer's -- and the run would
# empty the copy and refill it from whatever of the consumer's tree matches the file set, leaving a
# MANIFEST that MODE=check happily accepts. The tree check further down is the second half of it.
#
# Nothing is written before this: DEST is not even looked at yet.
if(IS_DIRECTORY "${REPO}")
    execute_process(
        COMMAND "${CORE_CPP_VENDOR_GIT}" -c core.autocrlf=false -c core.eol=lf -C "${REPO}"
                rev-parse --is-bare-repository
        RESULT_VARIABLE rc OUTPUT_VARIABLE isBare ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "core-cpp-vendor: REPO '${REPO}' is in no git repository (git exited ${rc}): ${err}"
            "MODE=sync reads git blobs, so REPO is a core-cpp checkout, a bare repository or a URL.")
    endif()
    string(STRIP "${isBare}" isBare)
    if(NOT isBare STREQUAL "true")
        core_cpp_vendor_git(topLevel "${REPO}" "finding the root of the repository at ${REPO}"
                            rev-parse --show-toplevel)
        core_cpp_vendor_same_directory("${REPO}" "${topLevel}" sameRoot)
        if(NOT sameRoot)
            message(FATAL_ERROR
                "core-cpp-vendor: REPO is '${REPO}', but that directory is not a repository root: "
                "the repository it belongs to has its root at '${topLevel}', and sync would copy "
                "THAT repository's files.\n"
                "This is what running sync with a VENDORED COPY's own script does -- REPO defaults "
                "to the script's parent directory, and git then reads the repository containing the "
                "copy. A vendored copy's own script is for MODE=check. To re-vendor, run sync from "
                "a core-cpp checkout, or pass -DREPO=<core-cpp checkout, bare repository or URL>.")
        endif()
    endif()
endif()

set(stagingDir "${DEST}${CORE_CPP_VENDOR_NEW_SUFFIX}")
set(backupDir "${DEST}${CORE_CPP_VENDOR_OLD_SUFFIX}")
file(REMOVE_RECURSE "${stagingDir}")

# Before anything is written: three things about DEST and its two siblings, all of them visible in
# a second, and all of them checked here rather than minutes later after a clone and a tree walk.
#
# DEST is not a symbolic link (core-cpp#25). EXISTS and IS_DIRECTORY resolve through one, so a link
# to a directory would pass every guard below as the directory it names, and then the replacement's
# two renames would move the LINK aside and put a real directory where it was: the consumer's link
# replaced, its target left holding the old copy, and success reported. A link to a file would be
# refused below as a file, which is right but says the wrong thing. Refused by name, before either.
if(IS_SYMLINK "${DEST}")
    file(READ_SYMLINK "${DEST}" linkTarget)
    message(FATAL_ERROR
        "core-cpp-vendor: ${DEST} is a symlink, and a sync replaces DEST itself rather than writing "
        "through it, so the link would be lost. Nothing has been changed. Point DEST at the "
        "directory it names (${linkTarget}).")
endif()

# DEST is a directory or it does not exist. A regular file there is someone's, and the replacement
# would rename it aside and delete it after a successful swap, reporting success: the occupant
# check below cannot see it, because a file holds no files.
if(EXISTS "${DEST}" AND NOT IS_DIRECTORY "${DEST}")
    message(FATAL_ERROR
        "core-cpp-vendor: ${DEST} is a file, not a directory, so it is not a core-cpp vendored "
        "copy and syncing would destroy it. Delete it, or point DEST at a directory.")
endif()

# A leftover backup is a previous run that failed between the two renames of the replacement, so it
# holds the only copy of what was there. It is the one thing a sync must not delete to make room,
# and the replacement refuses over it -- but it would refuse after every blob had been written, for
# a condition that is true right now.
if(EXISTS "${backupDir}")
    message(FATAL_ERROR
        "core-cpp-vendor: ${backupDir} already exists. That is where a run moves the previous copy "
        "aside, so a previous run failed and never put it back. Nothing has been changed now. Move "
        "it back to ${DEST}, or delete it once you are sure ${DEST} is the copy you want, and run "
        "the sync again.")
endif()

# An existing DEST is only ever replaced when it is a copy of ours, and the manifest is what says
# so. Nothing else may live in a vendored copy anyway -- MODE=check refuses an unlisted file -- so
# a directory with files and no manifest is someone else's work.
if(EXISTS "${DEST}" AND NOT EXISTS "${DEST}/${CORE_CPP_VENDOR_MANIFEST}")
    core_cpp_vendor_list_files("${DEST}" occupants)
    if(occupants)
        list(LENGTH occupants occupantCount)
        message(FATAL_ERROR
            "core-cpp-vendor: ${DEST} holds ${occupantCount} file(s) and no "
            "${CORE_CPP_VENDOR_MANIFEST}, so it is not a core-cpp vendored copy and syncing would "
            "delete someone's work. Empty it, or point DEST at a new directory.")
    endif()
endif()

file(MAKE_DIRECTORY "${stagingDir}")
set(CORE_CPP_VENDOR_STAGING_DIR "${stagingDir}")

set(repoPath "${REPO}")
set(clonePath "")
if(NOT IS_DIRECTORY "${REPO}")
    set(clonePath "${stagingDir}/repo.git")
    message(STATUS "core-cpp-vendor: cloning ${REPO} (a local path is read in place instead)")
    execute_process(
        COMMAND "${CORE_CPP_VENDOR_GIT}" -c core.autocrlf=false -c core.eol=lf
                clone --quiet --bare "${REPO}" "${clonePath}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        core_cpp_vendor_unstage()
        message(FATAL_ERROR "core-cpp-vendor: cloning ${REPO} failed (git exited ${rc}): ${out}${err}")
    endif()
    set(repoPath "${clonePath}")
endif()

# REF is a tag or a full commit SHA, and the tool enforces what its own message and
# docs/vendoring.md both promise. A branch is not a valid source, and neither is HEAD or a short
# SHA: the copy would carry `# ref master` in its manifest, and the recovery MODE=check recommends
# -- "re-run sync with the manifest's ref" -- would then restore whatever that branch points at
# today rather than the tree the manifest pins.
#
# CMake's regular expressions have no bounded repetition, so the length is its own test.
string(LENGTH "${REF}" refLength)
if(refLength EQUAL 40 AND REF MATCHES "^[0-9a-f]+$")
    core_cpp_vendor_git(commit "${repoPath}" "resolving the commit ${REF} in ${REPO}"
                        rev-parse --verify "${REF}^{commit}")
else()
    execute_process(
        COMMAND "${CORE_CPP_VENDOR_GIT}" -c core.autocrlf=false -c core.eol=lf -C "${repoPath}"
                rev-parse --verify --quiet "refs/tags/${REF}^{commit}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE commit ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        core_cpp_vendor_unstage()
        message(FATAL_ERROR
            "core-cpp-vendor: REF '${REF}' is neither a full 40-character commit SHA nor a tag of "
            "${REPO}. A branch, HEAD or a short SHA is not a valid source: it names a different "
            "tree from one day to the next, and the copy's manifest would record a ref that cannot "
            "restore it.")
    endif()
    string(REGEX REPLACE "[\r\n]+$" "" commit "${commit}")
endif()

# One `ls-tree -r -z` for the whole tree: NUL-separated, so no path needs quoting, and
# file(STRINGS) splits a NUL-separated file into exactly one entry per record.
set(lsTreeFile "${stagingDir}/ls-tree")
execute_process(
    COMMAND "${CORE_CPP_VENDOR_GIT}" -c core.autocrlf=false -c core.eol=lf -C "${repoPath}"
            ls-tree -r -z "${commit}"
    RESULT_VARIABLE rc OUTPUT_FILE "${lsTreeFile}" ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    core_cpp_vendor_unstage()
    message(FATAL_ERROR "core-cpp-vendor: listing the tree of ${commit} failed (git exited ${rc}): ${err}")
endif()
file(STRINGS "${lsTreeFile}" entries)

# Pass one: what the ref has. A module is a directory under src/core/; `base` is src/core/ itself,
# and cmake/CoreCppModules.cmake is the table that says which of them the build enters.
set(availableModules "base")
set(moduleTableBlob "")
set(hasCoreDirectory OFF)
foreach(entry IN LISTS entries)
    if(entry MATCHES "^[0-7]+ [a-z]+ ([0-9a-f]+)\tcmake/CoreCppModules\\.cmake$")
        set(moduleTableBlob "${CMAKE_MATCH_1}")
    elseif(entry MATCHES "^[0-7]+ [a-z]+ [0-9a-f]+\t(src/core/([^/]+)/)")
        set(hasCoreDirectory ON)
        if(NOT "${CMAKE_MATCH_2}" IN_LIST availableModules)
            list(APPEND availableModules "${CMAKE_MATCH_2}")
        endif()
    elseif(entry MATCHES "^[0-7]+ [a-z]+ [0-9a-f]+\tsrc/core/")
        set(hasCoreDirectory ON)
    endif()
endforeach()

# The second half of the guard against syncing the wrong repository: whatever REPO resolved to, the
# ref's tree has to be core-cpp's. A consumer's own repository carries neither of these, so a sync
# aimed at one stops here instead of filling the copy with the consumer's CMakeLists.txt.
set(notCoreCpp "")
if(NOT moduleTableBlob)
    list(APPEND notCoreCpp "cmake/CoreCppModules.cmake")
endif()
if(NOT hasCoreDirectory)
    list(APPEND notCoreCpp "src/core/")
endif()
if(notCoreCpp)
    core_cpp_vendor_unstage()
    list(JOIN notCoreCpp " and no " absent)
    message(FATAL_ERROR
        "core-cpp-vendor: the tree of ${REF} (${commit}) in ${REPO} is not a core-cpp tree -- it "
        "has no ${absent}. REPO names the repository to copy core-cpp OUT of; it is not the "
        "consumer's own repository, and not the directory a vendored copy lives in.")
endif()

if(NOT DEFINED MODULES OR MODULES STREQUAL "")
    set(MODULES ${availableModules})
endif()
set(unknownModules "")
foreach(module IN LISTS MODULES)
    if(NOT module IN_LIST availableModules)
        list(APPEND unknownModules "${module}")
    endif()
endforeach()
if(unknownModules)
    core_cpp_vendor_unstage()
    list(JOIN availableModules ", " known)
    list(JOIN unknownModules ", " unknown)
    message(FATAL_ERROR
        "core-cpp-vendor: MODULES names ${unknown}, which ${REF} has no module for "
        "(it has: ${known}).")
endif()

# A module whose row names no WHEN option is one core_cpp_add_modules() enters unconditionally, so
# a copy without its directory does not configure -- and CMake's own message for that is the
# generic "source directory does not exist", naming a path rather than the argument that dropped
# it. The list is read from the ref's own table, so it cannot drift from the hand-written MODULES
# strings in build.yml, docs/vendoring.md and tests/consumer-vendored/CMakeLists.txt.
core_cpp_vendor_git(moduleTable "${repoPath}" "reading cmake/CoreCppModules.cmake of ${commit}"
                    cat-file blob "${moduleTableBlob}")
string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" moduleTable "${moduleTable}")
string(REGEX MATCHALL "core_cpp_module\\([^)]*\\)" moduleRows "${moduleTable}")
set(unconditionalModules "")
foreach(row IN LISTS moduleRows)
    if(NOT row MATCHES "NAME[ \t\r\n]+([A-Za-z0-9_]+)")
        continue()
    endif()
    set(rowName "${CMAKE_MATCH_1}")
    if(row MATCHES "[ \t\r\n]WHEN[ \t\r\n]")
        continue()
    endif()
    list(APPEND unconditionalModules "${rowName}")
endforeach()
set(omittedModules "")
foreach(module IN LISTS unconditionalModules)
    if(NOT module IN_LIST MODULES)
        list(APPEND omittedModules "${module}")
    endif()
endforeach()
if(omittedModules)
    core_cpp_vendor_unstage()
    list(JOIN omittedModules ", " omitted)
    message(FATAL_ERROR
        "core-cpp-vendor: MODULES omits ${omitted}, which ${REF} builds unconditionally (the row "
        "in cmake/CoreCppModules.cmake names no WHEN option that could switch it off). The copy "
        "would not configure: core_cpp_add_modules() enters that module's directory, and the copy "
        "would not have one.")
endif()

# Pass two: copy every selected blob into the staging tree, and collect every reason not to.
set(refusals "")
set(copied "")
foreach(entry IN LISTS entries)
    if(NOT entry MATCHES "^([0-7]+) ([a-z]+) ([0-9a-f]+)\t(.+)$")
        string(APPEND refusals "\n  ${entry}: git ls-tree said this, which is not '<mode> <type> <sha><TAB><path>'")
        continue()
    endif()
    set(mode "${CMAKE_MATCH_1}")
    set(type "${CMAKE_MATCH_2}")
    set(blob "${CMAKE_MATCH_3}")
    set(path "${CMAKE_MATCH_4}")

    core_cpp_vendor_selects("${path}" "${MODULES}" selected)
    if(NOT selected)
        continue()
    endif()

    # A gitlink is a commit of another repository, recorded by SHA and nothing else; copying it
    # would copy a name for content that is not here.
    if(mode STREQUAL "160000" OR type STREQUAL "commit")
        string(APPEND refusals "\n  ${path}: is a submodule (gitlink ${blob}); a vendored copy carries files only")
        continue()
    endif()
    # A symlink's blob is its target path. Written as a file it is a file of a path; restored as a
    # link it depends on a filesystem and a checkout setting the consumer may not have.
    if(mode STREQUAL "120000")
        string(APPEND refusals "\n  ${path}: is a symbolic link; a vendored copy carries regular files only")
        continue()
    endif()

    set(target "${stagingDir}/${path}")
    get_filename_component(targetDir "${target}" DIRECTORY)
    file(MAKE_DIRECTORY "${targetDir}")
    execute_process(
        COMMAND "${CORE_CPP_VENDOR_GIT}" -c core.autocrlf=false -c core.eol=lf -C "${repoPath}"
                cat-file blob "${blob}"
        RESULT_VARIABLE rc OUTPUT_FILE "${target}" ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        string(APPEND refusals "\n  ${path}: reading blob ${blob} failed (git exited ${rc}): ${err}")
        continue()
    endif()

    # The bytes are the commit's, so a CR here is a CR the repository itself records. .gitattributes
    # normalises text to LF, so such a file is one git treated as binary -- which a text import,
    # and this file set, never is.
    #
    # Read as hex, because a plain file(READ) opens the file in text mode on Windows and hands back
    # a string a CRLF has already been taken out of. "0d" also occurs straddling two bytes (0x40
    # 0xd9 reads "40d9"), so a hit only counts at an even offset; the search walks past the odd
    # ones rather than splitting the whole file into a list of one element per byte.
    file(READ "${target}" hexContent HEX)
    set(scanned 0)
    set(carriageReturnAt -1)
    while(TRUE)
        string(FIND "${hexContent}" "0d" at)
        if(at EQUAL -1)
            break()
        endif()
        math(EXPR absolute "${scanned} + ${at}")
        math(EXPR straddling "${absolute} % 2")
        if(straddling EQUAL 0)
            math(EXPR carriageReturnAt "${absolute} / 2")
            break()
        endif()
        math(EXPR skip "${at} + 1")
        string(SUBSTRING "${hexContent}" ${skip} -1 hexContent)
        math(EXPR scanned "${scanned} + ${skip}")
    endwhile()
    if(NOT carriageReturnAt EQUAL -1)
        string(APPEND refusals "\n  ${path}: contains a CR byte (at offset ${carriageReturnAt})")
        continue()
    endif()
    list(APPEND copied "${path}")
endforeach()

# There is no "the ref selected no file" refusal here, because there is no way to reach one: the
# tree guard above has already refused a ref without cmake/CoreCppModules.cmake, and that file is
# in the set for every MODULES list, so `copied` is never empty by the time this is read.
if(refusals)
    core_cpp_vendor_unstage()
    message(FATAL_ERROR
        "core-cpp-vendor: refusing to vendor ${REF} from ${REPO}:${refusals}\n"
        "${DEST} is unchanged.")
endif()

# The copy is whole and legal; what is left is to finish it and put it in place. A refusal above
# left the previous copy exactly as it was.
#
# The two things this run made for itself, and that the copy must not carry, go first: git's
# ls-tree output, and the bare clone of a remote REPO.
file(REMOVE "${lsTreeFile}")
if(clonePath)
    file(REMOVE_RECURSE "${clonePath}")
endif()

# The manifest goes into the STAGED copy, before the swap, hashed from the files as they lie there.
# That is what makes the swap the last thing that happens: the staged tree is already a complete
# vendored copy, manifest included, so whichever of the two directories exists when the process
# stops is one that passes MODE=check. Written afterwards, as it was, there was a window in which a
# kill stranded a manifest-less copy with the previous one already gone -- a copy that fails its
# own check, and that the next sync then refuses to overwrite.
#
# The bytes do not depend on where it is written: nothing in the manifest names DEST, and MANIFEST
# is not among the files it lists, so it never hashes itself.
#
# Sorted by path, so the same ref always lists the same files in the same order.
#
# It is written with LF endings whatever the host, because the consumer COMMITS this file: a
# file(WRITE) opens the file in text mode, so a sync on Windows would write CRLF and two correct
# syncs of the same tag from two machines would differ in every line. file(CONFIGURE) is the one
# write in script mode that takes NEWLINE_STYLE. Its @ substitution is why the content's own '@'
# characters -- `git@github.com:...` is an ordinary REPO -- are routed through a variable that
# holds one. The FILES are unaffected either way: each is written from git's blob through a
# process's stdout, which is never translated.
list(SORT copied)
list(LENGTH copied fileCount)
set(manifest "# core-cpp vendored copy -- verify it with:\n")
string(APPEND manifest "#   cmake -DMODE=check -DDEST=<this directory> -P <this directory>/cmake/CoreCppVendor.cmake\n")
string(APPEND manifest "# repository ${REPO}\n")
string(APPEND manifest "# ref ${REF}\n")
string(APPEND manifest "# commit ${commit}\n")
string(APPEND manifest "# modules ${MODULES}\n")
string(APPEND manifest "# files ${fileCount}\n")
foreach(path IN LISTS copied)
    file(SHA256 "${stagingDir}/${path}" hash)
    string(APPEND manifest "${hash}  ${path}\n")
endforeach()
set(CORE_CPP_VENDOR_AT "@")
string(REPLACE "@" "@CORE_CPP_VENDOR_AT@" manifest "${manifest}")
file(CONFIGURE OUTPUT "${stagingDir}/${CORE_CPP_VENDOR_MANIFEST}" CONTENT "${manifest}"
     @ONLY NEWLINE_STYLE UNIX)

# The staged copy is complete. The replacement is the one part of a sync that can damage a copy
# that was already there, so it is a module of its own, and a test calls it. Two directory renames
# with a restore between them: DEST ends up holding the previous copy or this one, and both of
# those are whole copies that pass their own check.
include("${CORE_CPP_VENDOR_SCRIPT_DIR}/CoreCppVendorReplace.cmake")
core_cpp_vendor_replace("${stagingDir}" "${DEST}" "${backupDir}" replaceState replaceMessage)
if(NOT replaceState STREQUAL "OK")
    # FAILED-KEEP-BOTH is the one refusal that keeps what it names: its message is a pair of paths
    # for a human to choose between, and unstaging would delete one of them.
    if(NOT replaceState STREQUAL "FAILED-KEEP-BOTH")
        core_cpp_vendor_unstage()
    endif()
    message(FATAL_ERROR "core-cpp-vendor: ${replaceMessage}")
endif()
set(CORE_CPP_VENDOR_STAGING_DIR "")

message(STATUS
    "core-cpp-vendor: ${REF} (${commit}) copied into ${DEST}: ${fileCount} file(s), "
    "modules ${MODULES}")
