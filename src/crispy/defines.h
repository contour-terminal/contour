// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __has_include
    #if __has_include(<version>)
        #include <version>
    #endif
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define CRISPY_PACKED __attribute__((packed))
#else
    #define CRISPY_PACKED /*!*/
#endif

#if (defined(__cpp_concepts) && __cpp_concepts >= 201500L) \
    && (defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L)
    #define CRISPY_REQUIRES(x) requires x
#else
    #define CRISPY_REQUIRES(x) /*!*/
#endif
