# SPDX-License-Identifier: Apache-2.0
#
# CPACK_POST_BUILD_SCRIPTS hook: sign the finished disk image, then notarize and staple it.
#
# The signature the install step applies covers contour.app only. CPack wraps that bundle
# into a .dmg afterwards, and the image is a separate signable object -- an unsigned one
# is what Gatekeeper reports as "damaged" or "from an unidentified developer" the moment
# the download's quarantine attribute is evaluated.
#
# Variables come from the CPack config (see the APPLE branch of src/contour/CMakeLists.txt).
# CPACK_PACKAGE_FILES is set by CPack itself and lists the artifacts just produced.

foreach(_package IN LISTS CPACK_PACKAGE_FILES)
    if(NOT _package MATCHES "\\.dmg$")
        continue()
    endif()

    message(STATUS "Signing ${_package}")
    execute_process(
        COMMAND "${CPACK_CONTOUR_PYTHON}" "${CPACK_CONTOUR_BUNDLE_SCRIPT}" sign "${_package}"
                --identity "${CPACK_CONTOUR_CODE_SIGN_IDENTITY}"
        COMMAND_ERROR_IS_FATAL ANY
    )

    if(CPACK_CONTOUR_NOTARIZE)
        message(STATUS "Notarizing ${_package}")
        execute_process(
            COMMAND "${CPACK_CONTOUR_PYTHON}" "${CPACK_CONTOUR_BUNDLE_SCRIPT}" notarize "${_package}"
                    --keychain-profile "${CPACK_CONTOUR_NOTARY_PROFILE}"
            COMMAND_ERROR_IS_FATAL ANY
        )

        # The check a user's machine performs, run here so a broken image cannot reach a
        # release: -t install is the assessment Gatekeeper applies to a downloaded archive.
        execute_process(
            COMMAND spctl --assess --verbose=4 --type install "${_package}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endforeach()
