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

EXTRA_CMAKE_FLAGS="$EXTRA_CMAKE_FLAGS -DLIBUNICODE_UCD_BASE_DIR=$ROOTDIR/_ucd"

case "$OSTYPE" in
    darwin*)
        EXTRA_CMAKE_FLAGS="${EXTRA_CMAKE_FLAGS} -DCMAKE_PREFIX_PATH=$(brew --prefix qt@6)"
        ;;
    *)
        ;;
esac

echo "EXTRA_CMAKE_FLAGS: ${EXTRA_CMAKE_FLAGS}"

exec cmake "${ROOTDIR}" \
           -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
           -DCMAKE_CXX_COMPILER="${CXX}" \
           -DLIBTERMINAL_BUILD_BENCH_HEADLESS=ON \
           -DPEDANTIC_COMPILER=ON \
           -DPEDANTIC_COMPILER_WERROR=ON \
           -DCMAKE_CXX_STANDARD=20 \
           -DCODE_SIGN_CERTIFICATE_ID="${CODE_SIGN_ID:-}" \
           ${EXTRA_CMAKE_FLAGS} \
           -B "${BUILD_DIR}" \
           -GNinja
