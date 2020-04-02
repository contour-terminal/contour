#! /bin/bash
set -ex

BUILD_TYPE="${1:-Debug}"
WORKDIR="build"
if [[ "${BUILD_TYPE}" != "Debug" ]]; then
    WORKDIR="release"
fi

ROOTDIR="$(pwd)"
mkdir -p "${WORKDIR}"
cd "${WORKDIR}"

exec cmake "${ROOTDIR}" \
           -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
           -DCMAKE_CXX_FLAGS="-fdiagnostics-color=always" \
           -DOpenGL_GL_PREFERENCE="LEGACY" \
           -DYAML_CPP_BUILD_CONTRIB="OFF" \
           -DYAML_CPP_BUILD_TOOLS="OFF" \
           -GNinja

