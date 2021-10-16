include(CPM)

set(3rdparty_catch2_version "3e9c6fec222a5b307a712ba47dc4e930b37ad4e5" CACHE STRING "catch2: commit hash")
set(3rdparty_fmt_version "0a985fd4c6115eae8a455b110a747b265d1af2d1" CACHE STRING "fmt: commit hash")
set(3rdparty_freetype_version "bbab0a675ef29b0eaa96054a69ada63fc3051c51" CACHE STRING "freetype: commit hash")
set(3rdparty_harfbuzz_version "d58f1685cee7fef904f4149561a2dcf5ec519ac3" CACHE STRING "harfbuzz: commit hash")
set(3rdparty_libunicode_version "40e520d17febaca90e49d4d0db1252a1d345a91d" CACHE STRING "libunicode: commit hash")
set(3rdparty_mimalloc_version "v2.0.2" CACHE STRING "mimalloc: release tag")
set(3rdparty_range_v3_version "8f690283cc03146ad20514741cf69eafb325e974" CACHE STRING "range_v3: commit hash")
set(3rdparty_yaml_cpp_version "0d9dbcfe8c0df699aed8ae050dddaca614178fb1" CACHE STRING "yaml-cpp: commit hash")
set(3rdparty_termbenchpro_version "513cc8a7eb15b3b1a7940816944056d168d2c22a" CACHE STRING "termbench-pro: commit hash")

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
    URL_HASH SHA256=d39352486878820a39cbe6f3500f7df8e75eb37e606e733dde234b4ac6e0823c
    EXCLUDE_FROM_ALL YES
  )
endif()

CPMAddPackage(
  NAME fmt
  VERSION ${3rdparty_fmt_version}
  URL https://github.com/fmtlib/fmt/archive/${3rdparty_fmt_version}.zip
  URL_HASH SHA256=e082c3af5cadc8afc544a5853c75e285270ee46406d7cacf22fe80fd404ea66a
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
  URL_HASH SHA256=7c13c96ffcce90dd734b945db7f1c13d0af15add912ce14cc5943bd68626b6cf
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
  URL_HASH SHA256=555d6dd4e3017e93cc3d6a0b0507e0e1a8eb4534d636ce6bf30ead2d8c0a9e0c
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME libunicode
  VERSION ${3rdparty_libunicode_version}
  URL https://github.com/contour-terminal/libunicode/archive/${3rdparty_libunicode_version}.zip
  URL_HASH SHA256=d660e5d701917aa105323cb55015c2d5076c66e004e97e522d44345916acd6bb
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

if(CONTOUR_BUILD_WITH_EMBEDDED_FT_HB)
  CPMAddPackage(
    NAME harfbuzz
    VERSION ${3rdparty_harfbuzz_version}
    URL https://github.com/harfbuzz/harfbuzz/archive/${3rdparty_harfbuzz_version}.zip
    URL_HASH SHA256=cfff8c1703536f61b0c90f82ae1ca3e62e43bdf99c07504a3d107fd8ec808cbc
  )

  CPMAddPackage(
    NAME freetype
    VERSION ${3rdparty_freetype_version}
    URL https://github.com/freetype/freetype/archive/${3rdparty_freetype_version}.zip
    URL_HASH SHA256=d3f4d2ed447d09263eaf0fb1dccedee67fb75664b28b4b70c72c50b6c5d2223e
  )
endif()
