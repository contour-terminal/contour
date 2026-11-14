// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/KeyEquivalentMatch.hpp>
#include <contour/platform/NativeKeyTrace.hpp>

#ifdef __APPLE__

    #include <format>
    #include <string>
    #include <utility>

    #import <AppKit/AppKit.h>

namespace contour::platform
{

namespace
{
    /// The first code point of an NSString, or 0 when empty.
    char32_t firstCodePoint(NSString* string)
    {
        if (string == nil || string.length == 0)
            return 0;
        return static_cast<char32_t>([string characterAtIndex:0]);
    }

    /// Every UTF-16 unit of an NSString as hex, e.g. "f701".
    std::string hexUnits(NSString* string)
    {
        auto result = std::string {};
        if (string == nil)
            return result;
        for (NSUInteger i = 0; i < string.length; ++i)
            result += std::format("{}{:x}", i == 0 ? "" : ",", static_cast<unsigned>([string characterAtIndex:i]));
        return result;
    }

    std::string className(id object)
    {
        if (object == nil)
            return "nil";
        auto const* const name = NSStringFromClass([object class]).UTF8String;
        return name != nullptr ? std::string { name } : std::string { "unknown" };
    }

    /// A menu item's title for a trace line; never null.
    std::string titleOf(NSMenuItem* item)
    {
        auto const* const title = item.title.UTF8String;
        return title != nullptr ? std::string { title } : std::string {};
    }

    /// The name a trace line gives an event's type.
    char const* typeName(NSEventType type)
    {
        switch (type)
        {
            case NSEventTypeKeyDown: return "keyDown";
            case NSEventTypeKeyUp: return "keyUp";
            default: return "flagsChanged";
        }
    }

    class CocoaNativeKeyTrace final: public INativeKeyTrace
    {
      public:
        explicit CocoaNativeKeyTrace(NativeKeyTraceSink sink): _sink(std::move(sink))
        {
            auto const mask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged;
            _monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                                             handler:^NSEvent*(NSEvent* event) {
                                                                 trace(event);
                                                                 return event;
                                                             }];
        }

        ~CocoaNativeKeyTrace() override
        {
            if (_monitor != nil)
                [NSEvent removeMonitor:_monitor];
        }

      private:
        void trace(NSEvent* event) const
        {
            auto const type = typeName(event.type);
            auto const flags = static_cast<uint64_t>(event.modifierFlags);
            // Characters only for a Command or Control chord, which is what the diagnosis is about: plain typing
            // (a password) is never logged.
            auto const isChord = (flags & (nsmod::Command | nsmod::Control)) != 0;
            auto chars = std::string { "withheld" };
            if (event.type != NSEventTypeFlagsChanged && isChord)
                chars = hexUnits(event.charactersIgnoringModifiers);
            _sink(std::format("NativeKey app {} keyCode={:#x} chars=[{}] flags={:#x} ({}) window={} responder={}",
                              type,
                              static_cast<unsigned>(event.keyCode),
                              chars,
                              flags,
                              describeNsModifiers(flags),
                              static_cast<long>(event.windowNumber),
                              className(NSApp.keyWindow.firstResponder)));

            if (event.type == NSEventTypeKeyDown && (flags & (nsmod::Command | nsmod::Control)) != 0)
                traceMenus(NSApp.mainMenu, firstCodePoint(event.charactersIgnoringModifiers), flags);
        }

        void traceMenus(NSMenu* menu, char32_t eventChar, uint64_t flags) const
        {
            if (menu == nil)
                return;
            [menu.itemArray enumerateObjectsUsingBlock:^(NSMenuItem* item, NSUInteger /*index*/, BOOL* /*stop*/) {
                if (item.keyEquivalent.length != 0)
                {
                    auto const itemMods = static_cast<uint64_t>(item.keyEquivalentModifierMask);
                    auto const match =
                        classifyKeyEquivalent(eventChar, flags, firstCodePoint(item.keyEquivalent), itemMods);
                    if (match != KeyEquivalentMatch::None)
                        _sink(std::format("NativeKey menu {} title='{}' enabled={} key=[{}] mask={:#x} ({})",
                                          match == KeyEquivalentMatch::Strict ? "strict" : "ignoring-function",
                                          titleOf(item),
                                          item.enabled ? "yes" : "no",
                                          hexUnits(item.keyEquivalent),
                                          itemMods,
                                          describeNsModifiers(itemMods)));
                }
                traceMenus(item.submenu, eventChar, flags);
            }];
        }

        NativeKeyTraceSink _sink;
        id _monitor = nil;
    };
} // namespace

std::unique_ptr<INativeKeyTrace> makeNativeKeyTrace(NativeKeyTraceSink sink)
{
    return std::make_unique<CocoaNativeKeyTrace>(std::move(sink));
}

} // namespace contour::platform

#endif
