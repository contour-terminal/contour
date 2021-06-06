#! /bin/bash

set -e

prepare_build_ubuntu()
{
   mkdir -pv ${BUILD_DIR}
   cd ${BUILD_DIR}
   cmake .. \
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} \
      -DCMAKE_CXX_COMPILER="${CXX}" \
      -DYAML_BUILD_SHARED_LIBS=OFF \
      -DYAML_CPP_BUILD_CONTRIB=OFF \
      -DYAML_CPP_BUILD_TESTS=OFF \
      -DYAML_CPP_BUILD_TOOLS=OFF \
      -DYAML_CPP_INSTALL=OFF \
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
