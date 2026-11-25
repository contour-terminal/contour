# vtextension_remote_wire: the protocol Contour speaks with its extension host process, and the channels it
# travels on. The codec (proto/) is pure bytes-in/PDUs-out and the transport (transport/) is the operating
# system's byte channels; neither links a WebAssembly engine or vtextension itself, so that both the parent and
# the helper process link them, and both are always built.

set(_channel_source transport/Channel_posix.cpp)
if(WIN32)
    set(_channel_source transport/Channel_win32.cpp)
endif()

add_library(vtextension_remote_wire STATIC
    proto/Codec.cpp
    proto/Codec.hpp
    proto/Pdu.hpp
    proto/PduTable.hpp
    proto/Wire.hpp
    transport/Channel.hpp
    ${_channel_source}
    transport/PduStream.cpp
    transport/PduStream.hpp
)
# The codec names the runtime vocabulary (WasmRuntime.hpp, ExtensionError.hpp), which is header-only and
# std-only: the include path is all it takes.
target_include_directories(vtextension_remote_wire PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/include>
)

option(VTEXTENSION_FUZZING "Builds the extension host protocol's libFuzzer target (Clang only) [default: OFF]" OFF)
if(VTEXTENSION_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "VTEXTENSION_FUZZING needs Clang, for -fsanitize=fuzzer.")
    endif()
    # Decodes its input as every (direction, channel) pair, and checks that what decodes encodes again to
    # the same PDU. CI runs the fixed-seed mutation case in vtextension_test instead.
    add_executable(vtextension_remote_fuzz proto/Codec_fuzz.cpp)
    target_compile_options(vtextension_remote_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_options(vtextension_remote_fuzz PRIVATE -fsanitize=fuzzer)
    target_link_libraries(vtextension_remote_fuzz PRIVATE vtextension_remote_wire)
endif()
