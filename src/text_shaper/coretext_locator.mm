// SPDX-License-Identifier: Apache-2.0
#include <crispy/utils.hpp>

#include <text_shaper/coretext_locator.hpp>
#include <text_shaper/font.hpp>
#include <text_shaper/font_locator.hpp>

#import <AppKit/AppKit.h>
#import <CoreText/CTFont.h>
#import <CoreText/CTFontDescriptor.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace text
{
    namespace
    {
        FontPath ctFontPath(NSString const* name)
        {
            auto const fontRef = CTFontDescriptorCreateWithNameAndSize((CFStringRef)name, 16.0);

            CFURLRef const url = (CFURLRef)CTFontDescriptorCopyAttribute(fontRef, kCTFontURLAttribute);
            NSString const* fontPath = [NSString stringWithString: [(NSURL const*)CFBridgingRelease(url) path]];

            return FontPath{[fontPath cStringUsingEncoding: [NSString defaultCStringEncoding]]};
        }

        constexpr NSFontWeight makeFontWeight(FontWeight value) noexcept
        {
            switch (value)
            {
                case FontWeight::Thin: return NSFontWeightThin;
                case FontWeight::ExtraLight: return NSFontWeightUltraLight;
                case FontWeight::Light: return NSFontWeightLight;
                case FontWeight::DemiLight: return NSFontWeightLight; // does not exist on CoreText
                case FontWeight::Book: return NSFontWeightRegular; // does not exist on CoreText
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

        [[maybe_unused]] constexpr FontWeight ctFontWeight(int weight) noexcept
        {
            switch (weight)
            {
            case 2: return FontWeight::Thin;
            case 3: return FontWeight::ExtraLight;
            case 4: return FontWeight::Light;
            case 5: return FontWeight::Normal;
            case 6: return FontWeight::Medium;
            case 8: return FontWeight::DemiBold;
            case 9: return FontWeight::Bold;
            case 10: return FontWeight::ExtraBold;
            case 11: return FontWeight::Black;
            default: return FontWeight::Normal;
            }
        }

        [[maybe_unused]] constexpr FontSlant ctFontSlant(int slant) noexcept
        {
            if (unsigned(slant) & NSItalicFontMask)
                return FontSlant::Italic;

            if (unsigned(slant) & (NSUnitalicFontMask | NSUnboldFontMask))
                return FontSlant::Normal;

            // Figure out how to get Oblique font, even though according to some docs.
            // Oblique font is actually a fancy way to say Italic.

            return FontSlant::Normal;
        }
    }

    struct CoreTextLocator::Private
    {
        NSFontManager* fm = [NSFontManager sharedFontManager];

        ~Private()
        {
            [fm release];
        }
    };

    CoreTextLocator::CoreTextLocator() :
        _d{ new Private(), [](Private* p) { delete p; } }
    {
    }

    FontSourceList CoreTextLocator::locate(FontDescription const& description)
    {
        locatorLog()("Locating font chain for: {}", description);

        FontSourceList fonts;

        CFStringRef familyName = CFStringCreateWithCString(
            kCFAllocatorDefault, description.familyName.c_str(), kCFStringEncodingUTF8);

        bool const isItalic = description.slant == FontSlant::Italic;
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
            fonts.emplace_back(FontPath { fontURL.path.UTF8String } );
            CFRelease(fontURL);
        }

        // Get Fallback List
        auto const* cascadeList = (CFArrayRef) CTFontCopyDefaultCascadeListForLanguages(
            font,
            (CFArrayRef) NSLocale.preferredLanguages
        );

        if(std::holds_alternative<FontFallbackNone>(description.fontFallback))
            locatorLog()("Skipping fallback fonts as font fallback is set to none");
        else if (cascadeList) {
            for (CFIndex i = 0; i < CFArrayGetCount(cascadeList); i++) {
                const auto* fallbackFont = (CTFontDescriptorRef) CFArrayGetValueAtIndex(cascadeList, i);

                const auto* fallbackFontName = (NSString*) CTFontDescriptorCopyAttribute(fallbackFont, kCTFontFamilyNameAttribute);

                if(const auto* list = std::get_if<FontFallbackList>(&description.fontFallback) )
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
                            fonts.emplace_back(FontPath { fontURL.path.UTF8String });
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

    FontSourceList CoreTextLocator::all()
    {
        FontSourceList output;

        NSArray<NSString *> const* const fonts = [_d->fm availableFonts];

        for (NSString const* fontName in fonts) {
            output.emplace_back(ctFontPath(fontName));
        }

        return output;
    }

    FontSourceList CoreTextLocator::resolve(gsl::span<const char32_t> /*codepoints*/)
    {
        return {};
    }
}
