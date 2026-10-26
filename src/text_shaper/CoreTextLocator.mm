// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/BundledFonts.hpp>
#include <text_shaper/CoreTextLocator.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <harfbuzz/hb.h>

#include <sys/stat.h>

#include <algorithm>
#include <bit>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

namespace text
{
namespace
{
    struct CFReleaseDeleter
    {
        template <typename T>
        void operator()(T* value) const
        {
            CFRelease(value);
        }
    };

    template <typename T>
    using CFObject = std::unique_ptr<std::remove_pointer_t<T>, CFReleaseDeleter>;
    using FtLibrary = std::unique_ptr<std::remove_pointer_t<FT_Library>, decltype(&FT_Done_FreeType)>;
    using FontBlob = std::unique_ptr<hb_blob_t, decltype(&hb_blob_destroy)>;
    using FtFace = std::unique_ptr<std::remove_pointer_t<FT_Face>, decltype(&FT_Done_Face)>;

    constexpr NSFontWeight makeFontWeight(FontWeight value) noexcept
    {
        switch (value)
        {
            case FontWeight::Thin: return NSFontWeightThin;
            case FontWeight::ExtraLight: return NSFontWeightUltraLight;
            case FontWeight::Light: return NSFontWeightLight;
            case FontWeight::DemiLight: return NSFontWeightLight; // does not exist on CoreText
            case FontWeight::Book: return NSFontWeightRegular;    // does not exist on CoreText
            case FontWeight::Normal: return NSFontWeightRegular;
            case FontWeight::Medium: return NSFontWeightMedium;
            case FontWeight::DemiBold: return NSFontWeightSemibold;
            case FontWeight::Bold: return NSFontWeightBold;
            case FontWeight::ExtraBold: return NSFontWeightHeavy;
            case FontWeight::Black: return NSFontWeightBlack;
            case FontWeight::ExtraBlack: return NSFontWeightBlack; // does not exist on CoreText
        }
        return NSFontWeightRegular;
    }

    [[nodiscard]] CFObject<CTFontRef> matchFamily(std::string const& family,
                                                  FontDescription const& description)
    {
        auto nativeFamily = family == "emoji" ? std::string("Apple Color Emoji")
                                              : std::string(platformFontFamily(family));
        auto const name = CFObject<CFStringRef>(
            CFStringCreateWithCString(kCFAllocatorDefault, nativeFamily.c_str(), kCFStringEncodingUTF8));
        if (!name)
            return {};
        auto const weight = makeFontWeight(description.weight);
        auto const slant = description.slant == FontSlant::Normal ? 0.0 : 1.0;
        // Require the family, but allow the closest installed style. Traits belong in the
        // traits dictionary; a missing explicit fallback must not silently name a default font.
        auto const descriptor =
            CFObject<CTFontDescriptorRef>(CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef) @{
                (id) kCTFontFamilyNameAttribute: (__bridge id) name.get(),
                (id) kCTFontTraitsAttribute: @ {
                    (id) kCTFontWeightTrait: [NSNumber numberWithDouble:weight],
                    (id) kCTFontSlantTrait: [NSNumber numberWithDouble:slant],
                },
            }));
        auto const match = CFObject<CTFontDescriptorRef>(CTFontDescriptorCreateMatchingFontDescriptor(
            descriptor.get(), (__bridge CFSetRef) [NSSet setWithObject:(id) kCTFontFamilyNameAttribute]));
        if (!match)
        {
            locatorLog()("No installed font family matches {}", family);
            return {};
        }
        return CFObject<CTFontRef>(CTFontCreateWithFontDescriptor(match.get(), 16.0, nullptr));
    }

    // Replacing a font at the same path can change collection indices without changing names.
    // Include inode and ctime as well as size/mtime, so atomic replacements invalidate the table.
    [[nodiscard]] bool isSameFile(struct stat const& a, struct stat const& b) noexcept
    {
        return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
               && a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec
               && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec
               && a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec
               && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
    }
} // namespace

struct CoreTextLocator::Private
{
    struct FontFile
    {
        struct stat status {};
        FontBlob bytes { nullptr, hb_blob_destroy };
        FT_Long faceCount = 0;
        FT_Long nextFace = 0;
        std::unordered_map<std::string, int> indices;
        std::vector<std::pair<FT_Long, FT_ULong>> variableFaces;
    };

    std::mutex mutex;
    FtLibrary library { nullptr, FT_Done_FreeType };
    std::unordered_map<std::string, FontFile> files;

    Private()
    {
        FT_Library handle = nullptr;
        if (FT_Init_FreeType(&handle) == 0)
            library.reset(handle);
    }

    [[nodiscard]] FtFace openFace(FontFile const& file, FT_Long index) const
    {
        unsigned length = 0;
        auto const* bytes = hb_blob_get_data(file.bytes.get(), &length);
        FT_Face face = nullptr;
        if (FT_New_Memory_Face(library.get(),
                               reinterpret_cast<FT_Byte const*>(bytes),
                               static_cast<FT_Long>(length),
                               index,
                               &face)
            != 0)
            return { nullptr, FT_Done_Face };
        return { face, FT_Done_Face };
    }

    static void rememberFace(FontFile& file, FT_Face face, FT_Long index)
    {
        if (auto const* name = FT_Get_Postscript_Name(face))
            file.indices.try_emplace(name, static_cast<int>(index));
    }

    static void rememberBaseFace(FontFile& file, FT_Face face, FT_Long index)
    {
        rememberFace(file, face, index);
        auto const instances = static_cast<FT_ULong>(face->style_flags) >> 16u;
        if (instances != 0)
            file.variableFaces.emplace_back(index, instances);
    }

    // Cache file bytes and face metadata, never descriptors, coverage, point sizes or DPI.
    // The process-wide locator is shared by renderers, so the library and its tables share a lock.
    [[nodiscard]] std::optional<int> fontIndex(std::string const& path, std::string const& name)
    {
        auto const lock = std::lock_guard { mutex };
        struct stat status {};
        if (!library || ::stat(path.c_str(), &status) != 0)
            return std::nullopt;
        auto [entry, inserted] = files.try_emplace(path);
        auto& file = entry->second;
        if (inserted || !isSameFile(file.status, status))
        {
            file = FontFile {
                .status = status,
            };
            // HarfBuzz maps the file once. Collection faces then read the same bytes rather
            // than reopening the file and reading its SFNT tables through a stream each time.
            file.bytes.reset(hb_blob_create_from_file_or_fail(path.c_str()));
            auto const first = file.bytes ? openFace(file, 0) : FtFace { nullptr, FT_Done_Face };
            if (!first)
            {
                files.erase(entry);
                return std::nullopt;
            }
            file.faceCount = first->num_faces;
            file.nextFace = 1;
            rememberBaseFace(file, first.get(), 0);
        }
        if (auto const known = file.indices.find(name); known != file.indices.end())
            return known->second;

        // Discover collection faces lazily, once per file. A regular single-face font opens
        // just once; later descriptors for the same collection resume where its scan stopped.
        for (auto const index: std::views::iota(file.nextFace, file.faceCount))
        {
            auto const face = openFace(file, index);
            file.nextFace = index + 1;
            if (!face)
                continue;
            rememberBaseFace(file, face.get(), index);
            if (auto const known = file.indices.find(name); known != file.indices.end())
                return known->second;
        }

        // Named instances encode their one-based index in the high word. Only enumerate them
        // when no base face matches, and retain all their names so another miss cannot rescan.
        for (auto const& [base, count]: file.variableFaces)
            for (auto const instance: std::views::iota(FT_ULong { 1 }, count + 1))
            {
                auto const index = static_cast<FT_Long>(static_cast<FT_ULong>(base) | (instance << 16u));
                auto const face = openFace(file, index);
                if (face)
                    rememberFace(file, face.get(), index);
            }
        file.variableFaces.clear();
        if (auto const known = file.indices.find(name); known != file.indices.end())
            return known->second;
        return std::nullopt;
    }

    void appendFont(FontSourceList& output, CTFontRef font)
    {
        if (!font)
            return;
        auto const url =
            CFObject<CFURLRef>(static_cast<CFURLRef>(CTFontCopyAttribute(font, kCTFontURLAttribute)));
        if (!url)
            return;
        auto const* path = [(__bridge NSURL*) url.get() fileSystemRepresentation];
        if (!path)
            return;
        auto const name = CFObject<CFStringRef>(CTFontCopyPostScriptName(font));
        auto const* postscript = [(__bridge NSString*) name.get() UTF8String];
        if (!postscript || std::string_view(postscript) == "LastResort")
            return;
        auto const index = fontIndex(path, postscript);
        if (!index)
        {
            locatorLog()("Cannot map CoreText face {} to a FreeType face in {}", postscript, path);
            return;
        }
        if (std::ranges::any_of(output, [&](FontSource const& source) {
                auto const& existing = std::get<FontPath>(source);
                return existing.value == path && existing.collectionIndex == *index;
            }))
            return;
        output.emplace_back(FontPath {
            .value = path,
            .collectionIndex = *index,
        });
        locatorLog()("Adding font file: {} (face {})", output.back(), *index);
    }

    void appendDescriptors(FontSourceList& output, CFArrayRef descriptors)
    {
        if (!descriptors)
            return;
        for (auto const index: std::views::iota(CFIndex { 0 }, CFArrayGetCount(descriptors)))
        {
            auto const descriptor =
                static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(descriptors, index));
            auto const font = CFObject<CTFontRef>(CTFontCreateWithFontDescriptor(descriptor, 16.0, nullptr));
            appendFont(output, font.get());
        }
    }
};

CoreTextLocator::CoreTextLocator(): _d { new Private, [](Private* p) { delete p; } }
{
}

FontSourceList CoreTextLocator::locate(FontDescription const& description)
{
    @autoreleasepool
    {
        locatorLog()("Locating font chain for: {}", description);
        auto fonts = FontSourceList {};
        auto font = matchFamily(description.familyName, description);
        if (!font)
            font = matchFamily("Menlo", description);
        _d->appendFont(fonts, font.get());
        if (auto const* list = std::get_if<FontFallbackList>(&description.fontFallback))
        {
            for (auto const& family: list->fallbackFonts)
            {
                auto const fallback = matchFamily(family, description);
                _d->appendFont(fonts, fallback.get());
            }
        }
        else if (font && !std::holds_alternative<FontFallbackNone>(description.fontFallback))
        {
            auto const cascade = CFObject<CFArrayRef>(CTFontCopyDefaultCascadeListForLanguages(
                font.get(), (__bridge CFArrayRef) NSLocale.preferredLanguages));
            _d->appendDescriptors(fonts, cascade.get());
        }
        return fonts;
    }
}

FontSourceList CoreTextLocator::all()
{
    @autoreleasepool
    {
        auto fonts = FontSourceList {};
        auto const collection =
            CFObject<CTFontCollectionRef>(CTFontCollectionCreateFromAvailableFonts(nullptr));
        auto const descriptors =
            CFObject<CFArrayRef>(CTFontCollectionCreateMatchingFontDescriptors(collection.get()));
        _d->appendDescriptors(fonts, descriptors.get());
        return fonts;
    }
}

FontSourceList CoreTextLocator::resolve(gsl::span<char32_t const> codepoints)
{
    @autoreleasepool
    {
        auto fonts = FontSourceList {};
        if (codepoints.empty() || std::ranges::any_of(codepoints, [](char32_t cp) {
                return cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF);
            }))
            return fonts;
        // Let CoreFoundation transcode UTF-32 into its UTF-16 string representation. The
        // CoreText range must use that string's length, including surrogate pairs and selectors.
        auto constexpr Encoding =
            std::endian::native == std::endian::little ? kCFStringEncodingUTF32LE : kCFStringEncodingUTF32BE;
        auto const string = CFObject<CFStringRef>(
            CFStringCreateWithBytes(kCFAllocatorDefault,
                                    reinterpret_cast<UInt8 const*>(codepoints.data()),
                                    static_cast<CFIndex>(codepoints.size() * sizeof(char32_t)),
                                    Encoding,
                                    false));
        auto const base = CFObject<CTFontRef>(CTFontCreateWithName(CFSTR("Menlo"), 16.0, nullptr));
        if (!string || !base)
            return fonts;
        auto const font = CFObject<CTFontRef>(
            CTFontCreateForString(base.get(), string.get(), CFRangeMake(0, CFStringGetLength(string.get()))));
        _d->appendFont(fonts, font.get());
        return fonts;
    }
}

std::expected<void, FontRegistrationError> registerBundledFonts(BundledFontDirectories const& directories)
{
    auto result = std::expected<void, FontRegistrationError> {};
    for (auto const& directory: directories.paths)
    {
        auto const files = listBundledFontFiles(directory);
        if (!files)
        {
            if (result)
                result = std::unexpected { files.error() };
            continue;
        }
        for (auto const& file: *files)
        {
            auto const url = CFObject<CFURLRef>(CFURLCreateFromFileSystemRepresentation(
                kCFAllocatorDefault,
                reinterpret_cast<UInt8 const*>(file.c_str()),
                static_cast<CFIndex>(file.native().size()),
                false));
            CFErrorRef rawError = nullptr;
            auto const registered =
                url && CTFontManagerRegisterFontsForURL(url.get(), kCTFontManagerScopeProcess, &rawError);
            auto const error = CFObject<CFErrorRef>(rawError);
            // Registering the same file twice is harmless: the font is already usable.
            auto const alreadyRegistered =
                error && CFErrorGetCode(error.get()) == kCTFontManagerErrorAlreadyRegistered;
            if (!registered && !alreadyRegistered && result)
                result = std::unexpected { FontRegistrationError::RegistrationFailed };
        }
    }
    return result;
}

} // namespace text
