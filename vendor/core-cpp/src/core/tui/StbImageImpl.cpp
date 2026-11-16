// SPDX-License-Identifier: Apache-2.0
//
// The one translation unit that compiles stb_image's and stb_image_resize2's implementations,
// which are otherwise header-only declarations: two translation units defining them would collide
// at link time.
//
// It contains no code of core-cpp's, so its warnings are stb's. The module's CMakeLists.txt
// compiles this file with warnings off and, for the sanitizer builds, with -fno-sanitize=undefined
// (stb_image_resize2's SIMD paths trip UBSan's alignment checks). Those are per-source, PRIVATE
// compile options rather than pragmas in this file, because nothing here is ours to annotate.

// A file name is UTF-8, as every core-cpp path string is (loadImage()). Without this, stbi_load()
// opens it with fopen(), which on Windows reads the ANSI code page; with it, stb converts the name
// from UTF-8 and opens it with _wfopen_s(). It changes nothing on any other platform.
#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>
