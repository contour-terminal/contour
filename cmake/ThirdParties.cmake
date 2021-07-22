include(CPM)

set(3rdparty_catch2_version "bf61a418cbc4d3b430e3d258c5287780944ad505" CACHE STRING "catch2: commit hash")
set(3rdparty_fmt_version "561834650aa77ba37b15f7e5c9d5726be5127df9" CACHE STRING "fmt: commit hash")
set(3rdparty_libunicode_version "a0f72919e4520ee1a02890ea77f19ff16c92d4f8" CACHE STRING "libunicode: commit hash")
set(3rdparty_range_v3_version "0487cca29e352e8f16bbd91fda38e76e39a0ed28" CACHE STRING "range_v3: commit hash")
set(3rdparty_yaml_cpp_version "79aa6d53e5718ca44bc01ef05fdda7a849d353e0" CACHE STRING "yaml-cpp: commit hash")

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
    URL_HASH SHA256=7521e7e7ee7f2d301a639bdfe4a95855fbe503417d73af0934f9d1933ca38407
    EXCLUDE_FROM_ALL YES
  )
endif()

CPMAddPackage(
  NAME fmt
  VERSION ${3rdparty_fmt_version}
  URL https://github.com/fmtlib/fmt/archive/${3rdparty_fmt_version}.zip
  URL_HASH SHA256=310ba642b8944ecfc798fea39bfe66b91fd3c649d29c4fdfc218b0b2bb6c23d7
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME range_v3
  VERSION ${3rdparty_range_v3_version}
  URL https://github.com/ericniebler/range-v3/archive/${3rdparty_range_v3_version}.zip
  URL_HASH SHA256=e3992d30629d058e5918b9721d6fbdbc20f72b298cdf5cfb96e798fc4b5b54fe
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
  URL_HASH SHA256=d1822ca08b55eb55aa3176ee83873b0ed40390c11068c5b98fe5268c54a6c85f
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME libunicode
  VERSION ${3rdparty_libunicode_version}
  URL https://github.com/contour-terminal/libunicode/archive/${3rdparty_libunicode_version}.zip
  URL_HASH SHA256=11c64919dbfb25b040b774e25ac8cfa7823216298bdd9131e21bd09556790dc1
  EXCLUDE_FROM_ALL YES
)

