// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/ContextId.hpp>
#include <vtbackend/core/GraphicsAttributes.hpp>
#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/core/LineFlags.hpp>
#include <vtbackend/core/Primitives.hpp>
#include <vtbackend/grid/CellProxy.hpp>
#include <vtbackend/grid/CellUtil.hpp>
#include <vtbackend/grid/LineSoA.hpp>
#include <vtbackend/grid/SoAClusterWriter.hpp>

#include <crispy/BufferObject.hpp>
#include <crispy/Comparison.hpp>

#include <core/Assert.hpp>
#include <core/Flags.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace vtbackend
{

/// How @ref Line::toUtf8 renders the cells with no text of their own (NoTextCellFlags): the continuation
/// cell(s) of a wide (double-width) character, the padding (CellFlag::WrapPadding) one leaves at the end of a
/// row it did not fit, and the lower part of a tall text-sizing block (CellFlag::MulticellContinuation).
enum class ContinuationCell : uint8_t
{
    /// Emit nothing: a wide character contributes a single codepoint, so the result is the text as
    /// written. This is what copy/yank and selection want.
    Collapse = 0,
    /// Emit one space per cell with no text of its own, so every grid cell contributes exactly one
    /// codepoint and a codepoint index into the result equals a column offset. This is what the hint
    /// scanner needs to map a regex match position back to a grid column.
    Pad = 1,
};

// clang-format off
template <typename, bool> struct OptionalProperty;
template <typename T> struct OptionalProperty<T, false> {};
template <typename T> struct OptionalProperty<T, true> { T value; };
// clang-format on

/// [[deprecated("Use LineSoA directly")]]
/// Kept for backward compatibility with RenderBufferBuilder.
struct TrivialLineBuffer
{
    ColumnCount displayWidth;
    GraphicsAttributes textAttributes;
    GraphicsAttributes fillAttributes = textAttributes;
    HyperlinkId hyperlink {};

    ColumnCount usedColumns {};
    crispy::BufferFragment<char> text {};

    void reset(GraphicsAttributes attributes) noexcept
    {
        textAttributes = attributes;
        fillAttributes = attributes;
        hyperlink = {};
        usedColumns = {};
        text.reset();
    }
};

/// What EVERY physical piece of a logical line carries, head or not: who wrote the row, which command
/// block it belongs to and when the cursor first reached it.
///
/// Unlike the head-only offsets (Line::commandEndOffset(), Line::promptEndOffset()), which name a border
/// WITHIN a logical line, these name where a row came from, and a wrap changes none of them -- so reflow
/// hands them to every piece it cuts. One value, so reflow hands them on without listing them, and a
/// further such attribute is a member here rather than an edit at each of its sites.
struct LineProvenance
{
    ContextId contextId {};    ///< The context that wrote the row. @see Line::contextId
    CommandBlockId blockId {}; ///< The command block the row belongs to. @see Line::blockId
    uint32_t bornAt = 0;       ///< When the cursor first reached the row. @see Line::bornAt

    bool operator==(LineProvenance const&) const = default;
};

/**
 * Line API backed by LineSoA (Structure-of-Arrays storage).
 */
class Line
{
  public:
    /// Buffer type for reflow overflow columns.
    using InflatedBuffer = LineSoA;

    Line() { initializeBlankLineSoA(_storage); }

    /// Constructs a blank (un-materialized) line of @p cols width with @p attrs as fill attributes.
    /// O(1): no per-column allocation. The line materializes lazily on first write.
    Line(ColumnCount cols, LineFlags flags = {}, GraphicsAttributes attrs = {}):
        _columns { cols }, _flags { flags }
    {
        initializeBlankLineSoA(_storage, attrs);
    }

    /// Construct from TrivialLineBuffer (backward compat -- converts to SoA internally).
    Line(LineFlags flags, TrivialLineBuffer const& buffer): _columns { buffer.displayWidth }, _flags { flags }
    {
        if (buffer.text.empty())
        {
            initializeBlankLineSoA(_storage, buffer.fillAttributes);
        }
        else
        {
            initializeLineSoA(_storage, buffer.displayWidth, buffer.fillAttributes);
            writeTextToSoA(_storage, 0, buffer.text.view(), buffer.textAttributes, buffer.hyperlink);
        }
    }

    /// Construct from a LineSoA (used by reflow to create new lines from overflow data).
    Line(LineFlags flags, LineSoA&& soa, ColumnCount cols):
        _storage { std::move(soa) }, _columns { cols }, _flags { flags }
    {
    }

    Line(Line const&) = default;
    Line(Line&&) noexcept = default;

    /// Assignment dirties the DESTINATION row: margin scrolls and rotations move whole lines
    /// between rows (`lineAt(t) = std::move(lineAt(s))`, std::rotate, std::fill_n in Grid.cpp),
    /// and the receiving row is what changed from a consumer's point of view. Constructors stay
    /// defaulted on purpose: constructing a fresh slot is not a row change, and `_dirty = true`
    /// by default makes new lines pending anyway (bootstrap and rebuilds self-heal).
    Line& operator=(Line const& other)
    {
        if (this == &other)
            return *this;
        _storage = other._storage;
        assignScalarsFrom(other);
        return *this;
    }

    Line& operator=(Line&& other) noexcept
    {
        if (this == &other)
            return *this;
        _storage = std::move(other._storage);
        assignScalarsFrom(other);
        return *this;
    }

    // --- Change tracking (the daemon's per-line delta source) -------------------------------
    //
    // A one-byte dirty bit set by every mutating entry point below, and a revision stamped
    // lazily in batches by Grid::finalizeRevisions() from the grid's single monotonic
    // sequence counter. Deliberately NOT a LineFlag: flags flow into rendering and test
    // equality, and a transport-layer bookkeeping bit must not.

    /// The batch sequence number of the last change (0 = never stamped).
    [[nodiscard]] uint64_t revision() const noexcept { return _revision; }

    /// Marks the line as changed; the next finalize pass stamps it.
    void markDirty() noexcept { _dirty = true; }

    /// @return True while a change awaits its revision stamp.
    [[nodiscard]] bool isDirty() const noexcept { return _dirty; }

    /// Stamps the line with @p seqno iff it is dirty, clearing the dirty bit.
    /// @return True if the line was dirty (and got stamped).
    bool stampRevision(uint64_t seqno) noexcept
    {
        if (!_dirty)
            return false;
        _revision = seqno;
        _dirty = false;
        return true;
    }

    // ----------------------------------------------------------------------------------------

    /// Reset the line to the blank state with the given fill attributes, carrying @p flags -- and the
    /// offset of each of them it already carried.
    /// O(1): clears the SoA arrays without re-allocating per-column storage.
    /// Skips the six vector swaps when the line is already blank with matching fillAttrs.
    void reset(LineFlags flags, GraphicsAttributes attributes) noexcept
    {
        resetTo(flags, attributes, _columns, Storage::Release);
    }

    void reset(LineFlags flags, GraphicsAttributes attributes, ColumnCount count) noexcept
    {
        resetTo(flags, attributes, count, Storage::Release);
    }

    /// Resets the line exactly as reset() does, but keeps its storage's capacity, so the next write
    /// into it assigns into that storage instead of allocating it again.
    ///
    /// For a line about to be written again at once: the one the scrollback ring evicts at capacity
    /// and hands back as the new bottom line (@see Grid::scrollUp). reset() releases the storage on
    /// purpose -- after a clear, resident memory should follow the lines in use -- but for this line
    /// the release and the allocation that follows a moment later are pure churn.
    /// @param flags      The line's new flags.
    /// @param attributes The fill attributes its blank cells appear to have.
    /// @param columns    The line's new column count.
    void recycle(LineFlags flags, GraphicsAttributes attributes, ColumnCount columns) noexcept
    {
        resetTo(flags, attributes, columns, Storage::Keep);
    }

    /// Fill all cells with the given codepoint and attributes.
    void fill(LineFlags flags,
              GraphicsAttributes const& attributes,
              char32_t codepoint,
              uint8_t width) noexcept
    {
        resetMetadata(flags);
        if (codepoint == 0)
        {
            initializeBlankLineSoA(_storage, attributes);
        }
        else
        {
            materialize();
            for (size_t i = 0; i < unbox<size_t>(_columns); ++i)
                writeCellToSoA(_storage, i, codepoint, width, attributes);
        }
    }

    /// Fill from a column offset with ASCII text.
    void fill(ColumnOffset start, GraphicsAttributes const& sgr, std::string_view ascii)
    {
        assert(unbox<size_t>(start) + ascii.size() <= unbox<size_t>(_columns));
        _dirty = true;
        materialize();
        auto constexpr AsciiWidth = 1;
        auto col = unbox<size_t>(start);
        for (char const ch: ascii)
            writeCellToSoA(_storage, col++, static_cast<char32_t>(ch), AsciiWidth, sgr);
        // Reset remaining cells on the line after the written text
        auto const remaining = unbox<size_t>(_columns) - col;
        if (remaining > 0)
            clearRange(_storage, col, remaining, GraphicsAttributes {});
    }

    /// Tests if the line is in the blank (un-materialized) state.
    /// Blank lines have all SoA arrays empty but a non-zero logical column count.
    [[nodiscard]] bool isBlank() const noexcept
    {
        return isBlankLineSoA(_storage) && unbox<size_t>(_columns) > 0;
    }

    /// Tests if the line is blank AND its cached fill attributes match @p attrs.
    /// This is the invariant used to short-circuit partial-line clear and copy operations:
    /// a blank line with matching fillAttrs is already in the target state, so mutating
    /// would only trigger materialization without any observable effect.
    [[nodiscard]] bool isBlankWithFillAttrs(GraphicsAttributes const& attrs) const noexcept
    {
        return isBlank() && _storage.fillAttrs == attrs;
    }

    /// Tests if all cells are empty.
    [[nodiscard]] bool empty() const noexcept
    {
        if (isBlank())
            return true;
        return trimBlankRight(_storage, unbox<size_t>(_columns)) == 0;
    }

    [[nodiscard]] ColumnCount size() const noexcept { return _columns; }

    void resize(ColumnCount count)
    {
        _dirty = true;
        if (isBlank())
        {
            _columns = count;
            return;
        }
        _columns = count;
        resizeLineSoA(_storage, count);
    }

    /// Materialize the SoA arrays in-place if the line is blank.
    /// After materialize() returns, all six SoA arrays are sized to @c _columns and
    /// initialized to default values + the cached @c fillAttrs.
    void materialize() noexcept
    {
        if (isBlank())
            initializeLineSoA(_storage, _columns, _storage.fillAttrs);
    }

    /// Force materialization and return a mutable reference to the underlying SoA storage.
    /// Use this in place of @c storage() at every call site that writes through SoA arrays.
    /// Dirties pessimistically: this is the bulk-ASCII funnel (ONE store per writeText call).
    [[nodiscard]] LineSoA& materializedStorage() noexcept
    {
        _dirty = true;
        materialize();
        return _storage;
    }

    [[nodiscard]] CellProxy useCellAt(ColumnOffset column) noexcept
    {
        Require(ColumnOffset(0) <= column);
        Require(column <= ColumnOffset::cast_from(size())); // Allow off-by-one for sentinel.
        // Dirtying is deferred to the CellProxy write methods: a read-only proxy
        // (cast from non-const Line, or a const-qualified comparison path) no longer
        // dirties the line spuriously.
        materialize();
        return { _storage, unbox<size_t>(column), &_dirty };
    }

    [[nodiscard]] uint8_t cellEmptyAt(ColumnOffset column) const noexcept
    {
        Require(ColumnOffset(0) <= column);
        Require(column < ColumnOffset::cast_from(size()));
        if (isBlank())
            return true;
        auto const col = unbox<size_t>(column);
        return _storage.codepoints[col] == 0 || _storage.codepoints[col] == 0x20;
    }

    [[nodiscard]] uint8_t cellWidthAt(ColumnOffset column) const noexcept
    {
        if (isBlank())
            return 1;
        return _storage.widths[unbox<size_t>(column)];
    }

    /// Whether the cell at @p column is the right half of a wide glyph drawn from the column before it.
    /// @param column Any column; one outside the line continues nothing.
    [[nodiscard]] bool continuesAWideGlyphAt(ColumnOffset column) const noexcept
    {
        if (isBlank() || column < ColumnOffset(0) || column >= boxed_cast<ColumnOffset>(size()))
            return false;
        return _storage.sgr[unbox<size_t>(column)].flags.test(CellFlag::WideCharContinuation);
    }

    [[nodiscard]] LineFlags flags() const noexcept { return _flags; }
    [[nodiscard]] LineFlags& flags() noexcept
    {
        // Pessimistic: DECDWL/DECDHL and friends mutate through this reference.
        _dirty = true;
        return _flags;
    }

    [[nodiscard]] bool marked() const noexcept { return isFlagEnabled(LineFlag::Marked); }
    void setMarked(bool enable) { setFlag(LineFlag::Marked, enable); }

    [[nodiscard]] bool wrapped() const noexcept { return isFlagEnabled(LineFlag::Wrapped); }
    void setWrapped(bool enable) { setFlag(LineFlag::Wrapped, enable); }

    [[nodiscard]] bool wrappable() const noexcept { return isFlagEnabled(LineFlag::Wrappable); }
    void setWrappable(bool enable) { setFlag(LineFlag::Wrappable, enable); }

    [[nodiscard]] LineFlags wrappableFlag() const noexcept
    {
        return wrappable() ? LineFlag::Wrappable : LineFlag::None;
    }
    [[nodiscard]] LineFlags wrappedFlag() const noexcept
    {
        return wrapped() ? LineFlag::Wrapped : LineFlag::None;
    }
    [[nodiscard]] LineFlags markedFlag() const noexcept
    {
        return marked() ? LineFlag::Marked : LineFlag::None;
    }

    /// The flags a continuation line inherits from the logical line it belongs to.
    ///
    /// Only the ones that describe a PHYSICAL line. The semantic marks (HeadOnlyLineFlags) deliberately
    /// stay behind on the head: they say where a prompt or a command's output begins, and a wrap does not
    /// begin a second one.
    [[nodiscard]] LineFlags inheritableFlags() const noexcept
    {
        auto constexpr Inheritables = LineFlags { LineFlag::Wrappable };
        return _flags & Inheritables;
    }

    /// How many columns of this LOGICAL line a finished command printed, when the shell closed that
    /// command part-way into the line. Meaningful only on a head carrying LineFlag::CommandEnd.
    ///
    /// A shell's precmd emits OSC 133;D at the cursor, and the cursor is still sitting at the end of the
    /// command's output whenever that output did not end in a newline — so the prompt printed next lands
    /// on the very same line. One line, two owners; this offset is the border between them. Zero (the
    /// common case) means the output did end in a newline and the whole line belongs to the prompt.
    ///
    /// Counted from the start of the LOGICAL line, not of the physical one it happens to fall in: the rows
    /// above it times the page width, plus its column. That is what lets it survive a resize, which moves it
    /// with the cell it names -- across the rows a reflow re-chops the line into and the padding a wide
    /// character leaves at a row's end, or held to its row where the row is cut instead.
    [[nodiscard]] ColumnOffset commandEndOffset() const noexcept { return _commandEndOffset; }
    void setCommandEndOffset(ColumnOffset offset) noexcept
    {
        _dirty = true;
        _commandEndOffset = offset;
    }

    /// Where on this LOGICAL line the shell's prompt stopped printing and the user's input begins — the
    /// column OSC 133;B was emitted at. Meaningful only on a head carrying LineFlag::PromptEnd.
    ///
    /// The mirror image of commandEndOffset(): that one says where the PREVIOUS command's output stopped
    /// on this line, this one says where the prompt did. Together they cut a line that a shell shares
    /// between three owners — trailing output, prompt, typed command — into its parts.
    ///
    /// Counted from the start of the LOGICAL line, and moved by a resize, as commandEndOffset() is.
    [[nodiscard]] ColumnOffset promptEndOffset() const noexcept { return _promptEndOffset; }

    /// The context that wrote this line, or zero when none was in effect.
    [[nodiscard]] ContextId contextId() const noexcept { return _contextId; }

    /// Records that this line was produced under @p id, if it was not already.
    ///
    /// A ZERO id is ignored, and that is load-bearing in two places. A line's author is set when the
    /// line is written and cleared when the line is REUSED (@see reset), so "no context is active
    /// right now" is an absence of information rather than a claim that this line had no author --
    /// the cursor merely returning to a line after its context ended must not erase what wrote it.
    /// And on a daemon mirror, whose ancestry is replicated rather than parsed, the local active
    /// context is routinely zero while the ids arriving on the wire are not; clearing here would wipe
    /// every replicated stamp the moment the cursor moved.
    ///
    /// Deliberately does NOT mark the line dirty. The id is derived state -- a frontend resolves it
    /// through the context stack -- and nothing about the line's CONTENT changed, so dirtying it would
    /// push a delta for a row that did not visually change, twice per command, forever.
    void adoptContext(ContextId id) noexcept
    {
        if (!!id)
            _contextId = id;
    }

    /// The shell command block this line belongs to.
    /// @return The block's id, or zero when none.
    /// @see CommandBlockStore
    [[nodiscard]] CommandBlockId blockId() const noexcept { return _blockId; }

    /// Records that this line belongs to the command block @p id.
    ///
    /// The contract of adoptContext(), for the same reasons: a ZERO id is ignored (no block being active is
    /// an absence of information, not a claim that the line has none), and adopting does NOT dirty the line
    /// -- the id is derived state, carried to a mirror with the line's next real change and in snapshots.
    /// Last writer wins, so this is for an explicit transition: the line a ;D closes is routinely the line
    /// the next ;A opens. A row merely reached takes a block only if it has none: adoptProvenance().
    /// @param id The block; zero changes nothing.
    void adoptBlock(CommandBlockId id) noexcept
    {
        if (!!id)
            _blockId = id;
    }

    /// Forgets which command block this line belongs to, and dirties it if it named one -- a mirror only
    /// revisits dirty rows.
    ///
    /// Only for when the ids themselves start over (CommandBlockStore::idEpoch()): the id this line carries
    /// would otherwise name a block minted after it was written. A reset aside, nothing else takes a block
    /// away from a line.
    void forgetBlock() noexcept
    {
        if (!_blockId)
            return;
        _blockId = {};
        _dirty = true;
    }

    /// When the cursor first reached this line.
    /// @return Seconds since the session began, PLUS ONE, so that zero can mean "never stamped"; MaxBornAt
    ///         when the line was born too late in the session for its time to be known.
    [[nodiscard]] uint32_t bornAt() const noexcept
    {
        return static_cast<uint32_t>(_bornAt[0]) | (static_cast<uint32_t>(_bornAt[1]) << 8U)
               | (static_cast<uint32_t>(_bornAt[2]) << 16U);
    }

    /// Stamps the time the cursor first reached this line, if it carries none yet.
    ///
    /// Unlike adoptBlock(), this DIRTIES the line -- once, the moment it is first reached -- so the
    /// daemon's delta carries the stamp (and the block id adopted with it) even for a line nothing is
    /// ever written to, such as a blank line in a command's output.
    /// @param secondsPlusOne Seconds since the session began, plus one. Zero stamps nothing; a value past
    ///        MaxBornAt is stored as MaxBornAt.
    void stampBornAt(uint32_t secondsPlusOne) noexcept
    {
        if (secondsPlusOne == 0 || bornAt() != 0)
            return;
        auto const value = std::min(secondsPlusOne, MaxBornAt);
        _bornAt = { static_cast<uint8_t>(value),
                    static_cast<uint8_t>(value >> 8U),
                    static_cast<uint8_t>(value >> 16U) };
        _dirty = true;
    }

    /// The largest birth stamp three bytes hold, and the one that means "born, but when is no longer
    /// known": a later stamp is saturated to it, so a row born that late reads as timeless
    /// (Terminal::lineBirthTime() reports none) rather than wrapping round to the session's start. The known
    /// times are the stamps below it: up to 2^24 - 2 seconds, about 194 days, into the session.
    static constexpr uint32_t MaxBornAt = (1U << 24U) - 1U;

    /// What this row hands on to every physical piece reflow cuts it into.
    /// @return Its context, block and birth stamp.
    [[nodiscard]] LineProvenance provenance() const noexcept
    {
        return LineProvenance { .contextId = _contextId, .blockId = _blockId, .bornAt = bornAt() };
    }

    /// Takes on each part of @p provenance this row does not carry yet: the context and the block where it
    /// has none, the birth stamp where it is unstamped.
    ///
    /// Fill-once, so a row is never relabelled by being reached again. It is what the cursor arriving on a
    /// row hands it (Screen::updateCursorIterator()), and what reflow hands every chunk and every rebuilt
    /// row -- freshly constructed, so carrying nothing, and so ending up with exactly @p provenance.
    /// Relabelling a row is an explicit transition's job: relabel().
    /// @param provenance What the row is to carry where it carries nothing.
    void adoptProvenance(LineProvenance const& provenance) noexcept
    {
        // Assigned rather than adopted: on a row that carries none, a zero id leaves it carrying none either
        // way, and this runs on every line feed, where the test of the id would be the dearer half.
        if (!_contextId)
            _contextId = provenance.contextId;
        if (!_blockId)
            _blockId = provenance.blockId;
        stampBornAt(provenance.bornAt);
    }

    /// Relabels this row as an explicit transition does -- OSC 133;A, an OSC 3008 context change: each id
    /// @p transition carries replaces the row's own, last writer wins. A zero id leaves the row's alone, and
    /// the birth stamp is never rewritten.
    ///
    /// Unlike adoptContext() and adoptBlock(), this DIRTIES the row when it changes it: the row may already
    /// be clean and replicated, and a daemon mirror's delta scan only revisits dirty rows.
    /// @param transition The ids the transition hands out.
    void relabel(LineProvenance const& transition) noexcept
    {
        if (!wouldRelabel(transition))
            return;
        adoptContext(transition.contextId);
        adoptBlock(transition.blockId);
        _dirty = true;
    }

    /// Whether relabel(@p transition) would change this row: whether it carries an id the row has not.
    ///
    /// Asked before the row is handed out for writing, which below the page top arms the grid's history delta
    /// scan whether or not anything then changes (Grid::changingLineAt()).
    /// @param transition The ids the transition hands out.
    [[nodiscard]] bool wouldRelabel(LineProvenance const& transition) const noexcept
    {
        return (!!transition.contextId && transition.contextId != _contextId)
               || (!!transition.blockId && transition.blockId != _blockId);
    }

    void setPromptEndOffset(ColumnOffset offset) noexcept
    {
        _dirty = true;
        _promptEndOffset = offset;
    }

    void setFlag(LineFlags flags, bool enable) noexcept
    {
        _dirty = true;
        if (enable)
            _flags.enable(flags);
        else
            _flags.disable(flags);
    }

    [[nodiscard]] bool isFlagEnabled(LineFlags flags) const noexcept { return (_flags & flags).any(); }

    [[nodiscard]] LineSoA reflow(ColumnCount newColumnCount);
    [[nodiscard]] std::string toUtf8() const;

    /// The text of the columns [@p begin, @p end) of this line, blank cells rendered as spaces.
    /// @param begin First column to render; clamped to the line.
    /// @param end One past the last column to render; clamped to the line.
    /// @param continuation Whether the cells with no text of their own collapse away or emit a space; see
    ///                     @ref ContinuationCell.
    [[nodiscard]] std::string toUtf8(ColumnOffset begin,
                                     ColumnOffset end,
                                     ContinuationCell continuation = ContinuationCell::Collapse) const;

    /// The full text of this line with exactly one codepoint per grid column: a wide character's
    /// continuation cell becomes a space, so a codepoint index into the result equals a column
    /// offset. Used by the hint scanner, which maps regex match positions back to grid columns.
    [[nodiscard]] std::string toUtf8ColumnAligned() const;

    [[nodiscard]] std::string toUtf8Trimmed() const;
    [[nodiscard]] std::string toUtf8Trimmed(bool stripLeadingSpaces, bool stripTrailingSpaces) const;

    /// The text of columns [@p begin, @p end) with per-cell SGR escape sequences interleaved so each
    /// cell's rendition (colours + style flags) is preserved — the shape `capture-pane -e` wants. An
    /// SGR sequence is emitted only where the rendition CHANGES (reset-first, so nothing leaks), and a
    /// trailing reset closes any rendition still open at the end. A blank line renders as spaces wearing
    /// its FILL rendition — which is the cursor's pen at erase time, not necessarily the default one.
    /// @param begin First column to render; clamped to the line.
    /// @param end One past the last column to render; clamped to the line.
    /// @return The columns as UTF-8 with interleaved SGR.
    [[nodiscard]] std::string toUtf8WithSgr(ColumnOffset begin, ColumnOffset end) const;

    /// Gives every cell that wears the default foreground or background @p colors instead, blank cells
    /// included: what a row drawn in colours of its own, such as the indicator status line, shows where
    /// something it printed reset its rendition.
    /// @param colors The colours the row's default stands for.
    void resolveDefaultColors(RGBColorPair colors) noexcept;

    /// How many leading columns survive a trailing-space trim — tmux's `grid_line_length`, which
    /// `capture-pane` applies to every row unless the caller asked for `-J`/`-N`.
    ///
    /// A trailing column is dropped only when it is the DEFAULT cell: no text or a plain space,
    /// wearing the default rendition, carrying neither a hyperlink nor an image fragment. A space on
    /// a coloured pen therefore survives — it is something the application drew, and dropping it
    /// would silently erase the colour of every cleared region from a `capture-pane -e`.
    /// @return The column count to render; 0 for a row that is default cells throughout.
    [[nodiscard]] ColumnCount trimmedColumns() const noexcept;

    /// Tests whether the line can render as ONE batched RenderLine: its shape allows it, or it is blank.
    /// O(1) -- reads the shape every write maintains. @see isRenderBatchable(LineShape)
    [[nodiscard]] bool isTrivialBuffer() const noexcept
    {
        return vtbackend::isRenderBatchable(_storage.shape) || isBlank();
    }

    /// Tests whether the parser may bulk-write ASCII into this line: its shape allows it, or it is blank.
    /// O(1) -- reads the shape every write maintains. @see isBulkWritable(LineShape)
    ///
    /// The blank test stays even though every reset leaves a blank line Uniform: a line moved from
    /// keeps its shape beside emptied arrays, and is blank until it is reset or assigned.
    [[nodiscard]] bool isBulkWritable() const noexcept
    {
        return vtbackend::isBulkWritable(_storage.shape) || isBlank();
    }

    /// Build a TrivialLineBuffer for the render fast path.
    /// Only valid when isTrivialBuffer() returns true.
    ///
    /// @c textOut carries exactly ONE codepoint per column, which is what makes this cheap and what
    /// bounds when it may be used: a cell holding a grapheme cluster cannot be represented here,
    /// because the flat text has no cell boundaries for the renderer to recover the extra codepoints
    /// from. Such a line therefore never reaches this function -- appending a continuation codepoint
    /// degrades the line to Mixed (@see appendCodepointToCluster), as writing a wide cell's
    /// continuation and an image fragment already do. Without that, everything after each cluster's
    /// base codepoint is silently dropped from the rendered text.
    ///
    /// One codepoint per column is NOT the same as one COLUMN per cell, and the difference is
    /// reachable: a wide character leaves the batched path only through the continuation cell it
    /// writes, and @c Screen::clearAndAdvance skips that fill when a single column is left, so a
    /// full-width character on the last writable column stays here as a two-column cell. Consumers
    /// must therefore measure each codepoint rather than assume it is one column wide.
    ///
    /// @param textOut receives the codepoints directly from SoA (no UTF-8 encoding).
    [[nodiscard]] TrivialLineBuffer trivialBuffer(std::u32string& textOut) const
    {
        if (isBlank())
        {
            textOut.clear();
            return TrivialLineBuffer {
                .displayWidth = _columns,
                .textAttributes = _storage.fillAttrs,
                .fillAttributes = _storage.fillAttrs,
                .hyperlink = HyperlinkId {},
                .usedColumns = ColumnCount(0),
            };
        }

        auto const cols = unbox<size_t>(_columns);
        auto const used = trimBlankRight(_storage, cols);

        auto const textAttrs = (cols > 0) ? _storage.sgr[0] : GraphicsAttributes {};

        // Direct copy from SoA codepoints — no UTF-8 encoding needed.
        textOut.resize(used);
        for (size_t i = 0; i < used; ++i)
            textOut[i] = (_storage.clusterSize[i] == 0) ? U' ' : _storage.codepoints[i];

        auto tb = TrivialLineBuffer {
            .displayWidth = _columns,
            .textAttributes = textAttrs,
            .fillAttributes = textAttrs,
            .hyperlink = (cols > 0) ? _storage.hyperlinks[0] : HyperlinkId {},
            .usedColumns = ColumnCount::cast_from(used),
        };
        // text field left empty — caller passes textOut to the renderer directly
        return tb;
    }

    /// Access the underlying SoA storage. The mutable overload dirties
    /// pessimistically — callers reach it to write.
    [[nodiscard]] LineSoA& storage() noexcept
    {
        _dirty = true;
        return _storage;
    }
    [[nodiscard]] LineSoA const& storage() const noexcept { return _storage; }

  private:
    /// What a reset does with the storage of the cells it clears.
    enum class Storage : uint8_t
    {
        Release, ///< Free it, so resident memory follows the lines in use (@see reset).
        Keep,    ///< Keep its capacity for the write about to follow (@see recycle).
    };

    /// What every reset clears besides the cells: flags, the context, the block, the birth time, and the
    /// prompt and command borders of the flags it drops.
    ///
    /// An offset belongs to its flag (commandEndOffset() to CommandEnd, promptEndOffset() to PromptEnd): a
    /// reset that keeps the flag keeps the border it names. Dropping only the offset would leave a prompt
    /// redrawn with `\r\e[K` saying its input begins at column 0, prompt and all.
    /// @param flags The flags the reused row starts with.
    void resetMetadata(LineFlags flags) noexcept
    {
        auto const keeps = [this, flags](LineFlag flag) {
            return _flags.contains(flag) && flags.contains(flag);
        };
        if (!keeps(LineFlag::CommandEnd))
            _commandEndOffset = {};
        if (!keeps(LineFlag::PromptEnd))
            _promptEndOffset = {};
        _dirty = true;
        _flags = flags;
        _contextId = {};
        _blockId = {};
        _bornAt = {};
    }

    /// Resets the line to a blank one of @p columns, filled with @p attributes. Skips the cells when
    /// the line is already blank with those attributes.
    /// @param flags      The line's new flags.
    /// @param attributes The fill attributes its blank cells appear to have.
    /// @param columns    The line's new column count.
    /// @param storage    Whether the cells' storage is released or kept.
    void resetTo(LineFlags flags,
                 GraphicsAttributes const& attributes,
                 ColumnCount columns,
                 Storage storage) noexcept
    {
        resetMetadata(flags);
        _columns = columns;
        if (isBlankWithFillAttrs(attributes))
            return;
        switch (storage)
        {
            case Storage::Release: initializeBlankLineSoA(_storage, attributes); break;
            case Storage::Keep: recycleBlankLineSoA(_storage, attributes); break;
        }
    }

    /// Takes every scalar of @p other, the storage aside, and dirties this row (see operator=).
    /// @param other The row being copied or moved from.
    void assignScalarsFrom(Line const& other) noexcept
    {
        _columns = other._columns;
        _flags = other._flags;
        _contextId = other._contextId;
        _commandEndOffset = other._commandEndOffset;
        _promptEndOffset = other._promptEndOffset;
        _revision = other._revision;
        _bornAt = other._bornAt;
        _blockId = other._blockId;
        _dirty = true;
    }

    LineSoA _storage;
    ColumnCount _columns {};
    LineFlags _flags {};

    /// The OSC 3008 context that wrote this line, or zero for none.
    ///
    /// Placed HERE on purpose: _columns is 4-aligned and _commandEndOffset must be too, so there was a
    /// two-byte padding hole right at this offset. Associating every line with the context that
    /// produced it therefore costs nothing per line -- which is what makes it affordable across a
    /// whole scrollback. The static_assert below is what turns a future field reordering that breaks
    /// that into a build error rather than a silent +8 bytes on every line in the grid.
    ///
    /// Unlike _commandEndOffset and _promptEndOffset, this is NOT head-only: those name a border
    /// WITHIN a logical line, so only its head can carry them, while this names who WROTE the line --
    /// and a wrap does not change the author of the continuation. @see detail::addNewWrappedLines.
    ContextId _contextId {};

    ColumnOffset _commandEndOffset {};
    ColumnOffset _promptEndOffset {};
    uint64_t _revision = 0; ///< Batch seqno of the last change (see revision()).
                            ///< Compared against Grid::_seqno via operator> in
                            ///< forEachLineChangedSince. Both are uint64_t — at
                            ///< one batch per ~20ms a wrap takes ~11.7 billion
                            ///< years, so wrap is not handled in the comparison.
    bool _dirty = true;     ///< Fresh lines are pending: bootstrap self-heals.

    /// The birth stamp bornAt() decodes, three bytes little-endian. Placed in the padding after _dirty,
    /// like _blockId below, so neither costs a byte per line; the static_assert after the class keeps it so.
    std::array<uint8_t, 3> _bornAt {};

    /// The command block blockId() reports. NOT head-only, for the reason _contextId gives: it names what
    /// produced the line, and a wrap does not change that.
    CommandBlockId _blockId {};
};

/// Line's size is a fact about the whole scrollback, so it is pinned rather than left to drift.
///
/// _contextId was placed in the padding hole between _flags and _commandEndOffset precisely so that
/// associating every line with the context that wrote it costs nothing. A reordering that pushes it
/// out of that hole costs eight bytes on every line the grid holds -- tens of thousands of them -- and
/// would otherwise do so silently. If this fires, do not simply update the number: put the field back
/// where it fits, or accept the cost deliberately.
///
/// Expressed RELATIVE to LineSoA rather than as an absolute byte count, because an absolute count is
/// not portable: LineSoA holds a std::vector and a std::optional<std::unordered_map>, whose sizes
/// differ between libstdc++, libc++ and the MSVC STL, so a pinned literal would be a build break on
/// macOS and Windows rather than a statement about Line's own tail. What matters here is exactly that
/// tail -- the scalars after the storage -- and it is the same 32 bytes everywhere.
///
/// The tail after LineSoA is, by offset: _columns (+0), _flags (+4), _contextId (+6), _commandEndOffset
/// (+8), _promptEndOffset (+12), _revision (+16), _dirty (+24), _bornAt (+25..27), _blockId (+28..31) --
/// every byte used, none added. A field that does not fit there is a decision to grow every line.
static_assert(sizeof(Line) == sizeof(LineSoA) + 32,
              "Line's scalar tail grew. See the comment on Line::_contextId before changing this.");

} // namespace vtbackend

// LineFlags formatter is in LineFlags.h
