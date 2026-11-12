# The configurations that cannot be a production build, refused before CPM and FetchContent fetch their
# dependencies (a refusal after the fetch makes a misconfigured configure pay for all of it first; vcpkg
# installs its ports earlier still, in project()).
# Included from the top-level CMakeLists.txt only when CONTOUR_PRODUCTION_BUILD is ON, once the variables
# it reads are decided. The flags themselves are applied by ProductionBuild.cmake.

if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON needs CMAKE_BUILD_TYPE=Release, "
        "but it is '${CMAKE_BUILD_TYPE}': the packages are Release builds, and so are the binaries CI tests.")
endif()
if(NOT CONTOUR_SANITIZE STREQUAL "OFF")
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON cannot be combined with CONTOUR_SANITIZE=${CONTOUR_SANITIZE}: "
        "a sanitized binary is not the one that is shipped.")
endif()
if(CONTOUR_CODE_COVERAGE_ENABLED)
    message(FATAL_ERROR "CONTOUR_PRODUCTION_BUILD=ON cannot be combined with CONTOUR_CODE_COVERAGE_ENABLED=${CONTOUR_CODE_COVERAGE_ENABLED} "
        "(CONTOUR_COVERAGE=${CONTOUR_COVERAGE}): an instrumented binary is not the one that is shipped.")
endif()
