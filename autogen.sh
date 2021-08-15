#! /bin/bash
set -ex

if [[ -x "$(command -v realpath)" ]]; then
  ROOTDIR="$(realpath $(dirname $0))"
else
  ROOTDIR="$(dirname $0)"
fi

BUILD_TYPE="${1:-Debug}"
WORKDIR="$(pwd)"

exec cmake "${ROOTDIR}" \
           -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
           -DCMAKE_CXX_FLAGS="-fdiagnostics-color=always" \
           -DCMAKE_EXPORT_COMPILE_COMMANDS="ON" \
           -DYAML_CPP_BUILD_CONTRIB="OFF" \
           -DYAML_CPP_BUILD_TOOLS="OFF" \
           -DLIBTERMINAL_LOG_RAW="ON" \
           -DLIBTERMINAL_LOG_TRACE="ON" \
           -DLIBTERMINAL_EXECUTION_PAR="OFF" \
           -DCONTOUR_COVERAGE="OFF" \
           -DCONTOUR_PERF_STATS="ON" \
           -DCONTOUR_BLUR_PLATFORM_KWIN="ON" \
           -GNinja

