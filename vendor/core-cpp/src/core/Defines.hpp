// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Portability macros: CORE_PACKED (a packed struct), CORE_REQUIRES (a requires-clause where
/// concepts are available), CORE_CONSTEVAL (consteval where available), CORE_CONSTEXPR
/// (constexpr where the standard algorithms are) and CORE_LIFETIMEBOUND (Clang's lifetimebound
/// where the compiler has it).

#ifdef __has_include
    #if __has_include(<version>)
        #include <version>
    #endif
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define CORE_PACKED __attribute__((packed))
#else
    #define CORE_PACKED /*!*/
#endif

#if (defined(__cpp_concepts) && __cpp_concepts >= 201500L) \
    && (defined(__cpp_lib_concepts) && __cpp_lib_concepts >= 202002L)
    #define CORE_CONCEPTS_SUPPORTED
    #define CORE_REQUIRES(x) requires x
#else
    #define CORE_REQUIRES(x) /*!*/
#endif

#if (defined(__cpp_consteval) && __cpp_consteval >= 201811L)
    #define CORE_CONSTEVAL consteval
#else
    #define CORE_CONSTEVAL constexpr
#endif

// Marks a reference parameter that the constructed object (or the returned reference) keeps, so a
// compiler that knows the attribute warns when a temporary is bound to it (-Wdangling). Placed after
// the parameter's name: `IClock const& clock CORE_LIFETIMEBOUND`. Nothing elsewhere, where GCC would
// warn about an unknown attribute.
#ifdef __has_cpp_attribute
    #if __has_cpp_attribute(clang::lifetimebound)
        #define CORE_LIFETIMEBOUND [[clang::lifetimebound]]
    #endif
#endif
#ifndef CORE_LIFETIMEBOUND
    #define CORE_LIFETIMEBOUND /*!*/
#endif

// Use this only when constexpr std algorithm is not supported but we still
// wanna mark function constexpr using the std algorithms in their body
#if (defined(__cpp_lib_constexpr_algorithms) && __cpp_lib_constexpr_algorithms >= 201806L)
    #define CORE_CONSTEXPR constexpr
#else
    #define CORE_CONSTEXPR /**/
#endif
