#! /bin/bash

set -ex

BUILD_DIR=${BUILD_DIR:-build}
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

prepare_build_ubuntu()
{
   cmake \
       --preset linux-release \
       -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
      ${EXTRA_CMAKE_FLAGS}
}

main_linux()
{
    local ID=`lsb_release --id | awk '{print $NF}'`

    case "${ID}" in
        Ubuntu)
            prepare_build_ubuntu
            ;;
        *)
            echo "No automated build configuration is available yet."
            ;;
    esac
}

main_darwin()
{
    echo "No automated build configuration is available yet."
}

main()
{
    case "$OSTYPE" in
        linux-gnu*)
            main_linux
            ;;
        darwin*)
            main_darwin
            ;;
        *)
            echo "OS not supported."
            ;;
    esac
}

main
