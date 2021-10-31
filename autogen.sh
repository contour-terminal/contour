#! /bin/bash
set -ex

if [[ -x "$(command -v realpath)" ]]; then
  ROOTDIR="$(realpath $(dirname $0))"
else
  ROOTDIR="$(dirname $0)"
fi

BUILD_TYPE="${1:-Debug}"
BUILD_DIR="${ROOTDIR}/target/${BUILD_TYPE}"

EXTRA_CMAKE_FLAGS=""
case "$OSTYPE" in
    linux-gnu*)
        EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DCONTOUR_BLUR_PLATFORM_KWIN=ON"
        ;;
    darwin*)
        EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DQt5_DIR=$(brew --prefix qt5)/lib/cmake/Qt5"
        ;;
    *)
        ;;
esac

if [[ "${BUILD_TYPE}" != "Debug" ]]; then
    EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DCMAKE_INSTALL_PREFIX=~/usr/opt/contour"
fi

exec cmake "${ROOTDIR}" \
           -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
           ${EXTRA_CMAKE_FLAGS} \
           -B "${BUILD_DIR}" \
           -GNinja

