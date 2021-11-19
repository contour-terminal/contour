include(CPM)
set(CPM_DOWNLOAD_ALL ON)

set(3rdparty_catch2_version       "b9baae6d938133ff2fdc938053e88ccf51dd3afe" CACHE STRING "catch2: commit hash")
set(3rdparty_fmt_version          "a3ab36c80399f13e05ff2ff142df58fa9f3fd103" CACHE STRING "fmt: commit hash")
set(3rdparty_freetype_version     "cff026d41599945498044d2f4dcc0e610ffb6929" CACHE STRING "freetype: commit hash")
set(3rdparty_gsl_version          "e0880931ae5885eb988d1a8a57acf8bc2b8dacda" CACHE STRING "GSL: commit hash")
set(3rdparty_harfbuzz_version     "720ab0883b4bd7daa32a3c46031a9d8adb5c8a5f" CACHE STRING "harfbuzz: commit hash")
set(3rdparty_libunicode_version   "3c59a3a0eb0c57c3081d48ea77c22809ac7c2d6e" CACHE STRING "libunicode: commit hash")
set(3rdparty_mimalloc_version     "0be71a2cac17062bd8913cbd272c472a44331b7f" CACHE STRING "mimalloc: commit hash")
set(3rdparty_range_v3_version     "83783f578e0e6666d68a3bf17b0038a80e62530e" CACHE STRING "range_v3: commit hash")
set(3rdparty_yaml_cpp_version     "1713859b054b0a7fd867a59905dfbb0d3f774d54" CACHE STRING "yaml-cpp: commit hash")
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
    GITHUB_REPOSITORY "catchorg/Catch2"
    GIT_TAG ${3rdparty_catch2_version}
    EXCLUDE_FROM_ALL YES
  )
endif()

CPMAddPackage(
  NAME fmt
  GITHUB_REPOSITORY "fmtlib/fmt"
  GIT_TAG ${3rdparty_fmt_version}
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME range_v3
  GITHUB_REPOSITORY "ericniebler/range-v3"
  GIT_TAG ${3rdparty_range_v3_version}
  EXCLUDE_FROM_ALL YES
)

set(YAML_CPP_BUILD_CONTRIB OFF CACHE INTERNAL "")
set(YAML_CPP_BUILD_TESTS OFF CACHE INTERNAL "")
set(YAML_CPP_BUILD_TOOLS OFF CACHE INTERNAL "")
set(YAML_CPP_INSTALL OFF CACHE INTERNAL "")
set(YAML_BUILD_SHARED_LIBS OFF CACHE INTERNAL "")

CPMAddPackage(
  NAME yaml_cpp
  GITHUB_REPOSITORY "jbeder/yaml-cpp"
  GIT_TAG ${3rdparty_yaml_cpp_version}
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME GSL
  GITHUB_REPOSITORY "microsoft/GSL"
  GIT_TAG ${3rdparty_gsl_version}
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME termbenchpro
  GITHUB_REPOSITORY "contour-terminal/termbench-pro"
  GIT_TAG ${3rdparty_termbenchpro_version}
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME libunicode
  GITHUB_REPOSITORY "contour-terminal/libunicode"
  GIT_TAG ${3rdparty_libunicode_version}
  EXCLUDE_FROM_ALL YES
)

if(CONTOUR_BUILD_WITH_MIMALLOC)
  set(MI_BUILD_SHARED OFF CACHE INTERNAL "")
  set(MI_BUILD_TESTS OFF CACHE INTERNAL "")
  CPMAddPackage(
    NAME mimalloc
    GITHUB_REPOSITORY "microsoft/mimalloc"
    GIT_TAG ${3rdparty_mimalloc_version}
    EXCLUDE_FROM_ALL YES
  )
endif()

if(CONTOUR_BUILD_WITH_EMBEDDED_FT_HB)
  CPMAddPackage(
    NAME harfbuzz
    GITHUB_REPOSITORY "harfbuzz/harfbuzz"
    GIT_TAG ${3rdparty_harfbuzz_version}
    EXCLUDE_FROM_ALL YES
  )

  CPMAddPackage(
    NAME freetype
    GITHUB_REPOSITORY "freetype/freetype"
    GIT_TAG ${3rdparty_freetype_version}
    EXCLUDE_FROM_ALL YES
  )
endif()
