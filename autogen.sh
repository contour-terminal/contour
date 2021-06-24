#! /bin/bash
set -ex

ROOTDIR="$(dirname $0)"
BUILD_TYPE="${1:-Debug}"
WORKDIR="build"
if [[ "${BUILD_TYPE}" != "Debug" ]]; then
    WORKDIR="release"
fi

mkdir -p "${ROOTDIR}/${WORKDIR}"
cd "${ROOTDIR}/${WORKDIR}"

exec cmake "${ROOTDIR}" \
           -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
           -DCMAKE_CXX_FLAGS="-fdiagnostics-color=always" \
           -DCMAKE_EXPORT_COMPILE_COMMANDS="ON" \
           -DOpenGL_GL_PREFERENCE="GLVND" \
           -DYAML_CPP_BUILD_CONTRIB="OFF" \
           -DYAML_CPP_BUILD_TOOLS="OFF" \
           -DLIBTERMINAL_LOG_RAW="ON" \
           -DLIBTERMINAL_LOG_TRACE="ON" \
           -DLIBTERMINAL_EXECUTION_PAR="OFF" \
           -DCONTOUR_COVERAGE="OFF" \
           -DCONTOUR_PERF_STATS="OFF" \
           -DCONTOUR_BLUR_PLATFORM_KWIN="ON" \
           -GNinja

