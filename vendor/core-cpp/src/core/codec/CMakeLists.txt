# SPDX-License-Identifier: Apache-2.0

# core::codec: a tolerant, versionless field codec. Stream.hpp is the byte level (LEB128 varints, frames);
# Fields.hpp is the struct level, one `visitFields` list read and written by the same code. Both are
# platform-independent.
core_cpp_add_module(codec KIND STATIC
    HEADERS DecodeError.hpp Fields.hpp Stream.hpp
    SOURCES Stream.cpp
    PUBLIC_LIBS core::base)

core_cpp_add_test(codec SOURCES Fields_test.cpp Stream_test.cpp)
