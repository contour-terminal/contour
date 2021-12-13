#pragma once

#if defined(__GNUC__) || defined(__clang__)
    #define CONTOUR_PACKED __attribute__((packed))
#else
    #define CONTOUR_PACKED /*!*/
#endif

