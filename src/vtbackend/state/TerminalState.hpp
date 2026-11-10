// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/FileUrl.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/InFlightTail.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/Grid.hpp>
#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/screen/ProgramStatusSet.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/state/ImageState.hpp>
#include <vtbackend/state/InteractionState.hpp>
#include <vtbackend/state/ScreenState.hpp>
#include <vtbackend/state/ShellState.hpp>
#include <vtbackend/state/StateCodec.hpp>
#include <vtbackend/state/StateWire.hpp>
#include <vtbackend/vt/Charset.hpp>
#include <vtbackend/vt/DesktopNotification.hpp>
#include <vtbackend/vt/Functions.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>
#include <vtbackend/vt/ProgressState.hpp>
#include <vtbackend/vt/RectangularAreaChecksum.hpp>
#include <vtbackend/vt/VTType.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vtbackend
{

/// Field lists of the foreign types a TerminalState holds. @see FieldEncoder
namespace detail
{
    // {{{ foreign types

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, Margin::Vertical>
    void visitForeignFields(Visitor& visitor, T& margin)
    {
        visitor(1, margin.from);
        visitor(2, margin.to);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, Margin::Horizontal>
    void visitForeignFields(Visitor& visitor, T& margin)
    {
        visitor(1, margin.from);
        visitor(2, margin.to);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, Margin>
    void visitForeignFields(Visitor& visitor, T& margin)
    {
        visitor(1, margin.vertical);
        visitor(2, margin.horizontal);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, SavedTitles>
    void visitForeignFields(Visitor& visitor, T& titles)
    {
        visitor(1, titles.icon);
        visitor(2, titles.window);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, UserPreferredSupplementalSet>
    void visitForeignFields(Visitor& visitor, T& set)
    {
        visitor(1, set.intermediate);
        visitor(2, set.final);
        visitor(3, set.is96);
        visitor(4, set.minimumConformanceLevel);
        visitor(5, set.charset);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, FileUrl>
    void visitForeignFields(Visitor& visitor, T& url)
    {
        visitor(1, url.url);
        visitor(2, url.host);
        visitor(3, url.path);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, Progress>
    void visitForeignFields(Visitor& visitor, T& progress)
    {
        visitor(1, progress.state);
        visitor(2, progress.percentage);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, ProgramStatusRecord>
    void visitForeignFields(Visitor& visitor, T& record)
    {
        visitor(1, record.id);
        visitor(2, record.state);
        visitor(3, record.kind);
        visitor(4, record.progress);
        visitor(5, record.app);
        visitor(6, record.title);
        visitor(7, record.message);
        visitor(8, record.origin);
        visitor(9, record.since);
        visitor(10, record.generation);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, RGBColorPair>
    void visitForeignFields(Visitor& visitor, T& pair)
    {
        visitor(1, pair.foreground);
        visitor(2, pair.background);
    }

    template <typename Visitor, typename T>
        requires std::same_as<std::remove_const_t<T>, CursorColor>
    void visitForeignFields(Visitor& visitor, T& cursor)
    {
        visitor(1, cursor.color);
        visitor(2, cursor.textOverrideColor);
    }

    // }}}
} // namespace detail

/// Which page the cursor and the display are on, the status display, and the view into the history.
struct PagingState
{
    PageIndex cursorPage {};                     ///< Where cursor and VT output go.
    PageIndex displayedPage {};                  ///< What the user sees.
    PageIndex cursorPageBeforeAlternate {};      ///< Where leaving the alternate screen returns the cursor.
    PageIndex displayedPageBeforeAlternate {};   ///< What leaving it shows again (DECPCCM reset).
    std::array<PageIndex, 2> savedCursorPage {}; ///< The page each screen's DECSC was received on.
    ActiveStatusDisplay activeStatusDisplay = ActiveStatusDisplay::Main;   ///< DECSASD.
    StatusDisplayType statusDisplayType = StatusDisplayType::None;         ///< What is shown.
    StatusDisplayType selectedStatusDisplayType = StatusDisplayType::None; ///< What was selected.
    std::optional<StatusDisplayType> savedStatusDisplayType {};            ///< Shown before the notices.
    StatusNotices statusNotices {};                                        ///< The owners of the notices up.
    bool isDeniedStatusLineTitleOwed = false; ///< A denied host-writable line still owes its title.
    ScrollOffset scrollOffset {};             ///< How far the view is scrolled into the history.
    uint64_t displayedPageLinesFollowed = 0;  ///< @see Terminal::_displayedPageLinesFollowed

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.cursorPage);
        visitor(2, self.displayedPage);
        visitor(3, self.cursorPageBeforeAlternate);
        visitor(4, self.displayedPageBeforeAlternate);
        visitor(5, self.savedCursorPage);
        visitor(6, self.activeStatusDisplay);
        visitor(7, self.statusDisplayType);
        visitor(8, self.selectedStatusDisplayType);
        visitor(9, self.savedStatusDisplayType);
        visitor(10, self.statusNotices);
        visitor(11, self.isDeniedStatusLineTitleOwed);
        visitor(12, self.scrollOffset);
        visitor(13, self.displayedPageLinesFollowed);
    }
};

/// The three margins and the tab stops.
struct MarginsState
{
    Margin mainDisplay {};  ///< DECSTBM / DECSLRM of the main display, shared by every page.
    Margin hostWritable {}; ///< The host-writable status line's.
    Margin indicator {};    ///< The indicator status line's.
    /// The tab stops of the page's columns, ascending; none listed stands for the stops a terminal
    /// starts with. A terminal without any stop therefore comes back with the initial ones, and
    /// what was set or cleared right of the page is not carried: the format has no word for either.
    std::vector<ColumnOffset> tabs {};

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.mainDisplay);
        visitor(2, self.hostWritable);
        visitor(3, self.indicator);
        visitor(4, self.tabs);
    }
};

/// The part of a ColorPalette that control sequences change. The rest (highlights, status line colors, the
/// background image, ...) comes from the configuration the restoring process loads.
struct PaletteSlice
{
    ColorPalette::Palette palette {}; ///< The 256 colors, the dim and the special ones.
    RGBColor defaultForeground {};    ///< OSC 10.
    RGBColor defaultBackground {};    ///< OSC 11.
    CursorColor cursor {};            ///< OSC 12.
    RGBColor mouseForeground {};      ///< OSC 13.
    RGBColor mouseBackground {};      ///< OSC 14.
    std::array<std::optional<RGBColorPair>, ColorPalette::AlternateTextColorCount> alternateTextColors {};

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.palette);
        visitor(2, self.defaultForeground);
        visitor(3, self.defaultBackground);
        visitor(4, self.cursor);
        visitor(5, self.mouseForeground);
        visitor(6, self.mouseBackground);
        visitor(7, self.alternateTextColors);
    }
};

/// The palette in force and the XTPUSHCOLORS stack.
struct ColorsState
{
    PaletteSlice current {};            ///< The palette in force.
    std::vector<PaletteSlice> saved {}; ///< XTPUSHCOLORS slots; slot N is index N-1.
    uint64_t lastSaved = 0;             ///< @see Terminal::_lastSavedColorPalette

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.current);
        visitor(2, self.saved);
        visitor(3, self.lastSaved);
    }
};

/// The titles.
struct TitlesState
{
    std::string windowTitle {};              ///< OSC 2.
    std::string iconTitle {};                ///< OSC 1.
    std::vector<SavedTitles> savedTitles {}; ///< XTPUSHTITLE, deepest first.
    std::optional<std::string> tabName {};   ///< The tab's name.
    uint8_t titleModes = 0;                  ///< XTSMTITLE, a bit per feature.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.windowTitle);
        visitor(2, self.iconTitle);
        visitor(3, self.savedTitles);
        visitor(4, self.tabName);
        visitor(5, self.titleModes);
    }
};

/// Whether a sequence a mode turns on and off is recognised.
enum class SequenceAvailability : uint8_t
{
    Inactive = 0, ///< Ignored.
    Active,       ///< Recognised.
};

template <>
struct detail::StateEnumWire<SequenceAvailability>
{
    static constexpr auto Values =
        std::array { SequenceAvailability::Inactive, SequenceAvailability::Active };
};

/// The sequences a mode turns on and off past what the conformance level makes available: DECLRMM makes
/// CSI s set the left and right margins (DECSLRM) instead of saving the cursor (SCOSC). A DECSCL since makes
/// both available again, so neither follows from the modes.
inline constexpr auto ModeGatedSequences = std::array { DECSLRM, SCOSC };

/// What the terminal reports about itself, and what DECSTR and RIS reset to.
struct ProfileState
{
    VTType terminalId = VTType::VT525;                                           ///< DA1/DA2.
    VTType operatingLevel = VTType::VT525;                                       ///< DECSCL.
    ControlTransmissionMode c1TransmissionMode = ControlTransmissionMode::S7C1T; ///< S7C1T / S8C1T.
    UserPreferredSupplementalSet userPreferredSupplementalSet {};                ///< DECAUPSS.
    uint8_t checksumExtension = 0;                         ///< XTCHECKSUM, raw flag bits.
    unsigned maxSixelColorRegisters = 0;                   ///< XTSMGRAPHICS item 1.
    unsigned maxImageRegisterCount = 0;                    ///< XTSMGRAPHICS item 1 (images).
    bool usePrivateColorRegisters = false;                 ///< DECSET 1070.
    std::optional<ImageSize> negotiatedImageCanvasSize {}; ///< XTSMGRAPHICS item 2.
    CursorDisplay cursorDisplay = CursorDisplay::Steady;   ///< DECSCUSR.
    CursorShape cursorShape = CursorShape::Block;          ///< DECSCUSR.
    BellVolume warningBellVolume = BellVolume::High;       ///< DECSWBV.
    BellVolume marginBellVolume = BellVolume::Off;         ///< DECSMBV.
    std::chrono::milliseconds smoothLineScrolling {};      ///< DECSSCLS.

    /// Whether each of ModeGatedSequences is recognised.
    std::array<SequenceAvailability, ModeGatedSequences.size()> modeGatedSequences {};

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.terminalId);
        visitor(2, self.operatingLevel);
        visitor(3, self.c1TransmissionMode);
        visitor(4, self.userPreferredSupplementalSet);
        visitor(5, self.checksumExtension);
        visitor(6, self.maxSixelColorRegisters);
        visitor(7, self.maxImageRegisterCount);
        visitor(8, self.usePrivateColorRegisters);
        visitor(9, self.negotiatedImageCanvasSize);
        visitor(10, self.cursorDisplay);
        visitor(11, self.cursorShape);
        visitor(12, self.warningBellVolume);
        visitor(13, self.marginBellVolume);
        visitor(14, self.smoothLineScrolling);
        visitor(15, self.modeGatedSequences);
    }
};

/// A DECINVM invocation waiting for the main parser to return.
struct PendingMacroState
{
    int id = 0;       ///< The macro.
    std::string body; ///< A copy of its body.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.id);
        visitor(2, self.body);
    }
};

/// DECDMAC definitions and the macros running.
struct MacrosState
{
    std::map<int, std::string> macros {};        ///< The definitions, by id.
    std::vector<int> running {};                 ///< The macros running now, outermost first.
    std::optional<PendingMacroState> pending {}; ///< The invocation awaiting its run.
    bool areRunningMacrosVoid = false;           ///< clearMacros() voided the bodies running.
    uint64_t bytesRun = 0;                       ///< Macro bytes run by the latest invocation.
    std::string pendingLocalEcho {};             ///< Local echo produced while the parser ran.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.macros);
        visitor(2, self.running);
        visitor(3, self.pending);
        visitor(4, self.areRunningMacrosVoid);
        visitor(5, self.bytesRun);
        visitor(6, self.pendingLocalEcho);
    }
};

/// One defined glyph of a persisted soft font.
struct SoftGlyphState
{
    uint8_t position = 0; ///< 7-bit position in the set, 0x20..0x7F.
    uint8_t width = 0;    ///< Pixels; equals the font's full cell width.
    uint8_t height = 0;   ///< Pixels; equals the font's full cell height.
    std::vector<uint8_t>
        packedCoverage; ///< Row-major, top row first, 1 bit per pixel, least significant first.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.position);
        visitor(2, self.width);
        visitor(3, self.height);
        visitor(4, self.packedCoverage);
    }
};

/// One persisted DECDLD soft font. The glyph ids are content hashes, so nothing about identity travels: the
/// decoder rebuilds the glyphs, and the error glyph, from this.
struct SoftFontState
{
    uint8_t number = 0;     ///< Pfn, 0..2.
    std::string designator; ///< Dscs.
    uint8_t loadRank = 0;   ///< 0 = loaded first; decides which of two fonts sharing a designator wins.
    // The SoftFontDeclaration, enumerators as their numbers.
    uint8_t startPosition = 0x21;
    uint8_t erase = 0;
    uint8_t declaredWidth = 0;
    uint8_t declaredHeight = 0;
    uint8_t sizeSource = 0;
    uint16_t columnsPerPage = 80;
    uint8_t linesPerPage = 24;
    uint8_t usage = 0;
    uint8_t charsetSize = 0;
    // The SoftCellGeometry.
    uint8_t fullWidth = 0;
    uint8_t fullHeight = 0;
    uint8_t textWidth = 0;
    std::vector<SoftGlyphState> glyphs; ///< The defined positions, ascending.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.number);
        visitor(2, self.designator);
        visitor(3, self.loadRank);
        visitor(4, self.startPosition);
        visitor(5, self.erase);
        visitor(6, self.declaredWidth);
        visitor(7, self.declaredHeight);
        visitor(8, self.sizeSource);
        visitor(9, self.columnsPerPage);
        visitor(10, self.linesPerPage);
        visitor(11, self.usage);
        visitor(12, self.charsetSize);
        visitor(13, self.fullWidth);
        visitor(14, self.fullHeight);
        visitor(15, self.textWidth);
        visitor(16, self.glyphs);
    }
};

/// UDKs, the DEC locator and the DECDLD soft fonts.
struct KeyStringsState
{
    std::map<int, std::string> userDefinedKeys {}; ///< DECUDK strings, by key.
    bool isUdkLocked = false;                      ///< DECUDK lock.
    Terminal::LocatorState locator {};             ///< DECELR / DECSLE.
    uint8_t locatorButtonsHeld = 0;                ///< The buttons down, DECLRP's bits.
    uint8_t locatorButtonsOwnedByApp = 0;          ///< Those whose press was the application's.
    std::vector<SoftFontState> softFonts {};       ///< The loaded DECDLD fonts, by number.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.userDefinedKeys);
        visitor(2, self.isUdkLocked);
        visitor(3, self.locator);
        visitor(4, self.locatorButtonsHeld);
        visitor(5, self.locatorButtonsOwnedByApp);
        // 6 and 7 held the v1 encoding's DRCS charsets and designators, which were stored under wrong
        // positions and never displayed. They are retired: not written, and a v1 stream's are skipped.
        visitor(8, self.softFonts);
    }
};

/// The sixel color registers the DECSET 1070-off sessions share.
struct SixelPaletteState
{
    std::vector<RGBColor> registers {}; ///< Every register, in index order.
    unsigned maxSize = 0;               ///< The ceiling the palette may grow to.

    /// The registers as the sixel string in flight hooked, which its replay starts from; none outside one.
    std::optional<std::vector<RGBColor>> registersAtHook {};

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.registers);
        visitor(2, self.maxSize);
        visitor(3, self.registersAtHook);
    }
};

/// Session-scoped facts programs reported.
struct SessionState
{
    std::string kittyClipboardWrite {};                ///< OSC 5522 wdata packets so far.
    bool isKittyClipboardWriteOpen = false;            ///< Whether such a write is open.
    FileUrl currentWorkingDirectory {};                ///< OSC 7.
    Progress progress {};                              ///< OSC 9;4.
    std::vector<ProgramStatusRecord> programStatus {}; ///< OSC 7501, least recently updated first.
    uint64_t programStatusLastGeneration = 0;  ///< The last program-status generation handed out; 0 for none.
    std::vector<std::string> pointerShapes {}; ///< OSC 22 stack, innermost last.
    bool isPointerShapeBaseSetByApplication = false; ///< Whether its bottom entry is the application's.
    std::vector<std::pair<std::string, std::string>> userVars {}; ///< OSC 1337 SetUserVar, first set first.
    uint64_t instructionCounter = 0;                              ///< Instructions parsed so far.
    std::chrono::system_clock::time_point sessionEpoch {};        ///< What rows' birth stamps count from.
    std::chrono::system_clock::duration hostClockOffset {};       ///< The host's clock less this one's.
    UserInputLock userInputLock = UserInputLock::Unlocked;        ///< The user's lock of the keyboard.

    /// How long ago synchronized output (DECSET 2026) began, while a batch is open; none otherwise. A
    /// steady-clock instant, so it crosses as a span and is rebased onto the restoring process's clock.
    std::optional<std::chrono::nanoseconds> synchronizedOutputFor {};

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.kittyClipboardWrite);
        visitor(2, self.isKittyClipboardWriteOpen);
        visitor(3, self.currentWorkingDirectory);
        visitor(4, self.progress);
        visitor(5, self.programStatus);
        visitor(6, self.pointerShapes);
        visitor(7, self.isPointerShapeBaseSetByApplication);
        visitor(8, self.userVars);
        visitor(9, self.instructionCounter);
        visitor(10, self.sessionEpoch);
        visitor(11, self.hostClockOffset);
        visitor(12, self.userInputLock);
        visitor(13, self.synchronizedOutputFor);
        visitor(14, self.programStatusLastGeneration);
    }
};

/// Which escape grammar the parser reads.
enum class ParserGrammar : uint8_t
{
    Ansi = 0, ///< ANSI: DECANM set.
    Vt52,     ///< VT52, entered by resetting DECANM and left by `ESC <`.
};

template <>
struct detail::StateEnumWire<ParserGrammar>
{
    static constexpr auto Values = std::array { ParserGrammar::Ansi, ParserGrammar::Vt52 };
};

/// What the parser keeps past the end of a sequence, which the bytes after it depend on. Everything else it
/// holds follows from the bytes in flight (TerminalState::inFlightTail).
struct ParserState
{
    ParserGrammar grammar = ParserGrammar::Ansi; ///< The grammar it reads.
    char32_t precedingGraphicCharacter = 0;      ///< What REP repeats; zero for nothing.
    HookOrigin hookOrigin = HookOrigin::Stream;  ///< Where the last DCS was hooked.

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.grammar);
        visitor(2, self.precedingGraphicCharacter);
        visitor(3, self.hookOrigin);
    }
};

/// The construction settings a restore depends on: a terminal is built from them (@see settingsFor), and
/// restoreState() refuses a terminal built otherwise. The rest of the settings are the configuration's, which
/// a reload after the restore applies through its own paths.
struct ConstructionState
{
    PageSize pageSize {};           ///< Settings::pageSize: the total page, status line included.
    HistoryLimits historyLimits {}; ///< The primary screen's scrollback limits, as they are now.
    ReflowOnResize primaryReflow = ReflowOnResize::No; ///< Settings::primaryScreen.allowReflowOnResize.
    unsigned maxImageRegisterCount = 0;   ///< Settings::maxImageRegisterCount: the sixel palette's size.
    uint64_t hyperlinkCacheCapacity = 0;  ///< Settings::hyperlinkCacheCapacity.
    uint64_t maxProgramStatusRecords = 0; ///< Settings::maxProgramStatusRecords.
    uint64_t maxCommandBlocks = 0;        ///< Settings::commandBlockLimits.
    uint64_t maxContextDepth = 0;         ///< Settings::contextLimits.maxDepth.
    uint64_t maxRetainedContexts = 0;     ///< Settings::contextLimits.maxRetained.

    bool operator==(ConstructionState const&) const = default;

    /// Lists the fields for the state codec.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.pageSize);
        visitor(2, self.historyLimits);
        visitor(3, self.primaryReflow);
        visitor(4, self.maxImageRegisterCount);
        visitor(5, self.hyperlinkCacheCapacity);
        visitor(6, self.maxProgramStatusRecords);
        visitor(7, self.maxCommandBlocks);
        visitor(8, self.maxContextDepth);
        visitor(9, self.maxRetainedContexts);
    }
};

/// Everything a Terminal holds that a later byte or input could observe, as plain data.
///
/// One member per area, each a field of the stream under a stable id: an area added later is one more
/// field, which an older reader skips and a newer one defaults when it is absent.
struct TerminalState
{
    ConstructionState construction {};          ///< What the terminal was constructed with.
    ModesState modes {};                        ///< The modes.
    PagingState paging {};                      ///< Page and status display selection.
    std::vector<ScreenState> pages {};          ///< The sixteen pages; page 0 is the primary screen.
    ScreenState hostWritableStatusLine {};      ///< The host-writable status line.
    ScreenState indicatorStatusLine {};         ///< The indicator status line.
    ImageTableState images {};                  ///< Every image any screen refers to, filed once.
    MarginsState margins {};                    ///< Margins and tab stops.
    ColorsState colors {};                      ///< Palettes.
    TitlesState titles {};                      ///< Titles.
    ProfileState profile {};                    ///< Identity and graphics limits.
    MacrosState macros {};                      ///< Macros.
    KeyStringsState keyStrings {};              ///< UDKs, locator, DRCS.
    std::optional<SixelPaletteState> sixel {};  ///< The shared sixel palette, if the terminal has one.
    HyperlinkStorageState hyperlinks {};        ///< OSC 8 links.
    SessionState session {};                    ///< Reported session facts.
    InputGenerator::State input {};             ///< The input generator.
    ShellState shell {};                        ///< Command blocks, contexts, marks and folds.
    InteractionState interaction {};            ///< Vi mode, selection and search.
    ParserState parser {};                      ///< What the parser keeps past a sequence.
    DesktopNotificationsState notifications {}; ///< OSC 99 notifications in flight and alive.

    /// The bytes a parser in Ground is fed to resume what this one has in flight: a half-decoded character's,
    /// then those of the escape sequence in flight. Empty at rest. A restore replays them last, rebuilding
    /// the parser, the sequence builder and a hooked DCS's sub-parser from them rather than from a state of
    /// their own; the effects they had (a C0 control executed inside a sequence, a DCS hook's) are already in
    /// the rest of the state, and the replay does not have them again.
    std::vector<uint8_t> inFlightTail {};

    /// Whether @ref inFlightTail is the whole of what is in flight. A state where it is not cannot be
    /// restored.
    TailCompleteness inFlightTailOverflowed = TailCompleteness::Complete;

    /// Lists the areas for the state codec, each with the name a test reports it by. The one list of them.
    void visitFields(this auto& self, auto& visitor)
    {
        visitor(1, self.construction, "construction");
        visitor(2, self.modes, "modes");
        visitor(3, self.paging, "paging");
        visitor(4, self.pages, "pages");
        visitor(5, self.hostWritableStatusLine, "hostWritableStatusLine");
        visitor(6, self.indicatorStatusLine, "indicatorStatusLine");
        visitor(7, self.images, "images");
        visitor(8, self.margins, "margins");
        visitor(9, self.colors, "colors");
        visitor(10, self.titles, "titles");
        visitor(11, self.profile, "profile");
        visitor(12, self.macros, "macros");
        visitor(13, self.keyStrings, "keyStrings");
        visitor(14, self.sixel, "sixel");
        visitor(15, self.hyperlinks, "hyperlinks");
        visitor(16, self.session, "session");
        visitor(17, self.input, "input");
        visitor(18, self.shell, "shell");
        visitor(19, self.interaction, "interaction");
        visitor(20, self.parser, "parser");
        visitor(21, self.inFlightTail, "inFlightTail");
        visitor(22, self.inFlightTailOverflowed, "inFlightTailOverflowed");
        visitor(23, self.notifications, "notifications");
    }
};

/// The settings a terminal restoring @p state is to be constructed with: @p base, with every construction
/// setting the restore depends on (ConstructionState) taken from @p state.
/// @param state A captured terminal.
/// @param base  The settings the restoring process would otherwise construct the terminal with.
/// @return The settings, or TooLarge when @p state asks for a terminal beyond what this build constructs.
[[nodiscard]] std::expected<Settings, StateDecodeError> settingsFor(TerminalState const& state,
                                                                    Settings base);

} // namespace vtbackend
