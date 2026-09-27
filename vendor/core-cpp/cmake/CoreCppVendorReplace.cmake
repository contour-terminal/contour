# SPDX-License-Identifier: Apache-2.0
#
# The replacement phase of cmake/CoreCppVendor.cmake's MODE=sync, in a module of its own.
#
# It is here rather than inline for one reason: this is the only part of a sync that can damage a
# copy that was already there, and a function is callable from a test where a passage of a script
# is not. tests/cmake/check-vendor-selftest.cmake calls it with a new copy that does not exist,
# which is the portable way to make a rename fail, and then asserts that the copy it replaced is
# back and still passes its own MODE=check.
#
# Including this file defines core_cpp_vendor_replace() and does nothing else. It is a module, not
# a script, so it sets no policy of its own: whatever includes it has already said which it wants.

## @brief Puts @p newCopy in place of @p dest, recoverably.
##
## Two directory renames, rather than emptying @p dest and moving the new files in one by one:
##
##   1. @p dest -> @p backup      (skipped when @p dest does not exist)
##   2. @p newCopy -> @p dest
##   3. remove @p backup
##
## A failure at 1 leaves @p dest exactly as it was. A failure at 2 puts the previous copy back and
## reports -- on Windows a rename is not the single kernel call it is on a unix, and an open handle,
## a lock or a scanner can fail one. So @p dest ends up holding the old copy or the new one and
## never a mixture of the two, which is what a consumer's build depends on: the file-by-file move
## this replaced could leave it half of each.
##
## Only when the restore ALSO fails is there nothing left but to say where both trees are, and then
## neither of them is deleted. That is the one outcome no test reaches: to fail the restore, the
## rename that has just emptied @p dest would have to be unable to undo itself in the same
## directory, which nothing a test can set up will do.
##
## Each message is built with string(CONCAT) and not from several arguments to set(): set() would
## make a LIST of the fragments, and the caller would print them with a semicolon at every join.
##
## @param newCopy A COMPLETE vendored copy, its MANIFEST included. That is what lets a failure here
##        name both trees and mean it: whichever of the two a human adopts passes MODE=check.
##        cmake/CoreCppVendor.cmake writes the manifest into the staged tree before it calls this.
## @param dest The directory to replace. It need not exist, and its parent is created if it does
##        not. It must be a directory if it does exist; the caller refuses a file there.
## @param backup Where the previous copy is moved to. It must not exist; the caller refuses a
##        leftover one, early, because it holds the only copy of what a failed run moved aside.
## @param outState Receives OK, FAILED, or FAILED-KEEP-BOTH (both trees are still on disk).
## @param outMessage Receives what went wrong and what is where; empty when @p outState is OK.
function(core_cpp_vendor_replace newCopy dest backup outState outMessage)
    set(${outState} FAILED PARENT_SCOPE)

    get_filename_component(destParent "${dest}" DIRECTORY)
    file(MAKE_DIRECTORY "${destParent}")

    set(movedAside OFF)
    if(EXISTS "${dest}")
        file(RENAME "${dest}" "${backup}" RESULT renameResult)
        if(NOT renameResult STREQUAL "0")
            string(CONCAT message
                "could not move ${dest} aside to ${backup} (${renameResult}), so the copy was not "
                "replaced. ${dest} holds what it held before, and nothing of the new copy was "
                "written into it.")
            set(${outMessage} "${message}" PARENT_SCOPE)
            return()
        endif()
        set(movedAside ON)
    endif()

    file(RENAME "${newCopy}" "${dest}" RESULT renameResult)
    if(renameResult STREQUAL "0")
        if(movedAside)
            file(REMOVE_RECURSE "${backup}")
        endif()
        set(${outState} OK PARENT_SCOPE)
        set(${outMessage} "" PARENT_SCOPE)
        return()
    endif()

    if(NOT movedAside)
        string(CONCAT message
            "could not move the new copy from ${newCopy} into ${dest} (${renameResult}). ${dest} "
            "does not exist, and no previous copy was lost, because there was none.")
        set(${outMessage} "${message}" PARENT_SCOPE)
        return()
    endif()

    file(RENAME "${backup}" "${dest}" RESULT restoreResult)
    if(restoreResult STREQUAL "0")
        string(CONCAT message
            "could not move the new copy from ${newCopy} into ${dest} (${renameResult}). The "
            "previous copy has been put back, so ${dest} holds what it held before. A file of it "
            "may be open or locked by another process.")
        set(${outMessage} "${message}" PARENT_SCOPE)
        return()
    endif()

    set(${outState} FAILED-KEEP-BOTH PARENT_SCOPE)
    string(CONCAT message
        "could not move the new copy into ${dest} (${renameResult}), and could not put the "
        "previous copy back either (${restoreResult}). ${dest} does not exist at the moment, and "
        "NOTHING HAS BEEN DELETED:\n"
        "  the previous copy is at ${backup}\n"
        "  the new copy is at ${newCopy}\n"
        "Both are complete vendored copies, manifest included, so rename whichever of the two you "
        "want to ${dest} by hand and then delete the other; the one you keep passes MODE=check as "
        "it stands. Renaming the previous copy back is the state this sync promised, and leaves "
        "the sync to run again.")
    set(${outMessage} "${message}" PARENT_SCOPE)
endfunction()
