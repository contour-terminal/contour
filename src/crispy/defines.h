/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2022 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
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
