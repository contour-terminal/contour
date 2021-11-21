include(CPM)

set(3rdparty_catch2_version "2db1cf34047f76cb2679a3804b476881536ad27b" CACHE STRING "catch2: commit hash")
set(3rdparty_fmt_version "19cac63fe4b4d8fe6a4ced28de16a68659cf9035" CACHE STRING "fmt: commit hash")
set(3rdparty_freetype_version "81912a1385e8fc7694eda820221e15745cdcada4" CACHE STRING "freetype: commit hash")
set(3rdparty_gsl_version "e0880931ae5885eb988d1a8a57acf8bc2b8dacda" CACHE STRING "GSL: commit hash")
set(3rdparty_harfbuzz_version "84dc4e85e889d4b24cca7bb8ef04563fc6d1c3e6" CACHE STRING "harfbuzz: commit hash")
set(3rdparty_libunicode_version "3c59a3a0eb0c57c3081d48ea77c22809ac7c2d6e" CACHE STRING "libunicode: commit hash")
set(3rdparty_mimalloc_version "v2.0.3" CACHE STRING "mimalloc: release tag")
set(3rdparty_range_v3_version "83783f578e0e6666d68a3bf17b0038a80e62530e" CACHE STRING "range_v3: commit hash")
set(3rdparty_yaml_cpp_version "328d2d85e833be7cb5a0ab246cc3f5d7e16fc67a" CACHE STRING "yaml-cpp: commit hash")
set(3rdparty_termbenchpro_version "5a79261fbf5d26c9bf9d9a0d31f22ef4556cdd3b" CACHE STRING "termbench-pro: commit hash")

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
    URL_HASH SHA256=2ab73811ae7e931f7014a0619cf2947a6937eaa8794a4b44bd29acc5b1ed67eb
    EXCLUDE_FROM_ALL YES
  )
endif()

CPMAddPackage(
  NAME fmt
  VERSION ${3rdparty_fmt_version}
  URL https://github.com/fmtlib/fmt/archive/${3rdparty_fmt_version}.zip
  URL_HASH SHA256=a84fe9643527312fad9a1eef94dd6ebf2358d13c030f90457c3c25ebcf62b702
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME range_v3
  VERSION ${3rdparty_range_v3_version}
  URL https://github.com/ericniebler/range-v3/archive/${3rdparty_range_v3_version}.zip
  URL_HASH SHA256=eb976c9cf3ac4ae3a1c0ebd37e3a0230c73e92ec030e7f2a18b0e1597c70291c
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
  URL_HASH SHA256=b6b314ad5a5095536d0066dc5f9c4dd041039071b34cc7163f6d63b6ad81d023
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME GSL
  VERSION ${3rdparty_gsl_version}
  URL https://github.com/microsoft/GSL/archive/${3rdparty_gsl_version}.zip
  URL_HASH SHA256=4ece9bdccbd140d8fcd8df471f6da4c49d8a26ff0bca4ff329683995b3cce274
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME termbenchpro
  VERSION ${3rdparty_termbenchpro_version}
  URL https://github.com/contour-terminal/termbench-pro/archive/${3rdparty_termbenchpro_version}.zip
  URL_HASH SHA256=6c5f044e7738a560c70794dbcf61e3011be4514f43bda55962084f81c7a20270
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME libunicode
  VERSION ${3rdparty_libunicode_version}
  URL https://github.com/contour-terminal/libunicode/archive/${3rdparty_libunicode_version}.zip
  URL_HASH SHA256=1884993f33147121660cdc0df50d2d7b5fc8c699a01ea83a3ff7b16409893989
  EXCLUDE_FROM_ALL YES
)

if(CONTOUR_BUILD_WITH_MIMALLOC)
  set(MI_BUILD_SHARED OFF CACHE INTERNAL "")
  set(MI_BUILD_TESTS OFF CACHE INTERNAL "")
  CPMAddPackage(
    NAME mimalloc
    VERSION ${3rdparty_mimalloc_version}
    URL https://github.com/microsoft/mimalloc/archive/${3rdparty_mimalloc_version}.zip
    URL_HASH SHA256=8e5f0b74fdafab09e8853415700a9ade4d62d5f56cd43f54adf02580ceda86c1
    EXCLUDE_FROM_ALL YES
  )
endif()

if(CONTOUR_BUILD_WITH_EMBEDDED_FT_HB)
  CPMAddPackage(
    NAME harfbuzz
    VERSION ${3rdparty_harfbuzz_version}
    URL https://github.com/harfbuzz/harfbuzz/archive/${3rdparty_harfbuzz_version}.zip
    URL_HASH SHA256=b94f2c95b347fca0414892a79022d9a12e684ddbc65529c57a9cba4e2d8d3a42
  )

  CPMAddPackage(
    NAME freetype
    VERSION ${3rdparty_freetype_version}
    URL https://github.com/freetype/freetype/archive/${3rdparty_freetype_version}.zip
    URL_HASH SHA256=9c02e5a4309260288a2cba071af8fbe1cf430a5c6d3caa5a8ab973e5e2a83d90
  )
endif()
