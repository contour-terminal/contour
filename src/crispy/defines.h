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

#if (defined(__cpp_consteval) && __cpp_consteval >= 201811L)
    #define CRISPY_CONSTEVAL consteval
#else
    #define CRISPY_CONSTEVAL constexpr
#endif

// Use this only when constexpr std algorithm is not supported but we still
// wanna mark function constexpr using the std algorithms in their body
#if (defined(__cpp_lib_constexpr_algorithms) && __cpp_lib_constexpr_algorithms >= 201806L)
    #define CRISPY_CONSTEXPR constexpr
#else
    #define CRISPY_CONSTEXPR /**/
#endif
