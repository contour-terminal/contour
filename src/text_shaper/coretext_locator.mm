// SPDX-License-Identifier: Apache-2.0
#include <crispy/utils.h>

#include <text_shaper/coretext_locator.h>
#include <text_shaper/font.h>
#include <text_shaper/font_locator.h>

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

namespace text
{
    namespace
    {
        font_path ctFontPath(NSString const* _name)
        {
            auto const fontRef = CTFontDescriptorCreateWithNameAndSize((CFStringRef)_name, 16.0);

            CFURLRef const url = (CFURLRef)CTFontDescriptorCopyAttribute(fontRef, kCTFontURLAttribute);
            NSString const* fontPath = [NSString stringWithString: [(NSURL const*)CFBridgingRelease(url) path]];

            return font_path{[fontPath cStringUsingEncoding: [NSString defaultCStringEncoding]]};
        }

        constexpr font_weight ctFontWeight(int _weight) noexcept
        {
            switch (_weight)
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

        constexpr font_slant ctFontSlant(int _slant) noexcept
        {
            if (unsigned(_slant) & NSItalicFontMask)
                return font_slant::italic;

            if (unsigned(_slant) & (NSUnitalicFontMask | NSUnboldFontMask))
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

    font_source_list coretext_locator::locate(font_description const& _fd)
    {
        locatorLog()("Locating font chain for: {}", _fd);

        font_source_list output;

        // "Emoji"
        auto const familyName = crispy::toLower(_fd.familyName) != "emoji"
                              ? _fd.familyName.c_str()
                              : "Apple Color Emoji";

        NSArray<NSArray *>* fonts = [_d->fm
            availableMembersOfFontFamily: [NSString
                stringWithCString: familyName
                encoding: [NSString defaultCStringEncoding]
            ]
        ];

        if (fonts == nil)
        {
            locatorLog()("No fonts found. Falling back to font family: Menlo.");
            fonts = [_d->fm availableMembersOfFontFamily:@"Menlo"];
        }

        for (bool const forceWeight: {true, false})
        {
            for ([[maybe_unused]] bool const forceSlant: {true, false})
            {
                for (NSArray* object in fonts)
                {
                    auto path = ctFontPath([object objectAtIndex: 0]);
                    auto const weight = ctFontWeight([[object objectAtIndex: 2] intValue]);

                    if (forceWeight && weight != _fd.weight)
                        continue;

                    auto const slant = ctFontSlant([[object objectAtIndex: 3] intValue]);

                    if (slant != _fd.slant)
                        continue;

                    output.emplace_back(ctFontPath([object objectAtIndex: 0]));
                }
                if (!output.empty())
                    break;
            }
            if (!output.empty())
                break;
        }

        return output;
    }

    font_source_list coretext_locator::all()
    {
        font_source_list output;

        NSArray<NSString *> const* fonts = [_d->fm availableFonts];

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
