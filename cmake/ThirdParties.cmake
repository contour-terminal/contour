include(CPM)

set(3rdparty_catch2_version "10fb93cce8cb878fdfe8faa118b531258db96a26" CACHE STRING "catch2: commit hash")
set(3rdparty_fmt_version "d9fd695ac737f84f7de2d0a2aa346b25efb9afbf" CACHE STRING "fmt: commit hash")
set(3rdparty_libunicode_version "ce74dda96e0f7b2a1648f770a36f802ca6141f22" CACHE STRING "libunicode: commit hash")
set(3rdparty_mimalloc_version "v2.0.2" CACHE STRING "mimalloc: release tag")
set(3rdparty_range_v3_version "8f690283cc03146ad20514741cf69eafb325e974" CACHE STRING "range_v3: commit hash")
set(3rdparty_yaml_cpp_version "6308112e54fe41a3bc1ceeb2b807f0d09f15a1e9" CACHE STRING "yaml-cpp: commit hash")
set(3rdparty_termbenchpro_version "8f317315df288124f257ccc860d23b61703ddf47" CACHE STRING "termbench-pro: commit hash")

if(CONTOUR_TESTING OR CRISPY_TESTING OR LIBTERMINAL_TESTING)
  set(CATCH_BUILD_EXAMPLES OFF CACHE INTERNAL "")
  set(CATCH_BUILD_EXTRA_TESTS OFF CACHE INTERNAL "")
  set(CATCH_BUILD_TESTING OFF CACHE INTERNAL "")
  set(CATCH_ENABLE_WERROR OFF CACHE INTERNAL "")
  set(CATCH_INSTALL_DOCS OFF CACHE INTERNAL "")
  set(CATCH_INSTALL_HELPERS OFF CACHE INTERNAL "")

  CPMAddPackage(
    NAME catch2
    VERSION ${3rdparty_catch2_version}
    URL https://github.com/catchorg/Catch2/archive/${3rdparty_catch2_version}.zip
    URL_HASH SHA256=1545216747b1d0e4b9bd51843c61ac9a91b2d559977a3a93b9185ad8dbba6b82
    EXCLUDE_FROM_ALL YES
  )
endif()

CPMAddPackage(
  NAME fmt
  VERSION ${3rdparty_fmt_version}
  URL https://github.com/fmtlib/fmt/archive/${3rdparty_fmt_version}.zip
  URL_HASH SHA256=5c4e038b984161ddde7987472caf7e2ea864d49bede768038c6b93c9425ff218
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME range_v3
  VERSION ${3rdparty_range_v3_version}
  URL https://github.com/ericniebler/range-v3/archive/${3rdparty_range_v3_version}.zip
  URL_HASH SHA256=d866cb8f56aea491928af5cb1e7d2dbc4d46bd8a1a2a7b88194fb42b2c126e4b
  EXCLUDE_FROM_ALL YES
)

set(YAML_CPP_BUILD_CONTRIB OFF CACHE INTERNAL "")
set(YAML_CPP_BUILD_TESTS OFF CACHE INTERNAL "")
set(YAML_CPP_BUILD_TOOLS OFF CACHE INTERNAL "")
set(YAML_CPP_INSTALL OFF CACHE INTERNAL "")

CPMAddPackage(
  NAME yaml_cpp
  VERSION ${3rdparty_yaml_cpp_version}
  URL https://github.com/jbeder/yaml-cpp/archive/${3rdparty_yaml_cpp_version}.zip
  URL_HASH SHA256=a1d8401401de040fe78edd0e5499d37b72171864a58079f49cbcb7d5ca0b8a51
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME GSL
  VERSION "3.1.0"
  URL https://github.com/microsoft/GSL/archive/refs/tags/v3.1.0.zip
  URL_HASH SHA256=a1041e41e60f9cb3789036f1c84ea9b4298823cbe94d16b096971fdc3de485b7
  EXCLUDE_FROM_ALL YES
)


CPMAddPackage(
  NAME termbenchpro
  VERSION ${3rdparty_termbenchpro_version}
  URL https://github.com/contour-terminal/termbench-pro/archive/${3rdparty_termbenchpro_version}.zip
  URL_HASH SHA256=3746308910343641c2ccb7327698fff65e5833fe9a760cb4eea92570c4854f25
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME libunicode
  VERSION ${3rdparty_libunicode_version}
  URL https://github.com/contour-terminal/libunicode/archive/${3rdparty_libunicode_version}.zip
  URL_HASH SHA256=2253ef1aeaa4610068fd0c7eb2495cdc7f34617d4cbd0d8762f4d86d1bd76ac6
  EXCLUDE_FROM_ALL YES
)

if(CONTOUR_BUILD_WITH_MIMALLOC)
  set(MI_BUILD_SHARED OFF CACHE INTERNAL "")
  set(MI_BUILD_TESTS OFF CACHE INTERNAL "")
  CPMAddPackage(
    NAME mimalloc
    VERSION ${3rdparty_mimalloc_version}
    URL https://github.com/microsoft/mimalloc/archive/${3rdparty_mimalloc_version}.zip
    URL_HASH SHA256=6ccba822e251b8d10f8a63d5d7767bc0cbfae689756a4047cdf3d1e4a9fd33d0
    EXCLUDE_FROM_ALL YES
  )
endif()
