// SPDX-License-Identifier: Apache-2.0
#include <crispy/utils.h>

#include <text_shaper/coretext_locator.h>
#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

#import <AppKit/AppKit.h>
#import <CoreText/CTFont.h>
#import <CoreText/CTFontDescriptor.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace text
{
    namespace
    {
        font_path ctFontPath(NSString const* name)
        {
            auto const fontRef = CTFontDescriptorCreateWithNameAndSize((CFStringRef)name, 16.0);

            CFURLRef const url = (CFURLRef)CTFontDescriptorCopyAttribute(fontRef, kCTFontURLAttribute);
            NSString const* fontPath = [NSString stringWithString: [(NSURL const*)CFBridgingRelease(url) path]];

            return font_path{[fontPath cStringUsingEncoding: [NSString defaultCStringEncoding]]};
        }

        constexpr NSFontWeight makeFontWeight(font_weight value) noexcept
        {
            switch (value)
            {
                case font_weight::thin: return NSFontWeightThin;
                case font_weight::extra_light: return NSFontWeightUltraLight;
                case font_weight::light: return NSFontWeightLight;
                case font_weight::demilight: return NSFontWeightLight; // does not exist on CoreText
                case font_weight::book: return NSFontWeightRegular; // does not exist on CoreText
                case font_weight::normal: return NSFontWeightRegular;
                case font_weight::medium: return NSFontWeightMedium;
                case font_weight::demibold: return NSFontWeightSemibold;
                case font_weight::bold: return NSFontWeightBold;
                case font_weight::extra_bold: return NSFontWeightHeavy;
                case font_weight::black: return NSFontWeightBlack;
                case font_weight::extra_black: return NSFontWeightBlack; // does not exist on CoreText
            }
            return NSFontWeightRegular;
        }

        [[maybe_unused]] constexpr font_weight ctFontWeight(int weight) noexcept
        {
            switch (weight)
            {
            case 2: return font_weight::thin;
            case 3: return font_weight::extra_light;
            case 4: return font_weight::light;
            case 5: return font_weight::normal;
            case 6: return font_weight::medium;
            case 8: return font_weight::demibold;
            case 9: return font_weight::bold;
            case 10: return font_weight::extra_bold;
            case 11: return font_weight::black;
            default: return font_weight::normal;
            }
        }

        [[maybe_unused]] constexpr font_slant ctFontSlant(int slant) noexcept
        {
            if (unsigned(slant) & NSItalicFontMask)
                return font_slant::italic;

            if (unsigned(slant) & (NSUnitalicFontMask | NSUnboldFontMask))
                return font_slant::normal;

            // Figure out how to get Oblique font, even though according to some docs.
            // Oblique font is actually a fancy way to say Italic.

            return font_slant::normal;
        }
    }

    struct coretext_locator::Private
    {
        NSFontManager* fm = [NSFontManager sharedFontManager];

        ~Private()
        {
            [fm release];
        }
    };

    coretext_locator::coretext_locator() :
        _d{ new Private(), [](Private* p) { delete p; } }
    {
    }

    font_source_list coretext_locator::locate(font_description const& description)
    {
        locatorLog()("Locating font chain for: {}", description);

        font_source_list fonts;

        CFStringRef familyName = CFStringCreateWithCString(
            kCFAllocatorDefault, description.familyName.c_str(), kCFStringEncodingUTF8);

        bool const isItalic = description.slant == font_slant::italic;
        auto const fontWeight = makeFontWeight(description.weight);
        auto const fontSlant = isItalic ? NSFontItalicTrait : 0;

        CTFontDescriptorRef descriptor = CTFontDescriptorCreateWithAttributes(
            (__bridge CFDictionaryRef) @{
                (id)kCTFontFamilyNameAttribute: (__bridge id)familyName,
                (id)kCTFontWeightTrait: @(fontWeight),
                (id)kCTFontSlantTrait: @(fontSlant)
            });

        CTFontRef font = CTFontCreateWithFontDescriptor(descriptor, 0.0, nullptr);
        if (font) {
            auto* const fontURL = (NSURL const*) CTFontCopyAttribute(font, kCTFontURLAttribute);
            fonts.emplace_back(font_path { fontURL.path.UTF8String } );
            CFRelease(fontURL);
        }

        // Get Fallback List
        auto const* cascadeList = (CFArrayRef) CTFontCopyDefaultCascadeListForLanguages(
            font,
            (CFArrayRef) NSLocale.preferredLanguages
        );

        if(std::holds_alternative<font_fallback_none>(description.fontFallback))
            locatorLog()("Skipping fallback fonts as font fallback is set to none");
        else if (cascadeList) {
            for (CFIndex i = 0; i < CFArrayGetCount(cascadeList); i++) {
                const auto* fallbackFont = (CTFontDescriptorRef) CFArrayGetValueAtIndex(cascadeList, i);

                const auto* fallbackFontName = (NSString*) CTFontDescriptorCopyAttribute(fallbackFont, kCTFontFamilyNameAttribute);

                if(const auto* list = std::get_if<font_fallback_list>(&description.fontFallback) )
                {
                    locatorLog()("Checking if {} is in the list of allowed fallback fonts\n", std::string([fallbackFontName UTF8String]));
                    if( std::find(list->fallbackFonts.begin(), list->fallbackFonts.end(), std::string([fallbackFontName UTF8String])) == list->fallbackFonts.end())
                    {
                        locatorLog()("Skipping fallback font {} as it is not in the list of allowed fallback fonts", std::string([fallbackFontName UTF8String]));
                        continue;
                    }
                }

                if (fallbackFont) {
                    CTFontRef fallbackFontRef = CTFontCreateWithFontDescriptor(fallbackFont, 0.0, nullptr);
                    if (fallbackFontRef) {
                        auto const* const fontURL = (NSURL const*) CTFontCopyAttribute(fallbackFontRef, kCTFontURLAttribute);
                        if (fontURL)
                        {
                            fonts.emplace_back(font_path { fontURL.path.UTF8String });
                            CFRelease(fontURL);
                        }
                        else
                            locatorLog()("No URL found for fallback font at index {}", i);
                        CFRelease(fallbackFontRef);
                    }
                }
                else
                    locatorLog()("No fallback font found at index {}", i);
            }
        }
        else
            locatorLog()("No fallback fonts found");

        CFRelease(cascadeList);
        CFRelease(familyName);
        CFBridgingRelease(familyName);

        return fonts;
    }

    font_source_list coretext_locator::all()
    {
        font_source_list output;

        NSArray<NSString *> const* const fonts = [_d->fm availableFonts];

        for (NSString const* fontName in fonts) {
            output.emplace_back(ctFontPath(fontName));
        }

        return output;
    }

    font_source_list coretext_locator::resolve(gsl::span<const char32_t> /*codepoints*/)
    {
        return {};
    }
}
