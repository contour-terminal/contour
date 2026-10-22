// SPDX-License-Identifier: Apache-2.0
//! Bundled extension for the `terminal-extensions:statusline` world: an animated plasma drawn with Sixel
//! across the whole surface, with text on top. The showcase for taking the status line over, and for the
//! timer and its rate cap.
//!
//! The image is a fixed plasma pattern of [`COLORS`] palette entries, and the animation cycles the
//! palette: each frame defines the colours anew and repeats the same pixels, so the encoded pixels are
//! computed once per surface size and only the palette per frame. It asks for a frame every
//! [`FRAME_MS`] milliseconds while the pane is focused, and holds still while it is not.
//!
//! The image covers the surface once the size of a cell is known. A host that knows its font says so in
//! `state` and in `font-changed`, and the image is drawn again at each new cell size; a host that reports
//! none (zeros) is asked instead (`CSI 16 t`), after the first frame and after each resize, and
//! [`DEFAULT_CELL`] is assumed until the reply arrives. What
//! one frame may cost is bounded whatever the surface's size: the image is at most [`MAX_BANDS`] Sixel
//! bands tall, and its pattern is sampled at most [`MAX_SAMPLES`] times, more coarsely the larger it is.
//! A host that refuses Sixel shows the text alone; placed as a segment, which carries text only, it is
//! the text alone too.
//!
//! Taking the status line over replaces what it showed, so the text may name what it showed: `{mode}`
//! (the input mode), `{title}` and `{tabs}` (every tab's title, the active one in brackets) are filled in,
//! and only what the text names is followed. Text from the pane is shown as text: control characters are
//! dropped.
//!
//! Settings:
//! - `text`: drawn centred on top of the plasma, in bold white (default: none).
//! - `speed`: how fast the colours cycle, as a factor (default 1).
//!
//! Built for `wasm32-wasip1` as a plain core module, never componentized.

use std::cell::{Cell, RefCell};
use std::f32::consts::TAU;
use std::fmt::Write;

use crate::terminal_extensions::statusline::host;
use crate::terminal_extensions::statusline::types::{FontMetrics, InputMode, Tab};
use crate::wide_ranges::DOUBLE_WIDTH;

mod wide_ranges;

wit_bindgen::generate!({
    path: "../../../wit/statusline",
    world: "statusline",
});

/// The keys of the settings.
const TEXT_KEY: &str = "text";
const SPEED_KEY: &str = "speed";

/// How long a frame lasts, in milliseconds: about thirty frames a second.
const FRAME_MS: u32 = 33;

/// How many colours the palette cycles through.
const COLORS: usize = 16;

/// How far the palette moves per frame at speed 1, in turns of the colour wheel.
const TURNS_PER_FRAME: f32 = 1.0 / 240.0;

/// How bright a palette entry gets at most, in Sixel's percent; dim enough for text to stand out.
const BRIGHTNESS: f32 = 70.0;

/// The pixel size of a cell, width by height, assumed until the host reports it.
const DEFAULT_CELL: (u32, u32) = (8, 16);

/// The query for the cell size, and the start of its answer, `CSI 6 ; height ; width t`.
const CELL_SIZE_QUERY: &str = "\x1b[16t";
const CELL_SIZE_REPLY: &str = "\x1b[6;";

/// The rows of pixels one Sixel band covers.
const BAND_HEIGHT: u32 = 6;

/// How many columns of pixels one sample of the pattern colours, at the least.
const SAMPLE_STEP: usize = 4;

/// The most bands an image has: a status line is a row or two, and a taller surface keeps its rows below
/// without the plasma.
const MAX_BANDS: u32 = 16;

/// The most samples of the pattern an image takes, so that the first frame of any surface, which encodes
/// its pixels, finishes well inside a call's deadline in an interpreter.
const MAX_SAMPLES: usize = 2048;

/// The largest cell extent, in pixels, a reply is believed for; the image's cost grows with it.
const MAX_CELL_PIXELS: u32 = 512;

/// The steps of a full turn in [`SINE`].
const SINE_STEPS: u32 = 256;

/// The largest magnitude in [`SINE`].
const SINE_AMPLITUDE: i32 = 127;

/// What a placeholder of the text stands for.
#[derive(Clone, Copy, PartialEq)]
enum Field {
    Mode,
    Title,
    Tabs,
}

/// Each placeholder the text may hold, what it stands for, and the event that announces its changes.
const PLACEHOLDERS: [(&str, Field, EventKinds); 3] = [
    ("{mode}", Field::Mode, EventKinds::INPUT_MODE),
    ("{title}", Field::Title, EventKinds::TITLE),
    ("{tabs}", Field::Tabs, EventKinds::TABS),
];

/// Each input mode, as the text shows it.
const MODE_NAMES: [(InputMode, &str); 6] = [
    (InputMode::Insert, "INSERT"),
    (InputMode::Normal, "NORMAL"),
    (InputMode::Visual, "VISUAL"),
    (InputMode::VisualLine, "VISUAL LINE"),
    (InputMode::VisualBlock, "VISUAL BLOCK"),
    (InputMode::Hint, "HINT"),
];

/// The ranges of characters that take no cell of their own: combining marks, zero-width characters and
/// variation selectors.
const ZERO_WIDTH: [(u32, u32); 9] = [
    (0x0300, 0x036F),
    (0x1AB0, 0x1AFF),
    (0x1DC0, 0x1DFF),
    (0x200B, 0x200F),
    (0x20D0, 0x20FF),
    (0xFE00, 0xFE0F),
    (0xFE20, 0xFE2F),
    (0xE0000, 0xE007F),
    (0xE0100, 0xE01EF),
];

/// One piece of the text: as written, or filled in.
enum Piece {
    Literal(String),
    Placeholder(Field),
}

/// The pixels of one surface size, Sixel-encoded and without a palette: what every frame repeats.
struct Pixels {
    width: u32,
    height: u32,
    encoded: String,
}

/// Where the cell size the image is drawn at comes from.
#[derive(Clone, Copy, PartialEq)]
enum CellSource {
    /// Nobody said: [`DEFAULT_CELL`] is assumed, and the surface is asked.
    Assumed,
    /// The surface answered `CSI 16 t`; asked again after a resize.
    Queried,
    /// The host's font says, and says again when it changes.
    Host,
}

/// Whether the pane is focused: the animation runs only while it is.
#[derive(Clone, Copy, PartialEq)]
enum Focus {
    Unfocused,
    Focused,
}

/// What the placeholders are filled in with.
struct Fields {
    mode: InputMode,
    title: String,
    tabs: String,
}

thread_local! {
    /// The text drawn on top, in pieces. A guest instance is single-threaded.
    static TEXT: RefCell<Vec<Piece>> = const { RefCell::new(Vec::new()) };

    /// What the text's placeholders are filled in with.
    static FIELDS: RefCell<Fields> = const {
        RefCell::new(Fields { mode: InputMode::Insert, title: String::new(), tabs: String::new() })
    };

    /// The palette's speed factor.
    static SPEED: Cell<f32> = const { Cell::new(1.0) };

    /// How many frames have passed.
    static FRAME: Cell<u32> = const { Cell::new(0) };

    /// Whether the pane is focused.
    static FOCUS: Cell<Focus> = const { Cell::new(Focus::Unfocused) };

    /// The size of a cell in pixels, width by height, once the host has reported it.
    static CELL: Cell<Option<(u32, u32)>> = const { Cell::new(None) };

    /// Where [`CELL`] comes from.
    static CELL_SOURCE: Cell<CellSource> = const { Cell::new(CellSource::Assumed) };

    /// Whether the cell size has been asked for since the last resize.
    static ASKED: Cell<bool> = const { Cell::new(false) };

    /// The encoded pixels of the last size drawn.
    static PIXELS: RefCell<Option<Pixels>> = const { RefCell::new(None) };

    /// One turn of a sine, [`SINE_STEPS`] steps scaled to [`SINE_AMPLITUDE`]: the pattern is sampled per
    /// column, and an interpreted `sin` per sample would cost a whole frame's budget.
    static SINE: [i32; SINE_STEPS as usize] = std::array::from_fn(|step| {
        let angle = step as f32 / SINE_STEPS as f32 * TAU;
        (angle.sin() * SINE_AMPLITUDE as f32).round() as i32
    });
}

/// @return The sine of @p step steps of a turn of [`SINE_STEPS`], scaled to [`SINE_AMPLITUDE`].
fn sine(step: u32) -> i32 {
    SINE.with(|table| table[(step % SINE_STEPS) as usize])
}

/// @return The palette entry of the pixel at @p x, @p y of an image @p width pixels wide: three sine
///         waves summed, one along the row, one diagonal, and one around the image's top centre.
fn color_at(x: u32, y: u32, width: u32) -> usize {
    let dx = x.abs_diff(width / 2);
    let dy = y * 4;
    // An octagonal approximation of the distance from the centre, close enough for a pattern.
    let distance = dx.max(dy) + dx.min(dy) / 2;
    let value = sine(x) + sine((x + dy) * 3 / 4) + sine(distance * 3 / 2);
    let range = 3 * SINE_AMPLITUDE;
    // value is within [-range, range].
    ((value + range) as usize * COLORS / (2 * range as usize + 1)).min(COLORS - 1)
}

/// Appends @p value to @p out in decimal. The pixels are encoded in runs, thousands of them on a wide
/// surface, and `core::fmt` per run would cost the first frame its deadline in an interpreter.
fn push_number(out: &mut String, value: usize) {
    let mut digits = [0u8; 20];
    let mut start = digits.len();
    let mut rest = value;
    loop {
        start -= 1;
        digits[start] = b'0' + (rest % 10) as u8;
        rest /= 10;
        if rest == 0 {
            break;
        }
    }
    out.extend(digits[start..].iter().map(|&digit| char::from(digit)));
}

/// Appends @p count repetitions of the Sixel character @p sixel to @p out, run-length encoded.
fn push_run(out: &mut String, sixel: char, count: usize) {
    if count > 3 {
        out.push('!');
        push_number(out, count);
        out.push(sixel);
    } else {
        out.extend(std::iter::repeat_n(sixel, count));
    }
}

/// Appends to one colour's row of a band the columns @p start to @p end, after the empty ones since that
/// colour's previous run ended at @p end_of_previous, which becomes @p end.
fn push_band_run(
    row: &mut String,
    end_of_previous: &mut usize,
    filled: char,
    start: usize,
    end: usize,
) {
    push_run(row, '?', start - *end_of_previous);
    push_run(row, filled, end - start);
    *end_of_previous = end;
}

/// @return The Sixel data of an image of @p width by @p height pixels of the plasma pattern, band by band,
///         each colour selected by number from a palette defined beforehand.
///
/// The pattern is sampled every few columns of a band (at least [`SAMPLE_STEP`], and more as the image
/// grows, so that it takes at most [`MAX_SAMPLES`] samples), in the band's middle row, and filled over the
/// band's height: a colour then covers whole runs of columns, and a band costs one sample per step and
/// one Sixel run per change of colour. That is what keeps the first frame of any surface within its
/// call's deadline, in an interpreter.
fn encode_pixels(width: u32, height: u32) -> String {
    let columns = width as usize;
    let bands = height.div_ceil(BAND_HEIGHT);
    let step = SAMPLE_STEP.max((columns * bands as usize).div_ceil(MAX_SAMPLES));
    let mut out = String::new();
    // Per colour: that colour's row of the current band, and the column its last run ended at.
    let mut rows: Vec<String> = vec![String::new(); COLORS];
    let mut ends = [0usize; COLORS];
    for band in 0..bands {
        let top = band * BAND_HEIGHT;
        let filled = char::from(0x3F + ((1u8 << (height - top).min(BAND_HEIGHT)) - 1));
        let sample = top + (height - top).min(BAND_HEIGHT) / 2;
        rows.iter_mut().for_each(String::clear);
        ends.fill(0);
        // Each sample colours the next `step` columns; a run lasts while the samples agree.
        let mut run: Option<(usize, usize)> = None; // colour, first column
        for x in (0..columns).step_by(step) {
            let color = color_at(x as u32, sample, width);
            match run {
                Some((current, _)) if current == color => {}
                Some((current, start)) => {
                    push_band_run(&mut rows[current], &mut ends[current], filled, start, x);
                    run = Some((color, x));
                }
                None => run = Some((color, x)),
            }
        }
        if let Some((current, start)) = run {
            push_band_run(
                &mut rows[current],
                &mut ends[current],
                filled,
                start,
                columns,
            );
        }
        for (color, row) in rows.iter().enumerate().filter(|(_, row)| !row.is_empty()) {
            out.push('#');
            push_number(&mut out, color);
            out.push_str(row);
            out.push('$');
        }
        out.push('-');
    }
    out
}

/// Appends the palette of @p frame to @p out: every entry defined anew, the colour wheel turned on.
fn push_palette(out: &mut String, frame: u32) {
    let phase = frame as f32 * TURNS_PER_FRAME * SPEED.get();
    for color in 0..COLORS {
        let angle = (color as f32 / COLORS as f32 + phase) * TAU;
        let channel =
            |offset: f32| ((0.5 + 0.5 * (angle + offset * TAU).sin()) * BRIGHTNESS) as u32;
        let _ = write!(
            out,
            "#{color};2;{};{};{}",
            channel(0.0),
            channel(1.0 / 3.0),
            channel(2.0 / 3.0)
        );
    }
}

/// @return The answer to the cell size query, width by height, if @p reply holds one.
fn parse_cell_size(reply: &str) -> Option<(u32, u32)> {
    let start = reply.find(CELL_SIZE_REPLY)? + CELL_SIZE_REPLY.len();
    let (fields, _) = reply[start..].split_once('t')?;
    let (height, width) = fields.split_once(';')?;
    let size: (u32, u32) = (width.parse().ok()?, height.parse().ok()?);
    let believable = |extent: u32| (1..=MAX_CELL_PIXELS).contains(&extent);
    (believable(size.0) && believable(size.1)).then_some(size)
}

/// @return The cells @p c takes: 0, 1 or 2.
fn char_width(c: char) -> u32 {
    let code = u32::from(c);
    // Both tables are ascending and disjoint, so the range that can hold `code` is found by its end.
    let within = |ranges: &[(u32, u32)]| {
        let at = ranges.partition_point(|&(_, last)| last < code);
        ranges.get(at).is_some_and(|&(first, _)| first <= code)
    };
    if c.is_control() || within(&ZERO_WIDTH) {
        0
    } else if within(&DOUBLE_WIDTH) {
        2
    } else {
        1
    }
}

/// @return The cells @p text takes.
fn display_width(text: &str) -> u32 {
    text.chars().map(char_width).sum()
}

/// @return @p text split into its literal pieces and its placeholders.
fn parse_text(text: &str) -> Vec<Piece> {
    let mut pieces = Vec::new();
    let mut rest = text;
    while !rest.is_empty() {
        let next = PLACEHOLDERS
            .iter()
            .filter_map(|&(name, field, _)| rest.find(name).map(|at| (at, name, field)))
            .min_by_key(|&(at, _, _)| at);
        match next {
            Some((at, name, field)) => {
                if at > 0 {
                    pieces.push(Piece::Literal(rest[..at].to_string()));
                }
                pieces.push(Piece::Placeholder(field));
                rest = &rest[at + name.len()..];
            }
            None => {
                pieces.push(Piece::Literal(rest.to_string()));
                rest = "";
            }
        }
    }
    pieces
}

/// @return The event kinds that announce changes of what @p pieces name.
fn kinds_named(pieces: &[Piece]) -> EventKinds {
    PLACEHOLDERS
        .iter()
        .filter(|&&(_, field, _)| {
            pieces
                .iter()
                .any(|piece| matches!(piece, Piece::Placeholder(named) if *named == field))
        })
        .fold(EventKinds::empty(), |kinds, &(_, _, kind)| kinds | kind)
}

/// @return @p tabs' titles, the active one in brackets.
fn describe_tabs(tabs: &[Tab]) -> String {
    tabs.iter()
        .map(|tab| {
            if tab.active {
                format!("[{}]", tab.title)
            } else {
                tab.title.clone()
            }
        })
        .collect::<Vec<_>>()
        .join(" ")
}

/// @return The text with its placeholders filled in, control characters dropped.
fn expanded_text() -> String {
    let text = TEXT.with(|pieces| {
        FIELDS.with(|fields| {
            let fields = fields.borrow();
            pieces
                .borrow()
                .iter()
                .map(|piece| match piece {
                    Piece::Literal(text) => text.clone(),
                    Piece::Placeholder(Field::Mode) => MODE_NAMES
                        .iter()
                        .find(|(mode, _)| *mode == fields.mode)
                        .map_or("", |(_, name)| name)
                        .to_string(),
                    Piece::Placeholder(Field::Title) => fields.title.clone(),
                    Piece::Placeholder(Field::Tabs) => fields.tabs.clone(),
                })
                .collect::<String>()
        })
    });
    text.chars().filter(|c| !c.is_control()).collect()
}

/// Appends the text to @p out, centred on the top row of a surface @p cols wide.
fn push_text(out: &mut String, cols: u32) {
    let text = expanded_text();
    if text.is_empty() {
        return;
    }
    let column = cols.saturating_sub(display_width(&text)) / 2 + 1;
    let _ = write!(out, "\x1b[1;{column}H\x1b[1;97m{text}\x1b[m");
}

/// Takes the cell size from @p font, if the host knows it.
/// @return Whether that changed the cell size the image is drawn at.
fn adopt_font(font: FontMetrics) -> bool {
    if font.cell_width_px == 0 || font.cell_height_px == 0 {
        return false;
    }
    let cell = (
        font.cell_width_px.min(MAX_CELL_PIXELS),
        font.cell_height_px.min(MAX_CELL_PIXELS),
    );
    CELL_SOURCE.set(CellSource::Host);
    // Nothing to ask: the host says, and says again when it changes.
    ASKED.set(true);
    CELL.replace(Some(cell)) != Some(cell)
}

/// Asks for the next frame while the pane is focused.
fn animate() {
    if FOCUS.get() == Focus::Focused {
        host::request_frame(FRAME_MS);
    }
}

struct Plasma;

impl Guest for Plasma {
    fn load(config: Vec<(String, String)>) -> EventKinds {
        for (key, value) in config {
            match key.as_str() {
                TEXT_KEY => TEXT.with(|text| *text.borrow_mut() = parse_text(&value)),
                SPEED_KEY => {
                    if let Ok(speed) = value.trim().parse::<f32>() {
                        SPEED.set(speed);
                    }
                }
                _ => {}
            }
        }
        let state = host::state();
        FIELDS.with(|fields| {
            *fields.borrow_mut() = Fields {
                mode: state.input_mode,
                title: state.title,
                tabs: describe_tabs(&state.tabs),
            }
        });
        FOCUS.set(if state.focused {
            Focus::Focused
        } else {
            Focus::Unfocused
        });
        adopt_font(state.font);
        animate();
        EventKinds::FOCUS | EventKinds::FONT | TEXT.with(|pieces| kinds_named(&pieces.borrow()))
    }

    fn update(ev: Event) -> RenderRequest {
        match ev {
            Event::Timer if FOCUS.get() == Focus::Focused => {
                FRAME.set(FRAME.get().wrapping_add(1));
                animate();
                RenderRequest::Render
            }
            Event::Focus(focused) => {
                let was = FOCUS.replace(if focused {
                    Focus::Focused
                } else {
                    Focus::Unfocused
                });
                if focused && was == Focus::Unfocused {
                    animate();
                }
                RenderRequest::None
            }
            Event::FontChanged(font) => {
                if adopt_font(font) {
                    RenderRequest::Render
                } else {
                    RenderRequest::None
                }
            }
            Event::Resized(_) => {
                // A host that tells its font tells a change of cell size, too; otherwise it is asked again.
                if CELL_SOURCE.get() != CellSource::Host {
                    ASKED.set(false);
                }
                RenderRequest::Render
            }
            Event::Reply(bytes) => match parse_cell_size(&String::from_utf8_lossy(&bytes)) {
                Some(cell) if CELL_SOURCE.get() != CellSource::Host && CELL.get() != Some(cell) => {
                    CELL_SOURCE.set(CellSource::Queried);
                    CELL.set(Some(cell));
                    RenderRequest::Render
                }
                _ => RenderRequest::None,
            },
            Event::InputMode(mode) => {
                FIELDS.with(|fields| fields.borrow_mut().mode = mode);
                RenderRequest::Render
            }
            Event::Title(title) => {
                FIELDS.with(|fields| fields.borrow_mut().title = title);
                RenderRequest::Render
            }
            Event::Tabs(tabs) => {
                FIELDS.with(|fields| fields.borrow_mut().tabs = describe_tabs(&tabs));
                RenderRequest::Render
            }
            _ => RenderRequest::None,
        }
    }

    fn render(cols: u32, rows: u32) -> Vec<u8> {
        let (cell_width, cell_height) = CELL.get().unwrap_or(DEFAULT_CELL);
        let width = cols.saturating_mul(cell_width);
        let height = rows
            .saturating_mul(cell_height)
            .min(MAX_BANDS * BAND_HEIGHT);

        let mut out = String::new();
        let _ = write!(out, "\x1bP0;1q\"1;1;{width};{height}");
        push_palette(&mut out, FRAME.get());
        PIXELS.with(|pixels| {
            let mut pixels = pixels.borrow_mut();
            let current = pixels
                .as_ref()
                .is_some_and(|p| p.width == width && p.height == height);
            if !current {
                *pixels = Some(Pixels {
                    width,
                    height,
                    encoded: encode_pixels(width, height),
                });
            }
            if let Some(pixels) = pixels.as_ref() {
                out.push_str(&pixels.encoded);
            }
        });
        out.push_str("\x1b\\");
        push_text(&mut out, cols);
        if !ASKED.replace(true) {
            out.push_str(CELL_SIZE_QUERY);
        }
        out.into_bytes()
    }
}

export!(Plasma);

manifest::declare_manifest!("name = plasma\nversion = 1\n");
