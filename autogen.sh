#! /bin/sh
set -ex

# Minor helper to avoid repeating myself,
# This shortcut allows you to simply invoke ../../autogen.sh when being
# in directories like:
#     ./target/{Debug,RelWithDebInfo,Release}
if [ -e ../../autogen.sh ] && [ "x$1" == "x" ] &&
   [ -x "$(command -v realpath)" ] &&
   [ "$(basename $(realpath ${PWD}/..))" = "target" ]
then
    exec ../../autogen.sh $(basename $(realpath ${PWD}))
fi

if [ -x "$(command -v realpath)" ]; then
    ROOTDIR="$(realpath $(dirname $0))"
else
    ROOTDIR="$(dirname $0)"
fi

CXX=${CXX="g++"}
CXX_NAME=$(basename $CXX)

BUILD_TYPE="${1:-Debug}"
BUILD_DIR="${ROOTDIR}/target/$(uname -m)-$(uname -s)-${CXX_NAME}-${BUILD_TYPE}"

if test v$QTVER = v6; then
    EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DCONTOUR_BUILD_WITH_QT6=ON"
fi

case "$OSTYPE" in
    darwin*)
        if test v$QTVER = v6; then
            EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DQt6_DIR=$(brew --prefix qt6)/lib/cmake/Qt6"
        else
            EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DQt5_DIR=$(brew --prefix qt5)/lib/cmake/Qt5"
        fi
        ;;
    *)
        ;;
esac

if [ "${BUILD_TYPE}" != "Debug" ]; then
    EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DCMAKE_INSTALL_PREFIX=~/usr/opt/contour"
fi

echo "EXTRA_CMAKE_FLAGS: ${EXTRA_CMAKE_FLAGS}"

exec cmake "${ROOTDIR}" \
           -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
           -DCMAKE_CXX_COMPILER="${CXX}" \
           -DPEDANTIC_COMPILER=ON \
           -DPEDANTIC_COMPILER_WERROR=ON \
           ${EXTRA_CMAKE_FLAGS} \
           -B "${BUILD_DIR}" \
           -GNinja

