include(CPM)

set(3rdparty_catch2_version "bf61a418cbc4d3b430e3d258c5287780944ad505" CACHE STRING "catch2: commit hash")
set(3rdparty_fmt_version "8465869d7b04ae00f657332b3374ecf95b5e49c0" CACHE STRING "fmt: commit hash")
set(3rdparty_libunicode_version "a0f72919e4520ee1a02890ea77f19ff16c92d4f8" CACHE STRING "libunicode: commit hash")
set(3rdparty_range_v3_version "0487cca29e352e8f16bbd91fda38e76e39a0ed28" CACHE STRING "range_v3: commit hash")
set(3rdparty_yaml_cpp_version "b591d8ae2ad1ff373273c3e05973adf6c46abfa8" CACHE STRING "yaml-cpp: commit hash")
set(3rdparty_termbenchpro_version "c2312fc2ebadf45adc724295951d078b1adb7ffe" CACHE STRING "termbench-pro: version")

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
  URL_HASH SHA256=3c0a45ee3e3688b407b4243e38768f346e75ec4a9b16cefbebf17252048395da
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
  URL_HASH SHA256=a047cb1266bd994b60980fdc48cf0bb63880dc62a6997849014aebdaaeaf4495
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME termbenchpro
  VERSION ${3rdparty_termbenchpro_version}
  URL https://github.com/contour-terminal/termbench-pro/archive/${3rdparty_termbenchpro_version}.zip
  URL_HASH SHA256=c3e52de47d1aee2d29d4249a2950fea72ef176da3dc4efa778a3c037f654e3eb
  EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
  NAME libunicode
  VERSION ${3rdparty_libunicode_version}
  URL https://github.com/contour-terminal/libunicode/archive/${3rdparty_libunicode_version}.zip
  URL_HASH SHA256=11c64919dbfb25b040b774e25ac8cfa7823216298bdd9131e21bd09556790dc1
  EXCLUDE_FROM_ALL YES
)

