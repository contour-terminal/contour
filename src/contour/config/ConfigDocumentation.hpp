#pragma once

#include <algorithm>
#include <string_view>

namespace contour::config::documentation
{

template <std::size_t N>
struct StringLiteral
{
    constexpr StringLiteral(char const (&str)[N]): value {} { std::copy_n(str, N, value); }

    char value[N]; // NOLINT
};

template <auto T>
struct Wrap
{
};

template <StringLiteral Config, StringLiteral Web>
struct DocumentationEntry
{
};

constexpr StringLiteral Dummy { "{comment} fmt formatted doc {}\n" };

constexpr StringLiteral ShellConfig {
    "{comment} You can override the process to be started inside the terminal.\n"
    "{comment} If nothing is specified, the users' default login shell will be used.\n"
    "{comment} But you may as well log in to a remote host.\n"
    "shell: {}\n"
    "arguments: {}\n"
    "{comment} Sets initial working directory when spawning a new terminal.\n"
    "{comment} A leading ~ is expanded to the user's home directory.\n"
    "{comment} Default value is the user's home directory.\n"
    "initial_working_directory: {}\n"
    "\n"
};

constexpr StringLiteral EscapeSandboxConfig {
    "{comment} If this terminal is being executed from within Flatpak, enforces sandboxing\n"
    "{comment} then this boolean indicates whether or not that sandbox should be escaped or not.\n"
    "{comment}\n"
    "escape_sandbox: {}\n"
    "\n"
};

constexpr StringLiteral SshHostConfigConfig {
    "{comment} Builtin SSH-client configuration.\n"
    "{comment} Use this to directly connect to an SSH server.\n"
    "{comment} This will bypass the local PTY creation\n"
    "{comment} ssh:\n"
    "{comment}     {comment} Target host name to connect to via SSH. This may be a DNS name or IPv4 or "
    "IPv6 address.\n"
    "{comment}     {comment} This value MUST be provided when attempting to directly establish a "
    "connection via SSH.\n"
    "{comment}     {comment}\n"
    "{comment}     {comment} Note, that based on this hostname, the ~/.ssh/config will be looked up and\n"
    "{comment}     {comment} be used as default values when connecting to this host.\n"
    "{comment}     host: example.com\n"
    "{comment}\n"
    "{comment}     {comment} TCP/IP port to use to talk to the remote SSH server. This value defaults to "
    "22.\n"
    "{comment}     port: 22\n"
    "{comment}\n"
    "{comment}     {comment} Remote user name to use for logging into the the SSH server.\n"
    "{comment}     {comment} If not specified, the current local user name will be used as remote SSH "
    "login username.\n"
    "{comment}     user: somebody\n"
    "{comment}\n"
    "{comment}     {comment} When attempting to authenticate with an SSH key, at least the private key "
    "must be provided.\n"
    "{comment}     {comment} This usually is something similar to \"~/.ssh/id_rsa\", but can vary.\n"
    "{comment}     private_key: "
    "\n"
    "{comment}\n"
    "{comment}     {comment} The public key (e.g. \"~/.ssh/your_key.pub\") is usually not required, but "
    "some backends (not OpenSSL) may require it.\n"
    "{comment}     {comment} Defaults to an empty string (not specified).\n"
    "{comment}     public_key: "
    "\n"
    "{comment}\n"
    "{comment}     {comment} This mandates where to look up for known hosts to guard against MITM "
    "attacks.\n"
    "{comment}     {comment} This file is compatible to OpenSSH and thus defaults\n"
    "{comment}     {comment} to the location of OpenSSH's known_hosts, \"~/.ssh/known_hosts\".\n"
    "{comment}     known_hosts: \"~/.ssh/known_hosts\"\n"
    "{comment}\n"
    "{comment}     {comment} Mandates whether or not to enable SSH agent forwarding.\n"
    "{comment}     {comment} Default value currently is `false` (agent forwarding disabled),\n"
    "{comment}     {comment} and is for security reasons also the recommended way.\n"
    "{comment}     forward_agent: false\n"
    "\n"
};

constexpr StringLiteral MaximizedConfig { "{comment} When this profile is *activated*, this flag decides\n"
                                          "{comment} whether or not to put the window into maximized mode.\n"
                                          "maximized: {}"
                                          "\n" };

constexpr StringLiteral FullscreenConfig {
    "{comment} When this profile is being *activated*, this flag decides\n"
    "{comment} whether or not to put the terminal's screen into fullscreen mode.\n"
    "{comment} It is activated during startup as well as when switching from another profile to "
    "this one.\n"
    "fullscreen: {}\n"
    "\n"
};

constexpr StringLiteral ShowTitleBarConfig {
    "{comment} When this profile is *activated*, this flag selects the window decoration:\n"
    "{comment}   true  = the native (server-side) title bar and its window controls;\n"
    "{comment}   false = a frameless window with Contour's own client-side tab strip and controls.\n"
    "{comment} The tab strip is shown either way; only the min/max/close controls move to the OS frame.\n"
    "show_title_bar: {}\n"
    "\n"
};

constexpr StringLiteral WindowShadowConfig {
    "{comment} How far the drop shadow around a frameless window reaches: None, Small, Medium,\n"
    "{comment} Large or VeryLarge. The names and the sizes are Breeze's, so this matches what the\n"
    "{comment} rest of a Plasma desktop is doing.\n"
    "{comment} Only KWin can act on this. A window with `show_title_bar: true` gets the window\n"
    "{comment} manager's own shadow instead, and no other compositor -- GNOME/mutter, sway,\n"
    "{comment} Hyprland -- offers a way for an undecorated window to publish one, so everywhere\n"
    "{comment} else every value behaves as None. Windows and macOS draw their own.\n"
    "window_shadow: {}\n"
    "\n"
};

constexpr StringLiteral DimUnfocusedConfig {
    "{comment} Dims a pane while it is not the focused one — an inactive pane of a split, or any\n"
    "{comment} pane of an unfocused window — by blending it toward its own background color.\n"
    "{comment} The value is the blend amount between 0.0 (dimming disabled; the default) and 1.0\n"
    "{comment} (fully blended to the background color).\n"
    "dim_unfocused: {}\n"
    "\n"
};

constexpr StringLiteral ShowIndicatorOnResizeConfig {
    "{comment} When this profile is *activated*, this flag decides\n"
    "{comment} whether or not the size indicator on resize will be shown.\n"
    "size_indicator_on_resize: {}\n"
    "\n"
};

constexpr StringLiteral MouseConfig { "mouse:\n"
                                      "    {comment} whether or not to hide mouse when typing\n"
                                      "    hide_while_typing: {}\n"
                                      "\n" };

constexpr StringLiteral SearchCaseSensitivityConfig {
    "{comment} How the find bar compares letters when it opens. One of:\n"
    "{comment}   smart       -- case-insensitive until the search term contains a capital (default)\n"
    "{comment}   insensitive -- never case-sensitive\n"
    "{comment}   sensitive   -- always case-sensitive\n"
    "{comment} The bar's \"Aa\" button cycles the three at any time; this is only what it starts as.\n"
    "search_case_sensitivity: {}\n"
    "\n"
};

constexpr StringLiteral InsertAfterYankConfig {
    "{comment} Whether or not to switch from normal mode into insert after yank command. If this value is "
    "set to true,\n"
    "{comment} it will go to insert mode, otherwise it will stay in normal mode.\n"
    "insert_after_yank: {}\n"
    "\n"
};

constexpr StringLiteral CopyLastMarkRangeOffsetConfig {
    "{comment} Advanced value that is useful when CopyPreviousMarkRange is used\n"
    "{comment} with multiline-prompts. This offset value is being added to the\n"
    "{comment} current cursor's line number minus 1 (i.e. the line above the current cursor).\n"
    "copy_last_mark_range_offset: {}\n"
    "\n"
};

constexpr StringLiteral CopyCommandPreserveMultilineConfig {
    "{comment} Opt in to preserving internal line breaks and tabs when using Copy Command.\n"
    "{comment} CR/CRLF become LF; trailing whitespace and unsafe controls are removed.\n"
    "{comment} Pasting multiline text without bracketed paste may execute commands immediately.\n"
    "{comment} The default, false, replaces line breaks and tabs with spaces.\n"
    "copy_command_preserve_multiline: {}\n\n"
};

constexpr StringLiteral CopyCommandPreserveMultilineWeb {
    "Set `copy_command_preserve_multiline: true` to preserve internal line breaks and tabs when using "
    "Copy Command. The default is `false`, which keeps single-line copying. "
    "CR/CRLF are normalized to LF; trailing whitespace is removed. Unsafe controls are filtered in "
    "both modes. Pasting multiline text into a destination without bracketed paste may execute commands "
    "immediately."
};

using CopyCommandPreserveMultiline =
    DocumentationEntry<CopyCommandPreserveMultilineConfig, CopyCommandPreserveMultilineWeb>;

constexpr StringLiteral WMClassConfig {
    "{comment} Defines the class part of the WM_CLASS property of the window.\n"
};

constexpr StringLiteral TabLabelConfig {
    "{comment} Default template for the text shown on each GUI tab.\n"
    "{comment} Supported placeholders:\n"
    "{comment}   {{WindowTitle}}  the active pane's window/tab title\n"
    "{comment}   {{TabPosition}}  the tab's 1-based position\n"
    "{comment} Renaming a tab overrides this default; the rename may itself use the same placeholders.\n"
    "{comment} Split tabs show \"Multiple panes\" instead.\n"
};

constexpr StringLiteral TabColorConfig {
    "{comment} Default color for tabs opened with this profile ('#RRGGBB', or null for no profile color).\n"
    "{comment} Layout, user and application colors override this default.\n"
    "{comment} Split tabs keep the color of the profile that created the tab.\n"
    "tab_color: {}\n"
};

constexpr StringLiteral TabColorWeb {
    "Default tab color as `'#RRGGBB'`, or `null` for no profile color. Layout, user and application "
    "colors override it. Split tabs keep the color of the profile that created the tab.\n"
};

constexpr StringLiteral PixelReportingConfig {
    "{comment} The unit pixel sizes are reported to applications in (ignore-case).\n"
    "{comment} Applications divide a reported extent by the grid to learn the cell size, and size\n"
    "{comment} image canvases (sixel, for one) from it -- so the report is only usable if that\n"
    "{comment} division is exact. The cell is the font's advance in DEVICE pixels:\n"
    "{comment}   Device  = exact; images land 1:1 on the display's own pixels, gapless and sharp,\n"
    "{comment}             at the cost of an application drawing scale-times more pixels per\n"
    "{comment}             axis (default).\n"
    "{comment}   Logical = the canvas size a terminal with an integral logical cell reports, for\n"
    "{comment}             comparing the two on equal terms. At a fractional scale the cell has no\n"
    "{comment}             exact logical form, and a full-page image is letterboxed by a few percent.\n"
    "pixel_reporting: {}\n"
    "\n"
};

constexpr StringLiteral AccessibilityAnnouncementsConfig {
    "{comment} Whether to announce the bell, notifications and mode changes to assistive technology.\n"
    "{comment} Costs nothing while no screen reader is attached.\n"
    "accessibility_announcements: {}\n"
    "\n"
};

constexpr StringLiteral AccessibilityCaretReportingConfig {
    "{comment} Whether the cursor position is reported to assistive technology, so that screen\n"
    "{comment} magnifiers can follow the caret. Costs nothing while no assistive client is attached.\n"
    "accessibility_caret_reporting: {}\n"
    "\n"
};

constexpr StringLiteral HyperlinkHoverTooltipConfig {
    "{comment} Whether hovering an OSC 8 hyperlink shows a tooltip with its target.\n"
    "hyperlink_hover_tooltip: {}\n"
    "\n"
};

constexpr StringLiteral ProgressTimeoutConfig {
    "{comment} How long an application's progress indicator (OSC 9;4) stays on screen without being\n"
    "{comment} updated, in milliseconds. 0 (the default) disables expiry: the indicator persists\n"
    "{comment} until the application clears it, which is what the sequence specifies.\n"
    "progress_timeout: {}\n"
    "\n"
};

constexpr StringLiteral NotificationCloseTimeoutConfig {
    "{comment} How long after showing a desktop notification that asked to hear of its close\n"
    "{comment} (OSC 99 `c=1`) the terminal assumes it was closed, in milliseconds, on desktops that\n"
    "{comment} cannot report a close. A notification that states its own lifetime (OSC 99 `w=`)\n"
    "{comment} wins; 0 disables the assumption, reporting nothing. Any other notification is never\n"
    "{comment} assumed closed: it can still be withdrawn and clicked.\n"
    "notification_close_timeout: {}\n"
    "\n"
};

constexpr StringLiteral TabBarPositionConfig {
    "{comment} Where the GUI tab strip (tab bar) is placed within the window (ignore-case):\n"
    "{comment}   Top    = above the terminal content (default).\n"
    "{comment}   Bottom = below the terminal content.\n"
    "tab_bar_position: {}\n"
    "\n"
};

constexpr StringLiteral TabBarVisibilityConfig {
    "{comment} When the GUI tab strip (tab bar) is shown (ignore-case):\n"
    "{comment}   Always   = always show the tab strip (default).\n"
    "{comment}   Never    = never show the tab strip.\n"
    "{comment}   Multiple = show the tab strip only when the window has more than one tab.\n"
    "tab_bar_visibility: {}\n"
    "\n"
};

constexpr StringLiteral UiStyleConfig {
    "{comment} How the application's own chrome -- tab strip, title bar, menus, popups and the\n"
    "{comment} settings page -- is painted (ignore-case). This selects a shape, not a color: both\n"
    "{comment} styles follow the OS palette and the `theme` setting above.\n"
    "{comment}   native   = platform-native GUI chrome (default).\n"
    "{comment}   terminal = cell-quantized monospace chrome that reads like a TUI.\n"
    "{comment} Takes effect on the next start.\n"
    "ui_style: {}\n"
    "\n"
};

constexpr StringLiteral WindowControlStyleConfig {
    "{comment} How the window's own minimize/maximize/close controls are drawn, and which side of\n"
    "{comment} the tab bar they sit on (ignore-case). Only applies while the native title bar is\n"
    "{comment} hidden (show_title_bar: false); with it shown, the OS draws these itself.\n"
    "{comment}   auto    = match the host: macOS -> macos, Windows -> windows, KDE -> plasma,\n"
    "{comment}             anything else -> windows (default).\n"
    "{comment}   windows = trailing edge, flush rectangular buttons, red close hover.\n"
    "{comment}   macos   = leading edge, traffic lights, close/minimize/zoom order.\n"
    "{comment}   plasma  = trailing edge, circular hover fills (KDE Plasma's Breeze).\n"
    "window_control_style: {}\n"
    "\n"
};

// Quoted, like every other free-text string key (see WordDelimitersConfig below). Unquoted, the
// default empty value emits `ui_font_family:` -- a YAML null the reader cannot convert to a string --
// and a family beginning with a YAML indicator, of which "@MS Gothic" is a real one, would be read as
// something other than itself. The writer escapes `"` and `\` for exactly this context.
constexpr StringLiteral UiFontFamilyConfig {
    "{comment} Font family the terminal-style chrome is drawn with. Empty inherits the running\n"
    "{comment} profile's regular font, which is what makes the chrome match the terminal grid.\n"
    "{comment} Ignored while ui_style is native. Takes effect on the next start.\n"
    "ui_font_family: \"{}\"\n"
    "\n"
};

constexpr StringLiteral UiFontSizeConfig {
    "{comment} Font size, in points, the terminal-style chrome is drawn with. 0 inherits the\n"
    "{comment} running profile's font size. Ignored while ui_style is native.\n"
    "{comment} Takes effect on the next start.\n"
    "ui_font_size: {}\n"
    "\n"
};

constexpr StringLiteral MarginsConfig {
    "{comment} Window margins\n"
    "{comment}\n"
    "{comment} The margin values are applied on both sides and are given in pixels\n"
    "{comment} with DPI yet to be applied to these values.\n"
    "margins:\n"
    "    {comment} Horizontal (left/right) margins.\n"
    "    horizontal: {}\n"
    "    {comment} Vertical (top/bottom) margins.\n"
    "    vertical: {}\n"
    "\n"
};

constexpr StringLiteral TerminalSizeConfig { "{comment}Determines the initial terminal size in  characters\n"
                                             "terminal_size:\n"
                                             "    columns: {}\n"
                                             "    lines: {}\n"
                                             "\n" };

constexpr StringLiteral TerminalIdConfig {
    "{comment} Determines the terminal type that is being advertised.\n"
    "{comment} Possible values are:\n"
    "{comment}   - VT100\n"
    "{comment}   - VT220\n"
    "{comment}   - VT240\n"
    "{comment}   - VT330\n"
    "{comment}   - VT340\n"
    "{comment}   - VT320\n"
    "{comment}   - VT420\n"
    "{comment}   - VT510\n"
    "{comment}   - VT520\n"
    "{comment}   - VT525\n"
    "terminal_id: {}\n"
    "\n"
};

constexpr StringLiteral MaxHistoryLineCountConfig {
    "{comment} Number of lines to preserve (-1 for infinite).\n"
    "limit: {}\n"
    "\n"
};

constexpr StringLiteral HistoryScrollMultiplierConfig {
    "{comment} Number of lines to scroll on ScrollUp & ScrollDown events.\n"
    "scroll_multiplier: {}\n"
    "\n"
};

constexpr StringLiteral HistoryConfig {

    "history:\n"
    "    {comment} Number of lines to preserve (-1 for infinite).\n"
    "    limit: {}\n"
    "    {comment} Upper bound on the scrollback (-1 for infinite). Above `limit` it becomes headroom,\n"
    "    {comment} within which lines are evicted a whole command at a time rather than wherever the\n"
    "    {comment} limit falls -- so the oldest line kept is a prompt, with its output intact. Needs a\n"
    "    {comment} shell that emits OSC 133; without one the buffer simply bounds here.\n"
    "    hard_limit: {}\n"
    "    {comment} Whether new output lines bring a view scrolled back into the history down to the\n"
    "    {comment} bottom. Output repainted in place does not; typing always does.\n"
    "    auto_scroll_on_update: {}\n"
    "    {comment} Number of lines to scroll on ScrollUp & ScrollDown events.\n"
    "    scroll_multiplier: {}\n"
    "\n"

};

constexpr StringLiteral FoldingConfig {

    "folding:\n"
    "    {comment} Whether a finished command's output can be collapsed to its prompt line.\n"
    "    {comment} Requires shell integration: the fold ranges come from the OSC 133 marks a\n"
    "    {comment} shell leaves behind, so a shell that emits none has nothing to fold.\n"
    "    enabled: {}\n"
    "    {comment} Whether the gutter's block column, left of the grid, draws a marker on every\n"
    "    {comment} foldable prompt line. The column costs one column of terminal width, and is\n"
    "    {comment} reserved while this, gutter.exit_status or gutter.user_marks is on.\n"
    "    show_markers: {}\n"
    "    {comment} Whether to collapse the previous command automatically when a new prompt starts.\n"
    "    auto_collapse_on_new_command: {}\n"
    "    {comment} What a Vi-mode jump that targets a line inside a collapsed block does:\n"
    "    {comment}   expand - open the block so the cursor lands on what it was aiming at\n"
    "    {comment}   skip   - leave the block collapsed and stop at its prompt line instead\n"
    "    {comment} Plain motions (j, k, {{, }}, Ctrl-D, Ctrl-U) always step over a collapsed block\n"
    "    {comment} regardless of this setting.\n"
    "    on_jump_into_fold: {}\n"
    "\n"

};

constexpr StringLiteral GutterConfig {

    "gutter:\n"
    "    {comment} The strip left of the grid. Its width follows this configuration and never the\n"
    "    {comment} content, so a growing line number cannot resize the page mid-command. The block\n"
    "    {comment} column beside the grid is reserved while fold markers (folding.show_markers), exit\n"
    "    {comment} status or user marks are on, block_column_width cells wide.\n"
    "    {comment} Colour each command's fold controls by how it ended, draw a triangle beside a running\n"
    "    {comment} command, and a dot beside a finished one that shows no fold control -- one with nothing\n"
    "    {comment} to fold, or any one while fold markers are off. A failed one shows a cross instead of\n"
    "    {comment} the dot, and its fold's line is drawn heavy.\n"
    "    exit_status: {}\n"
    "    {comment} Draw a diamond beside every row marked with vi-mode mm.\n"
    "    user_marks: {}\n"
    "    {comment} The block column's width in cells, 1 or 2. At 2 its fold controls and marks are drawn\n"
    "    {comment} across both cells, twice the size, and a click anywhere in the column lands.\n"
    "    block_column_width: {}\n"
    "    {comment} Draw the fold under the pointer -- its line, its box and its end -- one stroke heavier, "
    "as\n"
    "    {comment} well as at full strength. Its colours are the color scheme's fold_marker.hover and\n"
    "    {comment} block_status_hover.\n"
    "    bold_on_hover: {}\n"
    "    {comment} Line numbers, as in vim:\n"
    "    {comment}   off      - none\n"
    "    {comment}   absolute - every row's number, counted from the session's first row (nu)\n"
    "    {comment}   relative - every row's distance from the cursor row (rnu)\n"
    "    {comment}   hybrid   - the cursor row's own number, every other row's distance (nu rnu)\n"
    "    line_numbers: {}\n"
    "    {comment} Columns the line numbers take, 3 to 10. A longer number keeps its last digits,\n"
    "    {comment} behind an ellipsis.\n"
    "    line_number_width: {}\n"
    "    {comment} Show the time each row was first reached, wherever it differs from the row above.\n"
    "    timestamps: {}\n"
    "    {comment} A std::format chrono specification in local time, such as '%H:%M' or '%a %H:%M:%S'.\n"
    "    {comment} It must begin with a % conversion -- '[%H:%M]', '8%H' and '>%H' are refused -- and\n"
    "    {comment} every character it spells must take exactly one column, so a double-width (CJK) or a\n"
    "    {comment} combining character is refused too. A refused format is reported and the default kept.\n"
    "    timestamp_format: {}\n"
    "\n"

};

constexpr StringLiteral OscContextConfig {

    "osc_context:\n"
    "    {comment} Whether to read OSC 3008, systemd's hierarchical context signalling (UAPI.15).\n"
    "    {comment} systemd 258 and later emit these around every interactive command with no\n"
    "    {comment} configuration at all, so on those systems the terminal learns command\n"
    "    {comment} boundaries, exit statuses and working directories for free.\n"
    "    {comment} Set to false to restore the behaviour from before this was consumed.\n"
    "    enabled: {}\n"
    "    {comment} How deep the context stack may grow before further contexts are ignored.\n"
    "    {comment} Per the specification the NEWER contexts are dropped, so a program cannot\n"
    "    {comment} push the elevate context above it out of the stack.\n"
    "    max_depth: {}\n"
    "    {comment} How many contexts are remembered for scrolled-back output that refers to them.\n"
    "    max_retained_contexts: {}\n"
    "    {comment} Whether OSC 3008 may stand in for a shell integration that is not installed:\n"
    "    {comment}   when_alone - derive marks from OSC 3008 only while OSC 133 has said nothing\n"
    "    {comment}   never      - only OSC 133 ever leaves marks\n"
    "    {comment} Once a shell integration speaks, it owns the marks for the rest of the session.\n"
    "    derive_markers: {}\n"
    "    {comment} Which contexts may tint the page background. The COLOURS come from the colour\n"
    "    {comment} scheme's tint: map, which no shipped scheme sets -- so nothing is tinted until\n"
    "    {comment} you choose a colour there. A context is something the session ANNOUNCED, and\n"
    "    {comment} anything that can write to your terminal can announce one, or fail to.\n"
    "    {comment}   boundaries - only elevate, chpriv, container, vm and remote\n"
    "    {comment}   all        - every type the colour scheme sets a tint for\n"
    "    {comment}   off        - ignore the tint map entirely\n"
    "    tinting: {}\n"
    "\n"

};

constexpr StringLiteral CommandBlocksConfig {

    "command_blocks:\n"
    "    {comment} How many shell commands one session remembers: the command line, its exit\n"
    "    {comment} status, how long it ran, where and when -- even once its output has scrolled\n"
    "    {comment} away. The oldest are forgotten first. Needs a shell that emits OSC 133 (or\n"
    "    {comment} systemd's OSC 3008); without one nothing is recorded. At least 1, at most 1000000.\n"
    "    {comment} A change applies to sessions opened afterwards.\n"
    "    max_records: {}\n"
    "    {comment} The program \"Open Output in Pager\" (the OpenCommandOutput action) runs when its\n"
    "    {comment} binding names none. Read as a shell: setting is: a string naming a program runs as\n"
    "    {comment} it is; otherwise its first word is the program and the rest its arguments -- on\n"
    "    {comment} Windows passed on as written, so C:\\Tools\\less.exe -R needs no quoting. The file\n"
    "    {comment} holding the output is appended as its last argument.\n"
    "    pager: {}\n"
    "    {comment} Whether that pager renders colours: sgr keeps them in the file it is given, plain\n"
    "    {comment} writes text only. A binding's own format: wins; a binding's own program: gets sgr.\n"
    "    pager_format: {}\n"
    "\n"

};

constexpr StringLiteral ScrollbarConfig {

    "scrollbar:\n"
    "    {comment} scroll bar position: Left, Right (the default), Hidden (ignore-case).\n"
    "    {comment} Hidden also hides the marks below. Right is the default for new configurations\n"
    "    {comment} only: a contour.yml written by an earlier version keeps Hidden, so set Right there\n"
    "    {comment} to see the marks.\n"
    "    position: {}\n"
    "    {comment} whether or not to hide the scrollbar when in alt-screen.\n"
    "    hide_in_alt_screen: {}\n"
    "    {comment} Which marks the scrollbar shows as ticks; clicking one scrolls its line to the top.\n"
    "    {comment} A list of:\n"
    "    {comment}   failures   - commands that exited with an error\n"
    "    {comment}   commands   - every other command, finished or still running\n"
    "    {comment}   user_marks - lines marked with `mm` in vi normal mode\n"
    "    {comment} [] shows none. The first two need a shell that emits OSC 133 (or systemd's\n"
    "    {comment} OSC 3008).\n"
    "    marks: {}\n"
    "\n"

};

constexpr StringLiteral NotifyOnCommandFinishConfig {

    "notify_on_command_finish:\n"
    "    {comment} Tells you when a command finishes. Needs shell integration (OSC 133): a shell\n"
    "    {comment} without it reports no commands, so nothing is ever notified.\n"
    "    {comment} When to notify, by where the pane is:\n"
    "    {comment}   never     - never\n"
    "    {comment}   unfocused - unless it is the focused pane of the active window\n"
    "    {comment}   hidden    - only while it is off screen (another tab, a zoomed sibling,\n"
    "    {comment}               a minimised window)\n"
    "    {comment}   always    - every time\n"
    "    when: {}\n"
    "    {comment} Commands that ran for fewer seconds than this never notify.\n"
    "    min_duration: {}\n"
    "    {comment} Which commands notify: any, or failure (a non-zero exit or a signal).\n"
    "    outcome: {}\n"
    "    {comment} How: notify (a desktop notification), bell (the profile's bell sound and\n"
    "    {comment} window alert), or notify_bell (both).\n"
    "    action: {}\n"
    "    {comment} When a notification still on screen is withdrawn: focus (the pane is\n"
    "    {comment} focused), next (the pane's next command starts), or never. Only a desktop\n"
    "    {comment} notification can be withdrawn (Linux); a tray message elsewhere stays until\n"
    "    {comment} it times out, whatever this says.\n"
    "    clear_on: {}\n"
    "\n"

};

constexpr StringLiteral StickyHeaderConfig {

    "sticky_header:\n"
    "    {comment} Pins the command line of the block you are reading over the top row once that\n"
    "    {comment} command has scrolled out of view above its own output. Needs prompt marks\n"
    "    {comment} (OSC 133;A): a shell that marks only where its commands run (;C, ;D) gets none.\n"
    "    {comment}   never    - never, but for show_evicted below\n"
    "    {comment}   scrolled - only while the viewport is scrolled back into history\n"
    "    {comment}   always   - also while following live output\n"
    "    mode: {}\n"
    "    {comment} Whether the header still names a command whose own line has already been dropped\n"
    "    {comment} from the scrollback (\"earlier output evicted\"): at the very top of the scrollback\n"
    "    {comment} whatever mode says, and anywhere else wherever mode would show a header.\n"
    "    show_evicted: {}\n"
    "\n"

};

constexpr StringLiteral AutoScrollOnUpdateConfig {
    "{comment} Boolean indicating whether or not to scroll down to the bottom on screen updates.\n"
    "auto_scroll_on_update: {}\n"
    "\n"
};

constexpr StringLiteral FontsConfig {
    "{comment} Font related configuration (font face, styles, size, rendering mode).\n"
    "font:\n"
    "    {comment} Initial font size in pixels.\n"
    "    size: {}\n"
    "\n"
    "    {comment} Font Locator API\n"
    "    {comment} Selects an engine to use for locating font files on the system.\n"
    "    {comment} This is implicitly also responsible for font fallback\n"
    "    {comment} Possible values are:\n"
    "    {comment} - native          : automatically choose the best available on the current platform\n"
    "    {comment} - mock            : mock font locator engine (not recommended for general use)\n"
    "    locator: {}\n"
    "\n"
    "    {comment} Text shaping related settings\n"
    "    text_shaping:\n"
    "        {comment} Selects which text shaping and font rendering engine to use.\n"
    "        {comment} Supported values are:\n"
    "        {comment} - native      : automatically choose the best available on the current platform.\n"
    "        {comment} - DirectWrite : selects DirectWrite engine (Windows only)\n"
    "        {comment} - CoreText    : selects CoreText engine (Mac OS/X only) (currently not "
    "implemented)\n"
    "        {comment} - OpenShaper  : selects OpenShaper (harfbuzz/freetype/fontconfig, available on "
    "all\n"
    "        {comment}                 platforms)\n"
    "        engine: {}\n"
    "\n"
    "    {comment} Uses builtin textures for pixel-perfect box drawing.\n"
    "    {comment} If disabled, the font's provided box drawing characters\n"
    "    {comment} will be used (Default: true).\n"
    "    builtin_box_drawing: {}\n"
    "\n"
    "    {comment} Maximum number of fallback fonts per font style.\n"
    "    {comment} Set to -1 for unlimited, 0 to disable fallbacks entirely.\n"
    "    {comment} Default: 16\n"
    "    max_fallback_count: {}\n"
    "\n"
    "    {comment} Font render modes tell the font rasterizer engine what rendering technique to use.\n"
    "    {comment}\n"
    "    {comment} Modes available are:\n"
    "    {comment} - lcd          Uses a subpixel rendering technique optimized for LCD displays.\n"
    "    {comment} - light        Uses a subpixel rendering technique in gray-scale.\n"
    "    {comment} - gray         Uses standard gray-scaled anti-aliasing.\n"
    "    {comment} - monochrome   Uses pixel-perfect bitmap rendering (also spelled: bitmap).\n"
    "    {comment} - color        Prefers the font's embedded color bitmaps.\n"
    "    render_mode: {}\n"
    "\n"
    "    {comment} Indicates whether or not to include *only* monospace fonts in the font and\n"
    "    {comment} font-fallback list (Default: false).\n"
    "    strict_spacing: {}\n"
    "\n"
    "    {comment} Font family to use for displaying text.\n"
    "    {comment}\n"
    "    {comment} A font can be either described in detail as below or as a\n"
    "    {comment} simple string value (e.g. \"monospace\" with the appropriate\n"
    "    {comment} weight/slant applied automatically).\n"
    "    regular:\n"
    "        {comment} Font family defines the font family name, such as:\n"
    "        {comment} \"\"Fira Code\", \"Courier New\", or \"monospace\" (default).\n"
    "        family: {}\n"
    "\n"
    "        {comment} Font weight can be one of:\n"
    "        {comment}   thin, extra_light, light, demilight, book, normal,\n"
    "        {comment}   medium, demibold, bold, extra_bold, black, extra_black.\n"
    "        weight: {}\n"
    "\n"
    "        {comment} Font slant can be one of: normal, italic, oblique.\n"
    "        slant: {}\n"
    "\n"
    "        {comment} Set of optional font features to be enabled. This\n"
    "        {comment} is usually a 4-letter code, such as ss01 or ss02 etc.\n"
    "        {comment}\n"
    "        {comment} Please see your font's documentation to find out what it\n"
    "        {comment} supports.\n"
    "        {comment}\n"
    "        features: {}\n"
    "\n"
    "        {comment} Customize fallback to use when rendering with the font\n"
    "        {comment} to disable any font fallback specify\n"
    "        {comment}fallback: none\n"
    "        {comment}\n"
    "        {comment} To specify a list of fallback fonts, use an array of strings.\n"
    "        {comment}fallback: [\"First\", \"Second\"]\n"
    "\n"
    "    {comment} If bold/italic/bold_italic are not explicitly specified, the regular font with\n"
    "    {comment} the respective weight and slant will be used.\n"
    "    {comment}bold: \"monospace\"\n"
    "    {comment}italic: \"monospace\"\n"
    "    {comment}bold_italic: \"monospace\"\n"
    "\n"
    "    {comment} This is a special font to be used for displaying unicode symbols\n"
    "    {comment} that are to be rendered in emoji presentation.\n"
    "    emoji: {}\n"
    "\n"
    "    {comment} Text outline draws a colored stroke around each text glyph to improve\n"
    "    {comment} readability on transparent or low-opacity backgrounds.\n"
    "    {comment}\n"
    "    {comment} Set to a number for simple thickness (e.g. text_outline: 1.0),\n"
    "    {comment} or use a map for thickness and color:\n"
    "    {comment}   text_outline:\n"
    "    {comment}     thickness: 1.0\n"
    "    {comment}     color: \"#000000\"\n"
    "    {comment}\n"
    "    {comment} Default: disabled (0)\n"
    "    {comment} text_outline: 0\n"
    "\n"
};

constexpr StringLiteral PermissionsConfig {

    "\n"
    "{comment} Some VT sequences should need access permissions.\n"
    "{comment}\n"
    "{comment}  These can be to:\n"
    "{comment}  - allow     Allows the given functionality\n"
    "{comment}  - deny      Denies the given functionality\n"
    "{comment}  - ask       Asks the user interactively via popup dialog for "
    "permission of the given action.\n"
    "permissions:\n"
    "    {comment} Allows capturing the screen buffer via `CSI > Pm ; Ps ; Pc ST`.\n"
    "    {comment} The response can be read from stdin as sequence `OSC 314 ; <screen capture> ST`\n"
    "    capture_buffer: {}\n"
    "    {comment} Allows changing the font via `OSC 50 ; Pt ST`.\n"
    "    change_font: {}\n"
    "    {comment} Allows displaying the \" Host Writable Statusline \" programmatically using `DECSSDT 2`.\n"
    "    display_host_writable_statusline: {}\n"
    "\n"
};

constexpr StringLiteral DrawBoldTextWithBrightColorsConfig {
    "{comment} Indicates whether or not bold text should be rendered in bright colors,\n"
    "{comment} for indexed colors.\n"
    "{comment} If disabled, normal color will be used instead.\n"
    "draw_bold_text_with_bright_colors: {}\n"
    "\n"
};

constexpr StringLiteral ColorsConfig {
    "{comment} Specifies a colorscheme to use (alternatively the colors can be inlined).\n"
    "{comment} Or choose from existing default palettes:\n"
    "{comment} contour, monokai, one-dark, one-light, gruvbox-light, gruvbox-dark,\n"
    "{comment} solarized-light, solarized-dark, papercolor-light, papercolor-dark.\n"
    "{comment}\n"
    "{comment} This can be either the name to a single colorscheme to always use,\n"
    "{comment} or a map with two keys (dark and light) to determine the color scheme to use for each.\n"
    "{comment}\n"
    "{comment} The dark color scheme is used when the system is configured to prefer dark mode and light "
    "theme otherwise.\n"
    "{comment}\n"
    "{comment} A configuration reload applies the colors to running sessions too, replacing any a\n"
    "{comment} program set there (OSC 4, 10, 11), just as switching to another profile does.\n"
    "\n"
    "colors: {}\n"
};

constexpr StringLiteral ModalCursorScrollOffConfig {
    "{comment} Configures a `scrolloff` for cursor movements in normal and visual (block) modes.\n"
    "{comment}\n"
    "vi_mode_scrolloff: {}\n"
    "\n"
};

constexpr StringLiteral ModeInsertConfig {
    "{comment} Terminal cursor display configuration\n"
    "cursor:\n"
    "    {comment} Supported shapes are:\n"
    "    {comment}\n"
    "    {comment} - block         a filled rectangle\n"
    "    {comment} - rectangle     just the outline of a block\n"
    "    {comment} - underscore    a line under the text\n"
    "    {comment} - bar:          the well known i-Beam\n"
    "    shape: {}\n"
    "    {comment} Determines whether or not the cursor will be blinking over time.\n"
    "    blinking: {}\n"
    "    {comment} Blinking interval (in milliseconds) to use when cursor is blinking.\n"
    "    blinking_interval: {}\n"
    "\n"
};

constexpr StringLiteral ModeNormalConfig { "{comment} vi-like normal-mode specific settings.\n"
                                           "{comment} Note, currently only the cursor can be customized.\n"
                                           "normal_mode:\n"
                                           "    cursor:\n"
                                           "        shape: {}\n"
                                           "        blinking: {}\n"
                                           "        blinking_interval: {}\n"
                                           "\n" };

constexpr StringLiteral ModeVisualConfig { "{comment} vi-like normal-mode specific settings.\n"
                                           "{comment} Note, currently only the cursor can be customized.\n"
                                           "visual_mode:\n"
                                           "    cursor:\n"
                                           "        shape: {}\n"
                                           "        blinking: {}\n"
                                           "        blinking_interval: {}\n"
                                           "\n" };

constexpr StringLiteral SmoothLineScrollingConfig {
    "{comment} Defines the number of milliseconds to wait before\n"
    "{comment} actually executing the LF (linefeed) control code\n"
    "{comment} in case DEC mode `DECSCLM` is enabled.\n"
    "slow_scrolling_time: {}\n"
    "\n"
};

constexpr StringLiteral SmoothScrollingConfig {
    "{comment} Enables pixel-based smooth scrolling.\n"
    "{comment} When enabled, scrolling will be performed in pixel increments\n"
    "{comment} rather than jumping by full lines, providing a smoother visual experience.\n"
    "{comment} This applies to mouse wheel and trackpad scrolling on the primary screen.\n"
    "smooth_scrolling: {}\n"
    "\n"
};

constexpr StringLiteral MomentumScrollingConfig {
    "{comment} Enables momentum (inertia) scrolling for touchpad gestures.\n"
    "{comment} When enabled, lifting your finger from the touchpad after a swipe\n"
    "{comment} will cause the terminal to continue scrolling with decelerating velocity.\n"
    "{comment} This governs touchpad finger-lift inertia only. Mouse-wheel smoothness is\n"
    "{comment} controlled by `smooth_scrolling` (which also glides wheel notches).\n"
    "momentum_scrolling: {}\n"
    "\n"
};

constexpr StringLiteral HighlightTimeoutConfig {
    "{comment} Time duration in milliseconds for which yank highlight is shown.\n"
    "vi_mode_highlight_timeout: {}\n"
    "\n"
};

constexpr StringLiteral InputMethodEditorSupportConfig {
    "{comment} Enables Input Method Editor (IME) support.\n"
    "{comment} This is enabled by default.\n"
    "input_method_editor: {}\n"
    "\n"
};

constexpr StringLiteral HighlightDoubleClickerWordConfig {
    "{comment} If enabled, and you double-click on a word in the primary screen,\n"
    "{comment} all other words matching this word will be highlighted as well.\n"
    "{comment} So the double-clicked word will be selected as well as highlighted, along with\n"
    "{comment} all other words being simply highlighted.\n"
    "{comment}\n"
    "{comment} This is currently implemented by initiating a search on the double-clicked word.\n"
    "{comment} Therefore one can even use FocusNextSearchMatch and FocusPreviousSearchMatch to\n"
    "{comment} jump to the next/previous same word, also outside of the current viewport.\n"
    "{comment}\n"
    "highlight_word_and_matches_on_double_click: {}\n"
    "\n"
};

constexpr StringLiteral StatusLineConfig {
    "status_line:\n"
    "    {comment} Either none or indicator.\n"
    "    {comment} This only reflects the initial state of the status line, as it can\n"
    "    {comment} be changed at any time during runtime by the user or by an application.\n"
    "    display: {}\n"
    "    {comment} Position to place the status line to, if it is to be shown.\n"
    "    {comment} This can be either value `top` or value `bottom`.\n"
    "    position: {}\n"
    "    {comment} Synchronize the window title with the Host Writable status_line if\n"
    "    {comment} and only if the host writable status line was denied to be shown.\n"
    "    sync_to_window_title: {}\n"
    "    {comment} Each segment is a template of literal text and {{Placeholder}} markers (e.g.\n"
    "    {comment} {{Clock}}, {{VTType}}). There is no brace escaping: any {{...}} that is not a\n"
    "    {comment} recognized placeholder is echoed verbatim, so you see exactly what you typed.\n"
    "    indicator:\n"
    "        left: \"{}\"\n"
    "        middle: \"{}\"\n"
    "        right: \"{}\"\n"
    "    {comment} Name of an entry of `extensions` that takes the whole status line over, or null to keep "
    "the\n"
    "    {comment} template above. Segments of the template may still draw other extensions with\n"
    "    {comment} {{Extension:<name>}}.\n"
    "    extension: {}\n"
    "\n"
};

constexpr StringLiteral BackgroundConfig {
    "background:\n"
    "    {comment} Background opacity to use. A value of 1.0 means fully opaque whereas 0.0 means fully\n"
    "    {comment} transparent. Only values between 0.0 and 1.0 are allowed.\n"
    "    opacity: {}\n"
    "    {comment} Some platforms can blur the transparent background (currently only Windows is "
    "    supported).\n"
    "    blur: {}\n"
    "\n"
};

constexpr StringLiteral BellConfig {
    "\n"
    "bell:\n"
    "    {comment} There is no sound for BEL character if set to \"off\".\n"
    "    {comment} If set to \" default \" BEL character sound will be default sound.\n"
    "    {comment} If set to path to a file then BEL sound will "
    "use that file. Example\n"
    "    {comment}   sound: \"/home/user/Music/bell.wav\"\n"
    "    sound: {}\n"
    "\n"
    "    {comment} Bell volume, a normalized value between 0.0 (silent) and 1.0 (loudest).\n"
    "    volume: {}\n"
    "\n"
    "    {comment} If this boolean is true, a window alert will "
    "be raised with each bell\n"
    "    alert: true\n"
    "\n"
};

constexpr StringLiteral FrozenDecModeConfig {
    "{comment} Defines a list of DEC modes to explicitly and permanently disable/enable support for.\n"
    "{comment}\n"
    "{comment} This is a developer-users-only option that may possibly help investigating problems.\n"
    "{comment} This option may also be used by regular users if they're asked to in order to disable\n"
    "{comment} a broken functionality. This is something we hardly try to avoid, of course.\n"
    "{comment}\n"
    "{comment} This can be an object with each key being the DEC mode number and its value a boolean,\n"
    "{comment} indicating whether this DEC mode is permanently set (enabled) or unset (disabled).\n"
    "{comment} A change applies to sessions opened afterwards.\n"
    "{comment}\n"
    "{comment} Example:\n"
    "{comment}\n"
    "{comment}     frozen_dec_modes:\n"
    "{comment}         2026: false\n"
    "{comment}         2027: true\n"
    "{comment}\n"
    "{comment}frozen_dec_modes:\n"
    "\n"
};

constexpr StringLiteral LiveConfig {
    "{comment} Determines whether the instance is reloading the configuration files "
    "whenever it is changing or not.\n"
    "live_config: {}\n"
    "\n"
};

constexpr StringLiteral PlatformPluginConfig {
    "{comment} Overrides the auto-detected platform plugin to be loaded.\n"
    "{comment}\n"
    "{comment} Possible (incomplete list of) values are:\n"
    "{comment} - auto        The platform will be auto-detected.\n"
    "{comment} - xcb         Uses XCB plugin (for X11 environment).\n"
    "{comment} - cocoa       Used to be run on Mac OS/X.\n"
    "{comment} - direct2d    Windows platform plugin using Direct2D.\n"
    "{comment} - winrt       Windows platform plugin using WinRT.\n"
    "platform_plugin: {}\n"
    "\n"
};

constexpr StringLiteral RendererConfig {
    "renderer:\n"
    "    {comment} Qt RHI graphics API used to render the terminal onto the screen.\n"
    "    {comment} Possible values are:\n"
    "    {comment} - auto        Native backend per platform (Direct3D 11/Windows, Metal/macOS, "
    "OpenGL/Linux).\n"
    "    {comment} - OpenGL      Force the OpenGL backend on every platform.\n"
    "    {comment} - vulkan      Force the Vulkan backend (Windows and Linux).\n"
    "    {comment} - direct3d11  Force the Direct3D 11 backend (Windows only).\n"
    "    {comment} - direct3d12  Force the Direct3D 12 backend (Windows only).\n"
    "    {comment} - metal       Force the Metal backend (macOS only).\n"
    "    {comment} - software    Force a software-emulated OpenGL rasterizer.\n"
    "    backend: {}\n"
    "\n"
    "    {comment} Which GPU renders the terminal. Takes effect at the next start.\n"
    "    {comment} - auto        The system's default GPU (Contour does not intervene).\n"
    "    {comment} - integrated  The integrated GPU, if present.\n"
    "    {comment} - discrete    The discrete GPU, if present; otherwise the integrated one.\n"
    "    {comment} - vvvv:dddd   A specific GPU by PCI vendor:device id, e.g. 10de:2820.\n"
    "    {comment} Vulkan and Direct3D honour this on Linux and Windows; OpenGL on Linux only.\n"
    "    gpu: {}\n"
    "\n"
    "    {comment} Enables/disables the use of direct-mapped texture atlas tiles for\n"
    "    {comment} the most often used ones (US-ASCII, cursor shapes, underline styles)\n"
    "    {comment} You most likely do not want to touch this.\n"
    "    {comment}\n"
    "    tile_direct_mapping: {}\n"
    "\n"
    "    {comment} Number of hashtable slots to map to the texture tiles.\n"
    "    {comment} Larger values may increase performance, but too large may also decrease.\n"
    "    {comment} This value is rounded up to a value equal to the power of two.\n"
    "    {comment}\n"
    "    tile_hashtable_slots: {}\n"
    "\n"
    "    {comment} Number of tiles that must fit at lest into the texture atlas.\n"
    "    {comment}\n"
    "    {comment} This does not include direct mapped tiles (US-ASCII glyphs,\n"
    "    {comment} cursor shapes and decorations), if tile_direct_mapping is set to true).\n"
    "    {comment}\n"
    "    {comment} Value must be at least as large as grid cells available in the terminal view.\n"
    "    {comment} This value is automatically adjusted if too small.\n"
    "    {comment}\n"
    "    tile_cache_count: {}\n"
    "\n"
};

constexpr StringLiteral CommandPaletteRecentCountConfig {
    "{comment} How many recently used commands the command palette (OpenCommandPalette, bound to\n"
    "{comment} Ctrl+Shift+P by default) pins above its alphabetical list.\n"
    "{comment}\n"
    "{comment} The list is remembered across restarts in the `command-history.yml` file next to this\n"
    "{comment} one. Set to 0 to disable the \"recently used\" section (and stop recording it).\n"
    "command_palette_recent_count: {}\n"
    "\n"
};

constexpr StringLiteral PTYReadBufferSizeConfig {
    "{comment} The most bytes taken from the PTY in one read, and so the most parsed in one go.\n"
    "{comment}\n"
    "{comment} Larger batches parse output faster; smaller ones show a flood of it in smaller steps.\n"
    "{comment} This is an advanced option. Use with care!\n"
    "{comment} A change applies to sessions opened afterwards.\n"
    "read_buffer_size: {}\n"
    "\n"
};

constexpr StringLiteral PTYBufferObjectSizeConfig {
    "{comment} Size in bytes per PTY Buffer Object.\n"
    "{comment}\n"
    "{comment} This is an advanced option of an internal storage. Only change with care!\n"
    "{comment} A change applies to sessions opened afterwards.\n"
    "pty_buffer_size: {}\n"
    "\n"
};

constexpr StringLiteral ReflowOnResizeConfig {
    "\n"
    "{comment} Whether or not to reflow the lines on terminal resize events.\n"
    "{comment} A change applies to sessions opened afterwards.\n"
    "reflow_on_resize: {}\n"
};

constexpr StringLiteral SnapToRowsConfig {
    "\n"
    "{comment} Align startup and manually resized window heights to complete terminal rows.\n"
    "{comment} Applies to a single visible pane, including a zoomed split pane.\n"
    "{comment} Windows snaps during resizing; macOS snaps when resizing ends; X11 uses WM hints.\n"
    "{comment} Wayland supports startup sizing only. Maximized/fullscreen sizes are unrestricted.\n"
    "snap_to_rows: {}\n"
};

constexpr StringLiteral RememberWindowPositionConfig {
    "\n"
    "{comment} Remember the normal window position and monitor, without saving window size.\n"
    "{comment} Enabled by default on Windows; opt in on macOS and X11. Wayland controls placement.\n"
    "{comment} Restoration applies to the first window at startup.\n"
    "{comment} Saves monitor-relative x/y and a zero-based monitor index in window-position.yml.\n"
    "remember_window_position: {}\n"
};

constexpr StringLiteral TabSwitchOnHorizontalWheelConfig {
    "\n"
    "{comment} Whether a horizontal mouse wheel tilt (or a horizontal trackpad swipe) over the tab\n"
    "{comment} strip or the terminal switches to the previous/next tab.\n"
    "{comment}\n"
    "{comment} An application that has asked for mouse reporting (vim, tmux, ...) keeps receiving the\n"
    "{comment} horizontal wheel; hold the bypass modifier (see bypass_mouse_protocol_modifiers) to\n"
    "{comment} switch tabs anyway. An explicit WheelLeft/WheelRight entry in input_mapping wins over\n"
    "{comment} this built-in behaviour.\n"
    "tab_switch_on_horizontal_wheel: {}\n"
};

constexpr StringLiteral GuiConfigLockedConfig {
    "\n"
    "{comment} When set to true, the in-app graphical settings page is opened read-only and Contour\n"
    "{comment} will not write any GUI-managed side files (profiles/, colorschemes/, settings.yml).\n"
    "{comment} Use this to keep this hand-maintained configuration the single source of truth.\n"
    "gui_config_locked: {}\n"
};

constexpr StringLiteral GraphemeClusteringConfig {
    "\n"
    "{comment} Whether DEC mode 2027 (grapheme clustering) starts out set.\n"
    "{comment} While set, a codepoint arriving after the first may widen how many columns its\n"
    "{comment} grapheme cluster occupies -- so U+FE0F widens its cluster to two. A cluster never\n"
    "{comment} narrows again: U+FE0E asks for text presentation, not for a column back. While\n"
    "{comment} unset, a cell is measured by plain wcwidth() with no clustering at all. An\n"
    "{comment} application can still turn the mode on or off at runtime; this only chooses what it\n"
    "{comment} finds. A change applies to sessions opened afterwards.\n"
    "grapheme_clustering: {}\n"
};

constexpr StringLiteral TextScalingMethodConfig {
    "\n"
    "{comment} How a glyph is enlarged when an application asks for scaled text via the kitty text\n"
    "{comment} sizing protocol (OSC 66), ignore-case:\n"
    "{comment}   stretch     = magnify the glyph already rasterized at the ordinary cell size.\n"
    "{comment}                 Costs nothing to rasterize and adds no texture atlas entries, but\n"
    "{comment}                 softens as the scale grows.\n"
    "{comment}   rerasterize = ask the font for the glyph at the larger size, so the outline is\n"
    "{comment}                 re-hinted and stays crisp (default). Costs a rasterization and an\n"
    "{comment}                 atlas entry per glyph and scale in use.\n"
    "text_scaling_method: {}\n"
};

constexpr StringLiteral ThemeConfig {
    "\n"
    "{comment} Light/dark appearance of the GUI chrome (title bar, tab strip, command palette,\n"
    "{comment} settings pages, dialogs), independent of the terminal grid (ignore-case):\n"
    "{comment}   terminal = follow the terminal's background: dark chrome over a dark background,\n"
    "{comment}              light over a light one (default).\n"
    "{comment}   system   = follow the operating system's color scheme.\n"
    "{comment}   dark     = force a dark GUI appearance regardless of the OS.\n"
    "{comment}   light    = force a light GUI appearance regardless of the OS.\n"
    "{comment} The terminal grid keeps following the OS light/dark preference via its color scheme.\n"
    "theme: {}\n"
};

constexpr StringLiteral ColorSchemesConfig {
    "{comment} Color Profiles\n"
    "{comment} --------------\n"
    "{comment}\n"
    "{comment} Here you can configure your color profiles, whereas a color can be expressed in "
    "standard web "
    "format,\n"
    "{comment} with a leading # followed by red/green/blue values, 7 characters in total.\n"
    "{comment} You may alternatively use 0x as prefix instead of #.\n"
    "{comment} For example 0x102030 is equal to '#102030'.\n"
    "color_schemes:\n"
};

constexpr StringLiteral ProfilesConfig {
    "\n"
    "{comment} Terminal Profiles\n"
    "{comment} -----------------\n"
    "{comment}\n"
    "{comment} Dominates how your terminal visually looks like. You will need at least one terminal "
    "profile.\n"
    "profiles:\n"
    "\n"
};

constexpr StringLiteral LayoutsConfig {
    "\n"
    "{comment} Layouts\n"
    "{comment} -------\n"
    "{comment}\n"
    "{comment} Named sets of tabs opened together. Each tab may set a title, color, working\n"
    "{comment} directory, a command to run (replacing the shell), a profile override, and split\n"
    "{comment} panes. See the documentation for the full schema.\n"
    "layouts:\n"
    "\n"
};
constexpr StringLiteral LayoutsWeb { "Named sets of tabs opened together." };
constexpr StringLiteral HostsConfig {
    "\n"
    "{comment} Hosts\n"
    "{comment} -----\n"
    "{comment}\n"
    "{comment} Saved remote hosts that \"Attach to Host\" lists, by name. Each host has:\n"
    "{comment}   url             ssh://[user@]host[:port] or tls://host:port (required)\n"
    "{comment}   token_file      the daemon's token file (required for tls://)\n"
    "{comment}   tls_ca          a CA to verify the daemon's certificate against (tls://)\n"
    "{comment}   remote_command  replaces the whole remote command (ssh://)\n"
    "{comment}   profile         the profile the remote sessions are created with\n"
    "{comment}\n"
    "{comment} hosts:\n"
    "{comment}     home:\n"
    "{comment}         url: \"ssh://me@homepc\"\n"
    "{comment}         remote_command: \"/opt/contour/bin/contour daemon --stdio\"\n"
    "{comment}         profile: \"remote\"\n"
    "{comment}     studio:\n"
    "{comment}         url: \"tls://studio.tailnet.ts.net:7297\"\n"
    "{comment}         token_file: \"~/.config/contour/studio.token\"\n"
    "{comment}         tls_ca: \"~/.config/contour/studio-ca.pem\"\n"
    "hosts:\n"
    "\n"
};
constexpr StringLiteral HostsWeb { "Saved remote hosts that \"Attach to Host\" lists." };
constexpr StringLiteral DefaultLayoutWeb {
    "Name of the layout to open at startup, instead of a single default tab."
};

constexpr StringLiteral WordDelimitersConfig { "{comment} Word delimiters when selecting word-wise.\n"
                                               "word_delimiters: \"{}\"\n"
                                               "\n" };

constexpr StringLiteral ExtendedWordDelimitersConfig {
    "{comment} Word delimiters for second selection when selecting word-wise.\n"
    "{comment} Setting allows you to set less strict boundaried between words, for example\n"
    "{comment} if you want to select whole ip address during selection set delimieters to \" \" (space)\n"
    "extended_word_delimiters: \"{}\"\n"
    "\n"
};

constexpr StringLiteral BypassMouseProtocolModifiersConfig {
    "{comment} This keyboard modifier can be used to bypass the terminal's mouse protocol,\n"
    "{comment} which can be used to select screen content even if the an application\n"
    "{comment} mouse protocol has been activated (Default: Shift).\n"
    "{comment}\n"
    "{comment} The same modifier values apply as with input modifiers (see below).\n"
    "bypass_mouse_protocol_modifier: {}\n"
    "\n"
};

constexpr StringLiteral OnMouseSelectionConfig {
    "{comment} Selects an action to perform when a text selection has been made.\n"
    "{comment}\n"
    "{comment} Possible values are:\n"
    "{comment}\n"
    "{comment} - None                        Does nothing\n"
    "{comment} - CopyToClipboard             Copies the selection to the primary clipboard.\n"
    "{comment} - CopyToSelectionClipboard    Copies the selection to the selection clipboard.\n"
    "{comment}This is not supported on all platforms.\n"
    "{comment}\n"
    "on_mouse_select: {}\n"
    "\n"
};

constexpr StringLiteral MouseBlockSelectionModifiersConfig {
    "{comment} Modifier to be pressed in order to initiate block-selection\n"
    "{comment} using the left mouse button.\n"
    "{comment}\n"
    "{comment} This is usually the Control modifier, but on OS/X that is not possible,\n"
    "{comment} so Alt or Meta would be recommended instead.\n"
    "{comment}\n"
    "{comment} Supported modifiers:\n"
    "{comment} - Alt\n"
    "{comment} - Control\n"
    "{comment} - Shift\n"
    "{comment} - Meta\n"
    "{comment}\n"
    "mouse_block_selection_modifier: {}\n"
    "\n"
};

constexpr StringLiteral InputMappingsConfig {
    "{comment} Key Bindings\n"
    "{comment} ------------\n"
    "{comment}\n"
    "{comment} In this section you can customize key bindings.\n"
    "{comment}\n"
    "{comment} A few bindings are BUILT IN and are not listed below: Ctrl+Tab / Ctrl+Shift+Tab and\n"
    "{comment} Ctrl+PageDown / Ctrl+PageUp switch tabs, right-click opens the context menu, and a\n"
    "{comment} horizontal wheel switches tabs. They apply only when this list does not claim the same\n"
    "{comment} key or button, so binding one here replaces it. They are built in rather than listed\n"
    "{comment} because this list, once written to a config file, replaces the built-in one wholesale --\n"
    "{comment} so a binding added to Contour after your config was generated could never reach you.\n"
    "{comment}\n"
    "{comment} Each array element in `input_mapping` represents one key binding,\n"
    "{comment} whereas `mods` represents an array of keyboard modifiers that must be pressed - as well "
    "as\n"
    "{comment} the `key` or `mouse` -  in order to activate the corresponding action,\n"
    "{comment}\n"
    "{comment} Additionally one can filter input mappings based on special terminal modes using the "
    "`modes` "
    "option:\n"
    "{comment} - Alt       : The terminal is currently in alternate screen buffer, otherwise it is in "
    "primary "
    "screen buffer.\n"
    "{comment} - AppCursor : The application key cursor mode is enabled (otherwise it's normal cursor "
    "mode).\n"
    "{comment} - AppKeypad : The application keypad mode is enabled (otherwise it's the numeric keypad "
    "mode).\n"
    "{comment} - Select    : The terminal has currently an active grid cell selection (such as selected "
    "text).\n"
    "{comment} - Insert    : The Insert input mode is active, that is the default and one way to test\n"
    "{comment}               that the input mode is not in normal mode or any of the visual select "
    "modes.\n"
    "{comment} - Search    : There is a search term currently being edited or already present.\n"
    "{comment} - Trace     : The terminal is currently in trace-mode, i.e., each VT sequence can be "
    "interactively\n"
    "{comment}               single-step executed using custom actions. See "
    "TraceEnter/TraceStep/TraceLeave "
    "actions.\n"
    "{comment}\n"
    "{comment} You can combine these modes by concatenating them via | and negate a single one\n"
    "{comment} by prefixing with ~.\n"
    "{comment}\n"
    "{comment} The `modes` option defaults to not filter at all (the input mappings always\n"
    "{comment} match based on modifier and key press / mouse event).\n"
    "{comment}\n"
    "{comment} `key` represents keys on your keyboard, and `mouse` represents buttons\n"
    "{comment} as well as the scroll wheel.\n"
    "{comment}\n"
    "{comment} Modifiers:\n"
    "{comment} - Alt\n"
    "{comment} - Control\n"
    "{comment} - Shift\n"
    "{comment} - Meta (this is the Windows key on Windows OS, and the Command key on OS/X, and Meta on "
    "anything "
    "else)\n"
    "{comment}\n"
    "{comment} Keys can be expressed case-insensitively symbolic:\n"
    "{comment}   APOSTROPHE, ADD, BACKSLASH, COMMA, DECIMAL, DIVIDE, EQUAL, LEFT_BRACKET,\n"
    "{comment}   MINUS, MULTIPLY, PERIOD, RIGHT_BRACKET, SEMICOLON, SLASH, SUBTRACT, SPACE\n"
    "{comment}   Enter, Backspace, Tab, Escape, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,\n"
    "{comment}   DownArrow, LeftArrow, RightArrow, UpArrow, Insert, Delete, Home, End, PageUp, "
    "PageDown,\n"
    "{comment}   Numpad_NumLock, Numpad_Divide, Numpad_Multiply, Numpad_Subtract, Numpad_CapsLock,\n"
    "{comment}   Numpad_Add, Numpad_Decimal, Numpad_Enter, Numpad_Equal,\n"
    "{comment}   Numpad_0, Numpad_1, Numpad_2, Numpad_3, Numpad_4,\n"
    "{comment}   Numpad_5, Numpad_6, Numpad_7, Numpad_8, Numpad_9\n"
    "{comment} or in case of standard characters, just the character.\n"
    "{comment}\n"
    "{comment} Mouse buttons can be one of the following self-explanatory ones:\n"
    "{comment}   Left, Middle, Right, WheelUp, WheelDown, WheelLeft, WheelRight\n"
    "{comment}\n"
    "{comment} Actions:\n"
    "{comment} - CancelSelection   Cancels currently active selection, if any.\n"
    "{comment} - ChangeProfile     Changes the profile to the given profile `name`.\n"
    "{comment} - ClearHistoryAndReset    Clears the history, performs a terminal hard reset and attempts "
    "to "
    "force a redraw of the currently running application.\n"
    "{comment} - CopyPreviousMarkRange   Copies the most recent range that is delimited by vertical line "
    "marks "
    "into clipboard.\n"
    "{comment} - CopySelection     Copies the current selection into the clipboard buffer.\n"
    "{comment} - CreateSelection   Creates selection with custom delimiters configured via `delimiters` "
    "member.\n"
    "{comment} - DecreaseFontSize  Decreases the font size by 1 pixel.\n"
    "{comment} - DecreaseOpacity   Decreases the default-background opacity by 5%.\n"
    "{comment} - FocusNextSearchMatch     Focuses the next search match (if any).\n"
    "{comment} - FocusPreviousSearchMatch Focuses the next previous match (if any).\n"
    "{comment} - FollowHyperlink   Follows the hyperlink that is exposed via OSC 8 under the current "
    "cursor "
    "position.\n"
    "{comment} - HintMode          Activates hint mode to scan for patterns (URLs, file paths, etc.).\n"
    "{comment}                     Parameters: patterns (url, filepath, githash, ipv4, ipv6, all, or "
    "names joined with |),\n"
    "{comment}                     hint_action (Copy, Open, Paste, CopyAndPaste, Select).\n"
    "{comment} - IncreaseFontSize  Increases the font size by 1 pixel.\n"
    "{comment} - IncreaseOpacity   Increases the default-background opacity by 5%.\n"
    "{comment} - MoveTabToLeft     Moves the current tab to the left.\n"
    "{comment} - MoveTabToRight    Moves the current tab to the right.\n"
    "{comment} - NewTerminal       Spawns a new terminal at the current terminals current working "
    "directory.\n"
    "{comment} - NoSearchHighlight Disables current search highlighting, if anything is still "
    "highlighted "
    "due to "
    "a prior search.\n"
    "{comment} - OpenConfiguration Opens the configuration file.\n"
    "{comment} - OpenFileManager   Opens the current working directory in a system file manager.\n"
    "{comment} - OpenSelection     Open the current terminal selection with the default system "
    "application "
    "(eg; "
    "xdg-open)\n"
    "{comment} - PasteClipboard    Pastes clipboard to standard input. Pass boolean parameter 'strip' to "
    "indicate whether or not to strip repetitive whitespaces down to one and newlines to "
    "whitespaces.\n"
    "{comment} - PasteSelection    Pastes current selection to standard input.\n"
    "{comment} - Quit              Quits the application.\n"
    "{comment} - ReloadConfig      Forces a configuration reload.\n"
    "{comment} - ResetConfig       Overwrites current configuration with builtin default configuration "
    "and "
    "loads "
    "it. Attention, all your current configuration will be lost due to overwrite!\n"
    "{comment} - ResetFontSize     Resets font size to what is configured in the config file.\n"
    "{comment} - ScreenshotVT      Takes a screenshot in form of VT escape sequences.\n"
    "{comment} - SaveScreenshot    Takes a screenshot and saves it into a file.\n"
    "{comment} - CopyScreenshot    Takes a screenshot and puts it into the system clipboard\n"
    "{comment} - ScrollDown        Scrolls down by the multiplier factor.\n"
    "{comment} - ScrollMarkDown    Scrolls one mark down (if none present, bottom of the screen)\n"
    "{comment} - ScrollMarkUp      Scrolls one mark up\n"
    "{comment} - ScrollOneDown     Scrolls down by exactly one line.\n"
    "{comment} - ScrollOneUp       Scrolls up by exactly one line.\n"
    "{comment} - ScrollPageDown    Scrolls a page down.\n"
    "{comment} - ScrollPageUp      Scrolls a page up.\n"
    "{comment} - ScrollToBottom    Scrolls to the bottom of the screen buffer.\n"
    "{comment} - ScrollToTop       Scrolls to the top of the screen buffer.\n"
    "{comment} - ScrollUp          Scrolls up by the multiplier factor.\n"
    "{comment} - SearchReverse     Opens the find bar, searching upwards from the current cursor "
    "position.\n"
    "{comment} - SendChars         Writes given characters in `chars` member to the applications input.\n"
    "{comment} - SwitchToTab       Switches to the tab position, given by extra parameter \"position\".\n"
    "{comment}                     The positions start at number 1.\n"
    "{comment} - SwitchToPreviousTab Switches to the previously active tab.\n"
    "{comment} - SwitchToTabLeft   Switches to the tab left of the current tab.\n"
    "{comment} - SwitchToTabRight  Switches to the tab right of the current tab.\n"
    "{comment} - CreateNewTab      Creates a new tab.\n"
    "{comment} - CloseTab          Closes the current tab.\n"
    "{comment} - ToggleAllKeyMaps  Disables/enables responding to all keybinds (this keybind will be "
    "preserved "
    "when disabling all others).\n"
    "{comment} - ToggleFullScreen  Enables/disables full screen mode.\n"
    "{comment} - ToggleInputMethodHandling Enables/disables IME (input method editor) handling.\n"
    "{comment} - ToggleInputProtection Enables/disables terminal input protection.\n"
    "{comment} - ToggleStatusLine  Shows/hides the VT320 compatible Indicator status line.\n"
    "{comment} - ToggleTitleBar    Shows/Hides titlebar\n"
    "{comment} - TraceBreakAtEmptyQueue Executes any pending VT sequence from the VT sequence buffer in "
    "trace "
    "mode, then waits.\n"
    "{comment} - TraceEnter        Enables trace mode, suspending execution until explicitly requested "
    "to "
    "continue (See TraceLeave and TraceStep).\n"
    "{comment} - TraceLeave        Disables trace mode. Any pending VT sequence will be flushed out and "
    "normal "
    "execution will be resumed.\n"
    "{comment} - TraceStep         Executes a single VT sequence that is to be executed next.\n"
    "{comment} - ViNormalMode      Enters/Leaves Vi-like normal mode. The cursor can then be moved via "
    "h/j/k/l "
    "movements in normal mode and text can be selected via v, yanked via y, and clipboard pasted via "
    "p.\n"
    "{comment} - WriteScreen       Writes VT sequence in `chars` member to the screen (bypassing the "
    "application).\n"
    "{comment} - SetTabTitle       Rename the active tab inline (opens the tab-title editor).\n"
    "{comment} - SetTabColor       Colorize the active tab. With `color: '#RRGGBB'` it applies that "
    "color directly; without it, the tab's color picker opens.\n"
    "{comment} - ResetTabColor     Removes the color given to the active tab, restoring its default.\n"
    "\n"
    "input_mapping:\n"
};

constexpr StringLiteral SpawnNewProcessConfig {
    "\n"
    "{comment} Flag to determine whether to spawn new process or not when creating new terminal\n"
    "spawn_new_process: {}\n"
};

constexpr unsigned DefaultEarlyExitThreshold = 5u;
constexpr StringLiteral EarlyExitThresholdConfig { "\n"
                                                   "{comment} Time in seconds to check for early threshold\n"
                                                   "early_exit_threshold: {}\n" };

constexpr StringLiteral ImagesConfig {
    "images:\n"
    "    {comment} Enable or disable sixel scrolling (SM/RM ?80 default)\n"
    "    sixel_scrolling: {}\n"
    "\n"
    "    {comment} Configures the maximum number of color registers available when rendering Sixel "
    "graphics.\n"
    "    sixel_register_count: {}\n"
    "\n"
    "    {comment} Enable or disable Good Image Protocol (GIP) support (disabled by default).\n"
    "    good_image_protocol: {}\n"
    "\n"
};

constexpr StringLiteral DaemonConfig {
    "daemon:\n"
    "    {comment} Where sessions live. auto (the default) hosts them in a daemon, starting one when\n"
    "    {comment} none runs, and falls back to in_process, with a notice, when none can be reached;\n"
    "    {comment} in_process keeps them in this process; required hosts them in a daemon and fails\n"
    "    {comment} when none can be reached.\n"
    "    mode: {}\n"
    "\n"
    "    {comment} The daemon's socket path. Empty selects the default.\n"
    "    socket: {}\n"
    "\n"
    "    {comment} Which attached client's size the shared grid takes when clients disagree:\n"
    "    {comment} latest, smallest or largest.\n"
    "    size_policy: {}\n"
    "\n"
    "    {comment} Scrollback lines the daemon keeps per session (-1 for unlimited). ~ uses the\n"
    "    {comment} hosting profile's history.limit.\n"
    "    history_limit: {}\n"
    "\n"
    "    {comment} What the daemon keeps of its workspace across a restart (a reboot, a crash, an upgrade):\n"
    "    {comment} windows, tabs, splits, each pane's directory and profile, and the tail of its "
    "scrollback.\n"
    "    {comment} `contour daemon stop` discards it, unless given --keep-snapshot.\n"
    "    restore:\n"
    "        {comment} Whether a restored pane gets its scrollback back. The file may hold secrets that\n"
    "        {comment} were on screen; it is readable by your user only.\n"
    "        scrollback: {}\n"
    "        {comment} The most scrollback lines kept per pane (0 to 100000).\n"
    "        scrollback_lines: {}\n"
    "        {comment} Programs a restored pane runs again when it was running one of them, e.g. [vim, "
    "htop].\n"
    "        {comment} Nothing is re-run unless listed here.\n"
    "        rerun_programs: {}\n"
    "        {comment} How often, in seconds, a changed workspace is written (at least 5).\n"
    "        interval: {}\n"
    "\n"
};

constexpr StringLiteral ExperimentalFeaturesConfig {
    "\n"
    "{comment} Section of experimental features.\n"
    "{comment} All experimental features are disabled by default and must be explicitly enabled here.\n"
    "{comment} NOTE: Contour currently has no experimental features behind this configuration wall.\n"
    "{comment} experimental:\n"
    "{comment}     {comment} Enables experimental support for feature X/Y/Z\n"
    "{comment}     feature_xyz: true\n"
};

constexpr StringLiteral DefaultColorsConfig {
    "{comment} Default colors\n"
    "default:\n"
    "    {comment} Default background color (this can be made "
    "transparent, see above).\n"
    "    background: {}\n"
    "    {comment} Default foreground text color.\n"
    "    foreground: {}\n"
    "    {comment} Default foreground text color when bold(/bright) mode is on.\n"
    "    bright_foreground: {}\n"
    "    {comment} Default foreground text color when dim mode is on.\n"
    "    dimmed_foreground: {}\n"
};

constexpr StringLiteral HyperlinkDecorationConfig {
    "\n"
    "{comment} How links are decorated: OSC 8 hyperlinks and the URLs, paths and patterns found in output.\n"
    "{comment} mode: when they are decorated\n"
    "{comment}   always   - always\n"
    "{comment}   hover    - only the one under the pointer\n"
    "{comment}   modifier - all of them while the follow modifier (Ctrl of Ctrl+click) is held\n"
    "{comment}   never    - never; Ctrl+click still opens them\n"
    "{comment} normal, hover: the decorator for a link at rest and under the pointer: underline,\n"
    "{comment} dotted-underline, double-underline, curly-underline, dashed-underline, overline,\n"
    "{comment} crossed-out, framed, encircle.\n"
    "hyperlink_decoration:\n"
    "    mode: {}\n"
    "    normal: {}\n"
    "    hover: {}\n"
};

constexpr StringLiteral LinkDetectionConfig {
    "\n"
    "{comment} Which links are found automatically in printed text, per screen. OSC 8 hyperlinks are\n"
    "{comment} always links, and hint mode always offers everything.\n"
    "{comment} primary_screen: the normal shell screen. other_screens: the alternate screen (vim, less,\n"
    "{comment} htop, ...) and every further page.\n"
    "{comment} urls also covers your own patterns, unless they open a `file:` URL, which follows "
    "file_paths.\n"
    "link_detection:\n"
    "    primary_screen:\n"
    "        urls: {}\n"
    "        file_paths: {}\n"
    "    other_screens:\n"
    "        urls: {}\n"
    "        file_paths: {}\n"
};

constexpr StringLiteral FileOpenerConfig {
    "\n"
    "{comment} How a detected file path opens on Ctrl+click and with hint mode's Open.\n"
    "{comment} command: a command line with {{path}}, {{line}} and {{column}}, split into arguments once\n"
    "{comment}   (no shell); each placeholder is filled inside its own argument, line and column being 1\n"
    "{comment}   where the path was printed without them. The program itself is written out, without a\n"
    "{comment}   placeholder. Empty: the system's default application.\n"
    "{comment}   A directory always opens in the file manager.\n"
    "{comment} placement: detached (a program of its own), or split or tab (a Contour pane running it)\n"
    "{comment} Example: command: 'code --goto {{path}}:{{line}}:{{column}}'\n"
    "file_opener:\n"
    "    command: {}\n"
    "    placement: {}\n"
};

/// The colour scheme's `hyperlink_decoration` section: colours only, the decorators and the mode being the
/// profile's.
constexpr StringLiteral HyperlinkDecorationColorsConfig {
    "\n"
    "{comment} The colours of a link's decoration, at rest and under the pointer.\n"
    "hyperlink_decoration:\n"
    "    normal: {}\n"
    "    hover: {}\n"
};

constexpr StringLiteral YankHighlightConfig {
    "\n"
    "{comment} Color to pick for vi_mode highlights.\n"
    "{comment} The value format is equivalent to how selection colors and "
    "alpha contribution "
    "is defined.\n"
    "vi_mode_highlight:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral NormalModeCursorlineConfig {
    "\n"
    "{comment} Color override for the current cursor's line when in vi_mode:\n"
    "{comment} The value format is equivalent to how selection colors and alpha "
    "contribution "
    "is defined.\n"
    "{comment} To disable cursorline in vi_mode, set foreground to CellForeground "
    "and "
    "background to CellBackground.\n"
    "vi_mode_cursorline:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral SelectionConfig {
    "\n"
    "{comment} The text selection color can be customized here.\n"
    "{comment} Leaving a value empty will default to the inverse of the content's "
    "color "
    "values.\n"
    "{comment}\n"
    "{comment} The color can be specified in RGB as usual, plus\n"
    "{comment} - CellForeground: Selects the cell's foreground color.\n"
    "{comment} - CellBackground: Selects the cell's background color.\n"
    "selection:\n"
    "    {comment} Specifies the color to be used for the selected text.\n"
    "    {comment}\n"
    "    foreground: {}\n"
    "    {comment} Specifies the alpha value (between 0.0 and 1.0) the configured "
    "foreground "
    "color\n"
    "    {comment} will contribute to the original color.\n"
    "    {comment}\n"
    "    {comment} A value of 1.0 will paint over, whereas a value of 0.5 will give\n"
    "    {comment} a look of a half-transparently painted grid cell.\n"
    "    foreground_alpha: {}\n"
    "    {comment} Specifies the color to be used for the selected background.\n"
    "    {comment}\n"
    "    background: {}\n"
    "    {comment} Specifies the alpha value (between 0.0 and 1.0) the configured "
    "background "
    "color\n"
    "    {comment} will contribute to the original color.\n"
    "    {comment}\n"
    "    {comment} A value of 1.0 will paint over, whereas a value of 0.5 will give\n"
    "    {comment} a look of a half-transparently painted grid cell.\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral SearchHighlightConfig {
    "\n"
    "{comment} Search match highlighting. Similar to selection highlighting.\n"
    "search_highlight:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral SearchHighlightFocusedConfig {
    "\n"
    "{comment} Search match highlighting (focused term). Similar to selection "
    "highlighting.\n"
    "search_highlight_focused:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral WordHighlightCurrentConfig {
    "\n"
    "{comment} Coloring for the word that is highlighted due to double-clicking it.\n"
    "{comment}\n"
    "{comment} The format is similar to selection highlighting.\n"
    "word_highlight_current:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral WordHighlightConfig {
    "\n"
    "{comment} Coloring for the word that is highlighted due to double-clicking\n"
    "{comment} another word that matches this word.\n"
    "{comment}\n"
    "{comment} The format is similar to selection highlighting.\n"
    "word_highlight_other:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral HintLabelConfig {
    "\n"
    "{comment} Colors for hint mode labels (the short alphabetic tags shown on matches).\n"
    "{comment} The format is equivalent to selection/search highlighting.\n"
    "hint_label:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral HintMatchConfig {
    "\n"
    "{comment} Colors for hint mode match bodies (the text behind the labels).\n"
    "{comment} The format is equivalent to selection/search highlighting.\n"
    "hint_match:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral HintPatternsConfig {
    "\n"
    "{comment} Patterns for hint mode and for the links detected in output.\n"
    "{comment} Built-in patterns: url and filepath (clickable), githash, ipv4 and ipv6 (hint mode only).\n"
    "{comment} An entry named like a built-in overrides only the keys it gives, and may leave out\n"
    "{comment} its regex: `- {{ name: filepath, clickable: false }}` stops underlining paths.\n"
    "{comment}   regex:     ECMAScript regular expression; back-references (\\1 to \\9) are not supported.\n"
    "{comment}   clickable: true to detect it in output, decorate it and open it with Ctrl+click\n"
    "{comment}              (default: false; the built-ins keep their own).\n"
    "{comment}   url:       for your own patterns, what Ctrl+click opens: {{0}} is the match,\n"
    "{comment}              {{1}} to {{9}} its capture groups (default: the match as printed).\n"
    "{comment}              A group is percent-encoded only where a URL cannot hold a character\n"
    "{comment}              (`/?&=+@:%` pass through): write the scheme and host out, groups after.\n"
    "{comment} A regex overriding filepath names the path with capture group 1, and its location\n"
    "{comment} with groups 2 and 3 (`:line[:column]`) and 4 and 5 (`(line[,column])`);\n"
    "{comment} any other pattern's link is its whole match.\n"
    "{comment} Example:\n"
    "{comment} hint_patterns:\n"
    "{comment}     - name: uuid\n"
    "{comment}       regex: "
    "'\\b[0-9a-fA-F]{{8}}-[0-9a-fA-F]{{4}}-[0-9a-fA-F]{{4}}-[0-9a-fA-F]{{4}}-[0-9a-fA-F]{{12}}\\b'\n"
    "{comment}     - name: jira\n"
    "{comment}       regex: '\\b[A-Z][A-Z0-9]+-\\d+\\b'\n"
    "{comment}       clickable: true\n"
    "{comment}       url: 'https://jira.example.com/browse/{{0}}'\n"
};

constexpr StringLiteral ExtensionsConfig {
    "\n"
    "{comment} WebAssembly extensions for the status line. Each runs sandboxed, one instance per pane, and\n"
    "{comment} none is configured by default. Name one in a status line template with {{Extension:<name>}}, "
    "or\n"
    "{comment} let it take the whole line over with `status_line.extension`.\n"
    "{comment}   path:        The module to load; a leading ~ is the home directory. Without it the name\n"
    "{comment}                selects an extension bundled with Contour.\n"
    "{comment}   config:      Text values the extension reads by key.\n"
    "{comment}   run_command: Whether the extension may run the commands its manifest declares:\n"
    "{comment}                allow, deny or ask (default: ask).\n"
    "{comment} Example:\n"
    "{comment} extensions:\n"
    "{comment}     git:    {{ run_command: ask }}\n"
    "{comment}     plasma: {{ config: {{ speed: \"2\" }} }}\n"
    "{comment}     mine:   {{ path: ~/wasm/mine.wasm, run_command: deny }}\n"
};

constexpr StringLiteral HintScrollbackLinesConfig {
    "\n"
    "{comment} How many scrollback rows a HintMode action with `scope: Scrollback` scans at most.\n"
    "{comment} Only that scope reads this; `scope: Visible` (the default) never leaves the viewport.\n"
    "hint_scrollback_lines: {}\n"
};

constexpr StringLiteral IndicatorStatusLineConfig {
    "\n"
    "{comment} Defines the colors to be used for the Indicator status line.\n"
    "{comment} Configuration consist of different sections: default, inactive, insert_mode, normal_mode, "
    "visual_mode.\n"
    "{comment} Each section customize status line colors for corresponding mode.\n"
    "indicator_statusline:\n"
    "    default:\n"
    "        foreground: {}\n"
    "        background: {}\n"
    "    inactive:\n"
    "        foreground: {}\n"
    "        background: {}\n"
};

constexpr StringLiteral FoldMarkerConfig {
    "\n"
    "{comment} Colors for the fold column drawn in the gutter left of the grid (output folding).\n"
    "{comment} Unset by default, and deliberately: the column is then derived from this scheme's own\n"
    "{comment} foreground/background -- a faded glyph at rest, coming up to full strength under the\n"
    "{comment} pointer -- so it stays legible in any theme without every theme having to know the\n"
    "{comment} feature exists. Uncomment to override.\n"
    "{comment} The values shown are what the default scheme derives, so uncommenting them as-is\n"
    "{comment} changes nothing -- they are a starting point to edit, not a second opinion.\n"
    "{comment} fold_marker:\n"
    "{comment}     default:\n"
    "{comment}         foreground: '#7e7c7c'\n"
    "{comment}         background: '#1a1716'\n"
    "{comment}     hover:\n"
    "{comment}         foreground: '#d0d0d0'\n"
    "{comment}         background: '#1a1716'\n"
};

constexpr StringLiteral FoldMarkerHeaderConfig {
    "\n"
    "{comment} Colors for the fold column drawn in the gutter left of the grid (output folding).\n"
    "{comment} Written back because this scheme sets them; remove the block to go back to deriving the\n"
    "{comment} column from this scheme's own foreground/background, faded at rest and at full strength\n"
    "{comment} under the pointer.\n"
    "fold_marker:\n"
};

constexpr StringLiteral FoldMarkerDefaultConfig { "    default:\n"
                                                  "        foreground: {}\n"
                                                  "        background: {}\n" };

constexpr StringLiteral FoldMarkerHoverConfig { "    hover:\n"
                                                "        foreground: {}\n"
                                                "        background: {}\n" };

constexpr StringLiteral ContextTintConfig {
    "\n"
    "{comment} Background tints for OSC 3008 hierarchical contexts: the page is painted in a different\n"
    "{comment} shade while an elevated, containerised, virtualised or remote context is in force, so\n"
    "{comment} that output produced under one is distinguishable at a glance.\n"
    "{comment} Unset by default, and deliberately. No colour is right in both a light and a dark scheme,\n"
    "{comment} a background that changes on upgrade reads as a rendering fault rather than a feature,\n"
    "{comment} and this is a signal worth opting into knowingly rather than inheriting.\n"
    "{comment} A context is something the session ANNOUNCED. Anything that can write to your terminal\n"
    "{comment} can announce one, or fail to announce one, so the absence of a tint is NOT a statement\n"
    "{comment} that nothing is running as root.\n"
    "{comment} These are deliberately subtle -- a few percent off the background, not a slab. On a\n"
    "{comment} light scheme you want the mirror image.\n"
    "{comment} tint:\n"
    "{comment}     elevate: '#2a1a1a'\n"
    "{comment}     container: '#161c26'\n"
    "{comment}     remote: '#1a2018'\n"
};

constexpr StringLiteral ContextTintHeaderConfig {
    "\n"
    "{comment} Background tints for OSC 3008 hierarchical contexts. Written back because this scheme\n"
    "{comment} sets them; remove the block to stop tinting. Note that a context is something the\n"
    "{comment} session announced, and anything that can write to your terminal can announce one -- or\n"
    "{comment} fail to. The absence of a tint is not a statement that nothing is running as root.\n"
    "tint:\n"
};

/// One `key: value` row of a map whose header precedes it, such as a colour scheme's `tint` or
/// `block_status`.
constexpr StringLiteral IndentedEntryConfig { "    {}: {}\n" };

constexpr StringLiteral BlockStatusConfig {
    "\n"
    "{comment} Colors of the gutter's block column by how each command ended: its fold controls, the\n"
    "{comment} triangle beside a running command and the dot beside a finished one all take them.\n"
    "{comment} Unset by default: success then looks exactly like the fold column (fold_marker above),\n"
    "{comment} failure takes this scheme's red (normal color 1) and running its blue (normal color 4),\n"
    "{comment} so a scheme that has never heard of them still shows them legibly. Uncomment to override.\n"
    "{comment} The values shown are what the default scheme derives, so uncommenting failure and running\n"
    "{comment} as they are changes nothing. Uncommenting success changes one thing: a success the scheme\n"
    "{comment} sets is lifted only halfway toward the text color under the pointer, as the other two are,\n"
    "{comment} where the fold column comes up to full strength.\n"
    "{comment} block_status:\n"
    "{comment}     success: '#7E7C7C'\n"
    "{comment}     failure: '#C63939'\n"
    "{comment}     running: '#4D79FF'\n"
};

constexpr StringLiteral BlockStatusHeaderConfig {
    "\n"
    "{comment} Colors of the gutter's block column by how each command ended. Written back because this\n"
    "{comment} scheme sets them; remove a key to derive it again -- success from the fold column, failure\n"
    "{comment} from normal color 1, running from normal color 4.\n"
    "block_status:\n"
};

constexpr StringLiteral BlockStatusHoverConfig {
    "\n"
    "{comment} Colors of the gutter's block column by how each command ended, while the pointer is over\n"
    "{comment} its fold. Unset by default: each is then its block_status color lifted halfway toward this\n"
    "{comment} scheme's text color -- and a success block_status leaves to the fold column comes up as\n"
    "{comment} the column does, to fold_marker.hover. Uncomment to override. The values shown are what\n"
    "{comment} the default scheme derives, so uncommenting them as they are changes nothing.\n"
    "{comment} block_status_hover:\n"
    "{comment}     success: '#D0D0D0'\n"
    "{comment}     failure: '#CB8484'\n"
    "{comment}     running: '#8EA4E7'\n"
};

constexpr StringLiteral BlockStatusHoverHeaderConfig {
    "\n"
    "{comment} Colors of the gutter's block column by how each command ended, while the pointer is over\n"
    "{comment} its fold. Written back because this scheme sets them; remove a key to derive it again from\n"
    "{comment} block_status.\n"
    "block_status_hover:\n"
};

constexpr StringLiteral GutterTextConfig {
    "\n"
    "{comment} Color of the gutter's line numbers and timestamps. Unset by default: this scheme's own\n"
    "{comment} foreground faded toward its background, as quiet as the resting fold column. The value\n"
    "{comment} shown is what the default scheme derives.\n"
    "{comment} gutter_text: '#7E7C7C'\n"
};

constexpr StringLiteral GutterTextSetConfig {
    "\n"
    "{comment} Color of the gutter's line numbers and timestamps. Remove it to derive the color from\n"
    "{comment} this scheme's own foreground and background again.\n"
    "gutter_text: {}\n"
};

constexpr StringLiteral StickyHeaderColorsConfig {
    "\n"
    "{comment} Colors of the sticky command header pinned over the top row (profile key sticky_header):\n"
    "{comment} its band, and the line along its bottom edge. Unset by default: the band is this scheme's\n"
    "{comment} background lifted a tenth of the way toward its foreground, the line about a third of the\n"
    "{comment} way, so both read in any theme. The values shown are what the default scheme derives, so\n"
    "{comment} uncommenting them as they are changes nothing.\n"
    "{comment} sticky_header:\n"
    "{comment}     background: '#2C2928'\n"
    "{comment}     separator: '#595757'\n"
};

constexpr StringLiteral StickyHeaderColorsHeaderConfig {
    "\n"
    "{comment} Colors of the sticky command header. Written back because this scheme sets them; remove a\n"
    "{comment} key to derive it again from this scheme's own background and foreground.\n"
    "sticky_header:\n"
};

constexpr StringLiteral InputMethodEditorConfig { "\n"
                                                  "{comment} Colors for the IME (Input Method Editor) area.\n"
                                                  "input_method_editor:\n"
                                                  "    foreground: {}\n"
                                                  "    background: {}\n" };

constexpr StringLiteral NormalColorsConfig {
    "\n"
    "{comment} Normal colors, by name. normal, bright and dim also take\n"
    "{comment} a list in this order: [black, red, ..., white].\n"
    "{comment} A color is '#rrggbb' or 0xrrggbb, not a decimal number.\n"
    "normal:\n"
    "    black:   {}\n"
    "    red:     {}\n"
    "    green:   {}\n"
    "    yellow:  {}\n"
    "    blue:    {}\n"
    "    magenta: {}\n"
    "    cyan:    {}\n"
    "    white:   {}\n"
};

constexpr StringLiteral BrightColorsConfig { "\n"
                                             "{comment} Bright colors\n"
                                             "bright:\n"
                                             "    black:   {}\n"
                                             "    red:     {}\n"
                                             "    green:   {}\n"
                                             "    yellow:  {}\n"
                                             "    blue:    {}\n"
                                             "    magenta: {}\n"
                                             "    cyan:    {}\n"
                                             "    white:   {}\n" };

constexpr StringLiteral DimColorsConfig {
    "\n"
    "{comment} Dim (faint) colors, if not set, they're automatically computed "
    "based on normal colors.\n"
    "{comment} dim:\n"
    "{comment}     black:   {}\n"
    "{comment}     red:     {}\n"
    "{comment}     green:   {}\n"
    "{comment}     yellow:  {}\n"
    "{comment}     blue:    {}\n"
    "{comment}     magenta: {}\n"
    "{comment}     cyan:    {}\n"
    "{comment}     white:   {}\n"
};

constexpr StringLiteral BoxArcStyleConfig {
    "\n"
    "{comment} Controls how arcs in box drawing characters are drawn. "
    "May be on of `round`, `ellips`. The default value is `round`.\n"
    "box_arc_style: {}\n"
};

constexpr StringLiteral BrailleStyleConfig {
    "\n"
    "{comment} Controls how Unicode Braille characters are rendered.\n "
    "{comment} May be one of `font`, `solid`, `circle`, `circle_empty`, "
    "`square`, `square_empty`, `aa_square`, `aa_square_empty`.\n"
    "{comment} The default value is `circle`.\n"
    "{comment} `font` disables special rendering and uses font characters.\n"
    "{comment} `solid` splits character box into 8 rectangles without space between them."
    "No antialiasing is used, some font sizes may result in differently sized \"dots\".\n"
    "{comment} `circle<..>` and `<..>square<..>` groups use circle and square \"dots\" respectively.\n"
    "{comment} `<..>_empty` draw non-filled \"dots\".\n"
    "{comment} `square<..>` does not use antialiasing, which may result in differently sized \"dots\". "
    "`aa_square<..>` uses antialiasing.\n"
    "braille_style: {}\n"
};
constexpr StringLiteral GitDrawingsConfig {
    "\n"
    "{comment} Controls how git branch drawings are rendered.\n"
    "{comment} `branch_style` controls style of connecting branches. "
    "May be on of `none`,`thin`,`thick`,`double. The default value is `thin`.\n"
    "{comment} `arc_style` controls style of arcs. May be one of `round`,`ellips`. "
    "`arc_style` has effect only if `branch_style` is `thin`. The default value is `round`.\n"
    "{comment} `merge_commit_style` controls style of merge commits. May be one of `solid`,`bullet`. The "
    "default value is `bullet`.\n"
    "git_drawings: \n"
    "    branch_style:       {}\n"
    "    arc_style:          {}\n"
    "    merge_commit_style: {}\n"
};

constexpr StringLiteral PlatformPluginWeb {
    "option allows you to override the auto-detected platform plugin to be loaded. You can specify values "
    "like `auto`, `xcb`, `cocoa`, `direct2d`, or `winrt` to determine the platform plugin. The default value "
    "is `auto`."
};

constexpr StringLiteral RendererWeb {
    "section contains configuration options related to the VT Renderer, which is responsible for rendering "
    "the terminal onto the screen. It includes the `backend` option to specify the Qt RHI graphics API, with "
    "possible values of `auto` (the native backend per platform), `OpenGL`, `vulkan`, `direct3d11`, "
    "`direct3d12`, `metal`, or `software`. The other options in this section control the "
    "tile mapping and caching for performance optimization. "
};

constexpr StringLiteral WordDelimitersWeb {
    "option defines the delimiters to be used when selecting words in the terminal. It is a string of "
    "characters that act as delimiters."
};

constexpr StringLiteral ExtendedWordDelimitersWeb {
    "option defines the delimiters to be used when selecting words in the second time. It is a string of "
    "characters that act as delimiters. By default word delimiters are used."
};

constexpr StringLiteral CommandPaletteRecentCountWeb {
    "option sets how many recently used commands the command palette pins above its alphabetical list. The "
    "palette is opened with the `OpenCommandPalette` action (bound to `Ctrl+Shift+P` by default), and the "
    "list of recent commands is remembered across restarts in the `command-history.yml` file beside your "
    "configuration file. Set it to `0` to turn the \"recently used\" section off entirely. The default value "
    "is `5`."
};

constexpr StringLiteral PTYReadBufferSizeWeb {
    "option sets the most bytes taken from the PTY in one read, and so the most parsed in one go. Larger "
    "batches parse output faster; smaller ones show a flood of it in smaller steps. It is an advanced option "
    "and should be used with caution. The default value is `{}`. A change applies to sessions opened "
    "afterwards."
};

constexpr StringLiteral PTYBufferObjectSizeWeb {
    "option sets the size in bytes per PTY Buffer Object. It is an advanced option for internal storage and "
    "should be changed carefully. The default value is `1048576`. A change applies to sessions opened "
    "afterwards."
};

constexpr StringLiteral DefaultProfilesWeb {
    "option determines the default profile to use in the terminal."
};

constexpr StringLiteral EarlyExitThresholdWeb {
    "option determines the early threshold time. If the shell ends earlier than the specified threshold "
    "without a successful exit status, additional message will be printed that the shell terminated too "
    "quickly and additional key press is required to close contour. A shell that exits successfully -- "
    "`exit` or Ctrl+D -- closes the window at once."
};

constexpr StringLiteral SpawnNewProcessWeb { "flag determines whether a new process should be spawned when "
                                             "creating a new terminal. The default value is `false`." };

constexpr StringLiteral GraphemeClusteringWeb {
    "Whether DEC mode 2027 starts out set. While set, a late codepoint may widen its grapheme "
    "cluster -- U+FE0F widens a cluster to two columns -- but a cluster never narrows again. "
    "While unset, a cell is measured by plain wcwidth() with no clustering. "
    "Applications may still toggle the mode at runtime. A change applies to sessions opened afterwards."
};

constexpr StringLiteral TextScalingMethodWeb {
    "How a glyph is enlarged for scaled text (kitty text sizing protocol, OSC 66). "
    "`stretch` magnifies the ordinary-size glyph: free to rasterize, no extra atlas entries, but "
    "softer at large scales. `rerasterize` asks the font for the glyph at the larger size: crisp at "
    "any scale, at the cost of a rasterization and an atlas entry per glyph and scale."
};

constexpr StringLiteral ReflowOnResizeWeb {
    "option controls whether or not the lines in the terminal should be reflowed when a resize event occurs. "
    "The default value is `true`. A change applies to sessions opened afterwards."
};

constexpr StringLiteral SnapToRowsWeb {
    "option aligns startup and manually resized window heights to complete terminal rows. "
    "The default is `false`. Applies to a single visible pane, including a zoomed split pane. "
    "Font metrics, window chrome and configured margins are taken into account. Windows snaps during "
    "resizing; macOS snaps when manual resizing ends; X11 uses window-manager size hints. Wayland has "
    "no resize-increment hint, so only startup sizing is supported there. Maximized and fullscreen "
    "sizes are unrestricted. Fractional scaling can leave less than one logical pixel of rounding slack. "
    "A change applies at the next startup or manual resize."
};

constexpr StringLiteral RememberWindowPositionWeb {
    "option remembers the last normal window position and monitor, without saving window size. "
    "The default is `true` on Windows and `false` on other platforms. Enable it to opt in on macOS "
    "and X11; Wayland leaves placement to the compositor. The first window restores its location "
    "at startup. The window-position.yml side file stores monitor-relative x/y coordinates and a "
    "zero-based monitor index. Disabling the option stops reading and writing position state."
};

constexpr StringLiteral TabSwitchOnHorizontalWheelWeb {
    "option controls whether a horizontal mouse wheel tilt (or a horizontal trackpad swipe) over the tab "
    "strip or the terminal switches to the previous/next tab. An application that has requested mouse "
    "reporting keeps receiving the horizontal wheel; holding the bypass modifier switches tabs anyway. An "
    "explicit `WheelLeft`/`WheelRight` entry in `input_mapping` takes precedence. The default value is "
    "`true`."
};

constexpr StringLiteral GuiConfigLockedWeb {
    "flag, when `true`, opens the in-app settings page read-only and prevents the GUI from writing any "
    "of its side files (`profiles/`, `colorschemes/`, `settings.yml`), so this hand-maintained "
    "configuration file stays the single source of truth. The default value is `false`."
};

constexpr StringLiteral ThemeWeb {
    "option selects the light/dark appearance of the GUI chrome (title bar, tab strip, command palette, "
    "settings pages, dialogs) independently of the operating system. Possible values are `terminal` "
    "(follow the terminal's background), `system` (follow the OS color scheme), `dark`, and `light`. "
    "The terminal grid keeps following the OS light/dark preference through its own color scheme. "
    "The default value is `terminal`."
};

constexpr StringLiteral BypassMouseProtocolModifiersWeb {
    "option specifies the keyboard modifier (e.g., Shift) that can be used to bypass the terminal's mouse "
    "protocol and select screen content."
};

constexpr StringLiteral MouseBlockSelectionModifiersWeb {
    "option determines the modifier (e.g., Control) that needs to be pressed to initiate block selection "
    "using the left mouse button."
};

constexpr StringLiteral OnMouseSelectionWeb {
    "option selects the action to perform when a text selection has been made. Possible values include "
    "`None`, `CopyToClipboard`, and `CopyToSelectionClipboard`."
};

constexpr StringLiteral LiveWeb { "option determines whether the instance should reload the configuration "
                                  "files whenever they change. The default value is `false`." };

constexpr StringLiteral ImagesWeb {
    "section contains configuration options related to inline images. It includes options like "
    "`sixel_scrolling`, `sixel_register_count`, and `good_image_protocol` to control various aspects of "
    "image rendering. The maximum image size is derived from the screen size; `max_width` and "
    "`max_height` are deprecated and ignored."
};

constexpr StringLiteral DaemonWeb {
    "section decides whether sessions are hosted by a daemon process, so they outlive the window.\n"
    "``` yaml\n"
    "daemon:\n"
    "  mode: auto\n"
    "  socket: \"\"\n"
    "  size_policy: latest\n"
    "  history_limit: 1000\n"
    "  restore:\n"
    "    scrollback: true\n"
    "    scrollback_lines: 1000\n"
    "    rerun_programs: []\n"
    "    interval: 30\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==mode== `in_process` keeps sessions in the GUI process, `auto` uses a "
    "daemon and falls back to in-process with a notice, `required` fails when no daemon can be reached.\n"
    ":octicons-horizontal-rule-16: ==socket== The daemon's socket path; empty selects the default.\n"
    ":octicons-horizontal-rule-16: ==size_policy== Which client's size wins when several are attached: "
    "`latest`, `smallest` or `largest`.\n"
    ":octicons-horizontal-rule-16: ==history_limit== Scrollback lines per session, `-1` for unlimited; "
    "unset uses the hosting profile's `history.limit`.\n"
    ":octicons-horizontal-rule-16: ==restore.scrollback== Whether a restored pane gets its scrollback back; "
    "the snapshot file may hold secrets that were on screen, and is readable by your user only.\n"
    ":octicons-horizontal-rule-16: ==restore.scrollback_lines== The most scrollback lines kept per pane, "
    "from `0` to `100000`; the live scrollback stays `history_limit`.\n"
    ":octicons-horizontal-rule-16: ==restore.rerun_programs== Programs a restored pane runs again when it "
    "was running one of them, such as `[vim, htop]`; nothing is re-run unless listed.\n"
    ":octicons-horizontal-rule-16: ==restore.interval== How often, in seconds, a changed workspace is "
    "written; at least `5`.\n"
};

constexpr StringLiteral ProfilesWeb { "All profiles inside configuration files share parent node `profiles`. "
                                      "To create profile you need to specify child node\n"
                                      "```yaml\n"
                                      "profiles:\n"
                                      "    main:\n"
                                      "    #default configuration\n"
                                      "    profile_for_windows:\n"
                                      "    # windows specific entries\n"
                                      "    profile_for_macos:\n"
                                      "    # macos specific entries\n"
                                      "\n"
                                      "```\n"
                                      "\n"
                                      "## Profile configuration\n"
                                      "\n" };

constexpr StringLiteral ShellWeb {

    "configuration section allows you to specify the process to be started inside the terminal. It provides "
    "flexibility to override the default login shell and supports logging in to a remote host.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    shell: \"/bin/bash\"\n"
    "    arguments: [\"some\", \"optional\", \"arguments\", \"for\", \"the\", \"shell\"]\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==arguments== (optional) Allows you to provide additional command-line "
    "arguments to the shell executable. These arguments will be passed to the shell when it is started "
    "inside the terminal.\n"
    "\n"
};

constexpr StringLiteral EscapeSandboxWeb {
    "option in the configuration file allows you to control the sandboxing behavior when the terminal is "
    "executed from within Flatpak. This configuration is relevant only if the terminal is running in a "
    "Flatpak environment.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    escape_sandbox: true\n"
    "```\n"
    "\n"

};

constexpr StringLiteral SshHostConfigWeb {
    "With this key, you can bypass local PTY and process execution and directly connect via TCP/IP to a "
    "remote SSH server.\n"
    "\n"
    "```yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    ssh:\n"
    "      host: remote-server.example.com\n"
    "      port: 22\n"
    "      user: \"CustomUserName\"\n"
    "      private_key: \"path/to/key\"\n"
    "      public_key: \"path/to/key.pub\"\n"
    "      known_hosts: \"~/.ssh/known_hosts\"\n"
    "      forward_agent: false\n"
    "```\n"
    "\n"
    "Note, only `host` option is required. Everything else is defaulted.\n"
    "Keep in mind, that the user's `~/.ssh/config` will be parsed with respect to the supported options "
    "above.\n"
    "These values can be overridden in the local Contour configuration as follows:\n"
    "\n"
    ":octicons-horizontal-rule-16: ==ssh.host== SSH server to establish the connection to.\n"
    ":octicons-horizontal-rule-16: ==ssh.port== SSH port (defaults to `22`). Only specify this value if it "
    "is deviating from the default value `22`.\n"
    ":octicons-horizontal-rule-16: ==ssh.private_key== Path to private key to use for key based "
    "authentication.\n"
    ":octicons-horizontal-rule-16: ==ssh.public_key== Path to public key that belongs to the private key. "
    "When using key based authentication, it depends on the underlying backend, if the public key is also "
    "required. OpenSSL for example does not require it.\n"
    ":octicons-horizontal-rule-16: ==ssh.known_hosts== Path to `known_hosts` file. This defaults to and "
    "usually is located in `~/.ssh/known_hosts`.\n"
    ":octicons-horizontal-rule-16: ==ssh.forward_agent== Boolean, indicating whether or not the local SSH "
    "auth agent should be requested to be forwarded. Note: this is currently not working due to an issue "
    "related to the underlying library being used, but is hopefully resolved soon.\n"
    "\n"
    "Note, custom environment variables may be passed as well, when connecting to an SSH server using this "
    "builtin-feature. Mind,\n"
    "that the SSH server is not required to accept all environment variables.\n"
    "\n"
    "If an OpenSSH server is used, have a look at the `AcceptEnv` configuration setting in the "
    "`sshd_config`\n"
    "configuration file on the remote SSH server, to configure what environment variables are permitted to "
    "be sent.\n"
    "\n"
};

constexpr StringLiteral MaximizedWeb {
    "configuration option determines whether the terminal window should be maximized when the specified "
    "profile is activated. Maximizing a window expands it to fill the entire available space on the screen, "
    "excluding the taskbar or other system elements.\n"
};

constexpr StringLiteral MarginsWeb {
    "Enforces a horizontal and vertical margin to respect on both sides of the terminal.\n"
    "This is particularly useful on operating systems (like MacOS) that draw the border frame into the main "
    "widgets space,\n"
    "or simply to create some artificial space to improve the user's focus.\n"
    "\n"
    "```yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    margins:\n"
    "      horizontal: 5\n"
    "      vertical: 0\n"
    "```\n"
    "\n"
};

constexpr StringLiteral BellWeb {
    "\n"
    "Configuration section permits tuning the behavior of the terminal bell.\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    bell:\n"
    "      sound: \"default\"\n"
    "      alert: false\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==sound== This option determines the sound of `BEL` (also `\a` or `0x07`) "
    "to `off` or `default` or sound generated by a file located at `path`. <br />\n"
    ":octicons-horizontal-rule-16: ==alert== This option determines whether or not a window alert will be "
    "raised each time a bell is ringed. Useful for tiling window managers like i3 or sway.\n"
    "\n"
};

constexpr StringLiteral WMClassWeb {
    "\n"
    "Configuration option defines the class part of the `WM_CLASS` property of the terminal window. The "
    "`WM_CLASS` property is a standard X11 property used to identify and classify windows by their class and "
    "instance.\n"
    "\n"
};

// NB: Documentation strings are std::vformat FORMAT templates (Writer::format), so every literal
// brace must be doubled — a bare `{WindowTitle}` here is an invalid arg-id that makes the whole
// `contour documentation configuration profile` subcommand throw before printing anything.
constexpr StringLiteral TabLabelWeb {
    "\n"
    "Default template for the text shown on each GUI tab. The value is a string with optional `{{Name}}` "
    "placeholders that are substituted per tab:\n"
    "\n"
    "- `{{WindowTitle}}` — the active pane's window/tab title.\n"
    "- `{{TabPosition}}` — the tab's 1-based position in the window.\n"
    "\n"
    "Literal text outside placeholders is kept verbatim, and an unknown placeholder is echoed verbatim (you "
    "see exactly the `{{Name}}` you typed). Renaming a tab overrides this default for that tab; the rename "
    "text "
    "may itself contain the "
    "same placeholders (so renaming a tab to `{{WindowTitle}}` tracks its title). A tab holding a split is "
    "shown as `Multiple panes`.\n"
    "\n"
};

constexpr StringLiteral PixelReportingWeb {
    "\n"
    "Selects the unit Contour reports pixel sizes to applications in, via `TIOCGWINSZ`\n"
    "(`ws_xpixel`/`ws_ypixel`), `CSI 14 t`, `CSI 16 t` and `XTSMGRAPHICS`.\n"
    "\n"
    "Applications derive the cell size by dividing a reported extent by the grid and size image\n"
    "canvases from it, so on a scaled display this decides how large an image an application draws\n"
    "into a given area.\n"
    "\n"
    "A report is only usable if that division is exact, and Contour's cell is the font's advance in\n"
    "**device** pixels — at a fractional scale it has no exact logical counterpart (17 / 1.75 =\n"
    "9.714). Valid values (ignore-case):\n"
    "\n"
    "- `Device` — report device pixels. The division is exact, so an image lands 1:1 on the display's\n"
    "  own pixels and is never resampled — gapless and sharp. The cost is that the application draws\n"
    "  as many times more pixels per axis as the display is scaled by. This is the default.\n"
    "- `Logical` — report logical pixels, matching the canvas size a terminal whose cell is integral\n"
    "  in logical pixels (Konsole, for one) would report, which is useful when comparing the two on\n"
    "  equal terms. Exact only where the scale divides both cell axes evenly (an unscaled display,\n"
    "  or a 20x40 cell at scale 2). Otherwise each axis is floored on its own, which changes the\n"
    "  cell's aspect ratio, and a full-page image is letterboxed by the difference — a few percent\n"
    "  of unpainted space down one side.\n"
    "\n"
    "On an unscaled display the two are identical.\n"
    "\n"
};

constexpr StringLiteral AccessibilityAnnouncementsWeb {
    "\n"
    "Whether events with no representation in the accessibility tree -- the bell, a desktop "
    "notification, a mode change -- are announced to assistive technology. Costs nothing while no "
    "assistive client is attached. See [Accessibility](../accessibility.md).\n"
    "\n"
};

constexpr StringLiteral AccessibilityCaretReportingWeb {
    "\n"
    "Whether the terminal reports where the caret is to assistive technology, which is how a screen "
    "magnifier follows what you are typing. Costs nothing while no assistive client is attached. Turn "
    "it off to rule the caret reports out when diagnosing input latency. See "
    "[Accessibility](../accessibility.md).\n"
    "\n"
};

constexpr StringLiteral HyperlinkHoverTooltipWeb {
    "\n"
    "Whether hovering an [OSC 8 hyperlink](../vt-extensions/index.md) shows a tooltip naming its "
    "target, so it can be read before it is followed. A local `file://` target is shown as a plain "
    "path.\n"
    "\n"
};

constexpr StringLiteral ProgressTimeoutWeb {
    "\n"
    "How long an application's progress indicator ([OSC 9;4](../vt-extensions/osc-9-4-progress.md)) "
    "stays on screen without being updated, in milliseconds.\n"
    "\n"
    "`0` (the default) disables expiry entirely: the indicator persists until the application clears "
    "it with state `0`, which is what the sequence specifies and what ConEmu and Windows Terminal do. "
    "The terminal cannot tell a long-running operation from an application that died holding one, so "
    "a non-zero value trades that fidelity for a guard against a bar left stranded on screen. "
    "Applications using the protocol are expected to refresh it about once a second, so a timeout "
    "well above that -- 15000 or so -- is the useful range.\n"
    "\n"
};

constexpr StringLiteral NotificationCloseTimeoutWeb {
    "\n"
    "How long after showing a desktop notification that asked to hear of its close (`c=1`) the terminal "
    "waits before assuming the desktop closed it, in milliseconds.\n"
    "\n"
    "This only applies where a close cannot be observed. Contour normally learns that a notification "
    "was dismissed or expired from the desktop itself and reports it back to the application as the "
    "OSC 99 `p=close` event. Inside a Flatpak sandbox that is impossible: the only reachable service "
    "is the `org.freedesktop.portal.Notification` portal, which has no close signal of any kind, so "
    "an application that asked for close events (`c=1`) would otherwise wait forever.\n"
    "\n"
    "A notification that states its own lifetime through `w=` is answered after that instead -- the "
    "application already said what it wanted. Only `w=-1`, \"whatever the desktop does\", falls back "
    "to this value.\n"
    "\n"
    "Two cases are deliberately never answered, because there is no moment at which an answer would "
    "be true: `0` here, which disables the assumption outright, and `w=0` on the notification, which "
    "asks for a popup that never expires on its own. An application that requested close events in "
    "either case is not told.\n"
    "\n"
    "A notification that asked for no close event -- a command's finish notice among them -- is never "
    "assumed closed: assuming it would only make it impossible to withdraw, and its click, from the "
    "desktop's notification history, would go unanswered.\n"
    "\n"
    "Read once per terminal session, so a changed value reaches sessions opened after the reload.\n"
    "\n"
    "Because the report is a timer rather than an observation, it is marked `untracked` on the wire, "
    "so an application can tell the two apart.\n"
    "\n"
};

constexpr StringLiteral TabBarPositionWeb {
    "\n"
    "Selects where the GUI tab strip (tab bar) is placed within the window. Valid values (ignore-case):\n"
    "\n"
    "- `Top` — the tab strip is drawn above the terminal content. This is the default.\n"
    "- `Bottom` — the tab strip is drawn below the terminal content.\n"
    "\n"
};

constexpr StringLiteral UiStyleWeb {
    "\n"
    "Selects how the application's own chrome is painted: the tab strip and title bar, and the "
    "controls (buttons, fields, menu entries, combo boxes) inside menus, popups and the settings page. "
    "This selects a *shape*, not a color: both styles take their colors from the "
    "operating system palette and follow the [`theme`](#theme) setting. Valid values (ignore-case):\n"
    "\n"
    "- `native` — platform-native GUI chrome. This is the default.\n"
    "- `terminal` — the chrome is quantized to whole character cells of a monospace font and its "
    "corners are squared off, so the window reads as one continuous TUI rather than as a GUI wrapped "
    "around a terminal. Tabs keep every GUI affordance (drag to reorder, tear off, rename, close, "
    "context menu); only their appearance changes.\n"
    "\n"
    "This is the recommended way to get the terminal look now that a window can hold split panes: the "
    "indicator status line's `{{Tabs}}` item still works, but it is drawn per pane, so a split window "
    "would show one tab list per pane instead of one per window.\n"
    "\n"
    "The Qt Quick Controls half of the style is selected once, before the first control is created, so "
    "a change takes effect on the next start rather than live.\n"
    "\n"
};

constexpr StringLiteral WindowControlStyleWeb {
    "\n"
    "Selects how the window's own minimize/maximize/close controls are drawn, and which side of the tab "
    "bar they sit on. These are drawn only while the native title bar is hidden "
    "([`show_title_bar`](profiles.md#show_title_bar) is `false`, the default), which is when the tab bar "
    "doubles as the title bar; with the native frame shown, the operating system draws them itself. "
    "Valid values (ignore-case):\n"
    "\n"
    "- `auto` — match the host: macOS gets `macos`, Windows gets `windows`, a KDE Plasma session gets "
    "`plasma`, and anything else gets `windows`. This is the default.\n"
    "- `windows` — trailing (right) edge, flush rectangular buttons in minimize/maximize/close order, "
    "with a red close hover.\n"
    "- `macos` — leading (left) edge, traffic lights in close/minimize/zoom order. The dots reveal their "
    "glyphs while the group is hovered and grey out while the window is inactive.\n"
    "- `plasma` — trailing (right) edge, like `windows` but with the circular hover fills of KDE "
    "Plasma's Breeze decoration.\n"
    "\n"
    "Independent of [`ui_style`](#ui_style): every value works in both the native and the terminal "
    "chrome, and takes its extents from whichever of those is active — so under `ui_style: terminal` the "
    "controls stay quantized to whole character cells, and the traffic lights are drawn as coloured "
    "cell-wide dots.\n"
    "\n"
    "Unlike `ui_style`, this applies immediately when changed from the settings page.\n"
    "\n"
};

constexpr StringLiteral UiFontFamilyWeb {
    "\n"
    "Font family the terminal-style chrome is drawn with. When empty (the default) the chrome inherits "
    "the regular font family of the profile the window is running (`-p`/`--profile`, else "
    "[`default_profile`](#default_profile_name)), which is what makes it match the terminal grid below it. "
    "Ignored while [`ui_style`](#ui_style) is `native`, where the chrome uses the platform UI font. "
    "Takes effect on the next start.\n"
    "\n"
};

constexpr StringLiteral UiFontSizeWeb {
    "\n"
    "Font size, in points, the terminal-style chrome is drawn with. When 0 (the default) the chrome "
    "inherits the font size of the profile the window is running. Ignored while "
    "[`ui_style`](#ui_style) is `native`. Takes effect on the next start.\n"
    "\n"
};

constexpr StringLiteral TabBarVisibilityWeb {
    "\n"
    "Selects when the GUI tab strip (tab bar) is shown. Valid values (ignore-case):\n"
    "\n"
    "- `Always` — the tab strip is always shown. This is the default.\n"
    "- `Never` — the tab strip is never shown.\n"
    "- `Multiple` — the tab strip is shown only when the window has more than one tab, and hidden while a "
    "single tab remains.\n"
    "\n"
};

constexpr StringLiteral TerminalIdWeb {
    "\n"
    "configuration option allows you to specify the terminal type that will be advertised by the terminal "
    "emulator. The terminal type indicates the set of capabilities and features that the terminal supports, "
    "enabling compatibility with different applications and systems.\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    terminal_id: VT525\n"
    "```\n"
    "\n"
};

constexpr StringLiteral TerminalSizeWeb {
    "configuration option allows you to specify the initial size of the terminal window in terms of the "
    "number of columns and lines.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    terminal_size:\n"
    "      columns: 80\n"
    "      lines: 25\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==columns== This option specifies the number of columns (characters) to "
    "be displayed in the terminal window. In the provided example, the value is set to 80. <br/>\n"
    ":octicons-horizontal-rule-16: ==lines== This option specifies the number of lines to be displayed in "
    "the terminal window. In the provided example, the value is set to 25. <br/>\n"
    "\n"
};

constexpr StringLiteral HistoryWeb {
    "configuration allows you to customize the behavior and settings related to the terminal's history, "
    "including the number of preserved lines, auto-scrolling, and scroll events.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    history:\n"
    "      limit: 1000\n"
    "      hard_limit: 2000\n"
    "      auto_scroll_on_update: false\n"
    "      scroll_multiplier: 3\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==limit== This option specifies the number of lines to preserve in the "
    "terminal's history. A value of -1 indicates unlimited history, meaning that all lines are preserved. In "
    "the provided example, the limit is set to 1000. <br/>\n"
    ":octicons-horizontal-rule-16: ==hard_limit== The upper bound on the scrollback, and what makes "
    "eviction command-aware. Anything above `limit` is headroom: the buffer is allowed to grow into it, "
    "and when it fills, whole (prompt, output) blocks are dropped from the top rather than a slice taken "
    "wherever `limit` fell. The oldest line kept is then a prompt with its command's output intact, "
    "instead of an orphaned tail. Block boundaries come from the "
    "[OSC 133](../vt-extensions/osc-133-shell-integration.md) marks a shell with shell integration "
    "emits, so with no such shell -- or for a single command whose output is larger than the whole "
    "headroom -- there is no boundary to honour and the buffer simply bounds at this value, evicting "
    "line by line as before. Values below `limit` are raised to it, which disables the feature; -1 "
    "means infinite, and an infinite `limit` makes this infinite too since nothing is ever evicted. "
    "Defaults to `limit`. <br/>\n"
    ":octicons-horizontal-rule-16: ==auto_scroll_on_update== This boolean option determines whether "
    "new output brings a view scrolled back into the history down to the bottom. Only output that adds "
    "lines counts: a prompt, a progress bar or a clock repainted in place does not, so a shell that "
    "redraws its prompt on a timer leaves the view where you put it. If set to false, the terminal "
    "maintains the current scroll position until you type, which always brings the view down. Defaults "
    "to false. <br/>\n"
    ":octicons-horizontal-rule-16: ==scroll_multiplier== This option defines the number of lines to scroll "
    "when the ScrollUp or ScrollDown events occur. By default, scrolling up or down moves three lines at a "
    "time. You can adjust this value as needed. In the provided example, scroll_multiplier is set to 3. "
    "<br/>\n"
    "\n"
};

constexpr StringLiteral OscContextWeb {
    "configuration controls whether Contour reads OSC 3008, systemd's hierarchical context signalling "
    "(UAPI.15): the nested stack of contexts a shell, `run0`, `ssh` or a container runtime opens around "
    "whatever runs inside it.\n"
    "``` yaml\n"
    "osc_context:\n"
    "  enabled: true\n"
    "  max_depth: 16\n"
    "  max_retained_contexts: 256\n"
    "  derive_markers: when_alone\n"
    "  tinting: boundaries\n"
    "```\n"
    "systemd 258 and later ship a shell snippet that emits these sequences around every interactive "
    "command with no configuration at all, so on such a system Contour learns command boundaries, exit "
    "statuses (including whether a command crashed or was interrupted, which OSC 133 cannot express) "
    "and working directories without any shell integration being installed.\n"
    ":octicons-horizontal-rule-16: ==enabled== Whether the sequences are read at all. When false "
    "nothing is tracked, nothing is derived and nothing is tinted. <br/>\n"
    ":octicons-horizontal-rule-16: ==max_depth== How deep the context stack may grow. Beyond it, "
    "further contexts are ignored -- the NEWER ones, so that a program cannot push the context "
    "established above it out of the stack. <br/>\n"
    ":octicons-horizontal-rule-16: ==max_retained_contexts== How many contexts are remembered for "
    "scrolled-back output that still refers to them. Older ones are forgotten, and output belonging to "
    "them simply stops being attributed. <br/>\n"
    ":octicons-horizontal-rule-16: ==derive_markers== Whether OSC 3008 may stand in for a shell "
    "integration that is not installed. `when_alone` derives prompt and command marks from it only "
    "while OSC 133 has said nothing; once a shell integration speaks, it owns the marks for the rest of "
    "the session. `never` leaves marks entirely to OSC 133. <br/>\n"
    ":octicons-horizontal-rule-16: ==tinting== Which contexts may tint the page background. The "
    "colours themselves come from the colour scheme's `tint:` map, which no shipped scheme sets, so "
    "nothing is tinted until you choose a colour there. Note that a context is something the session "
    "**announced**: anything that can write to your terminal can announce one, or fail to announce one, "
    "so the absence of a tint is not a statement that nothing is elevated. <br/>\n"
};

constexpr StringLiteral FoldingWeb {
    "configuration controls output folding: collapsing a finished command's output down to the prompt "
    "line it was entered at, and expanding it again.\n"
    "``` yaml\n"
    "folding:\n"
    "  enabled: true\n"
    "  show_markers: true\n"
    "  auto_collapse_on_new_command: false\n"
    "  on_jump_into_fold: expand\n"
    "```\n"
    "Folding is driven entirely by the OSC 133 semantic marks a shell with shell integration emits, so "
    "no additional escape sequence is involved and any shell that already speaks OSC 133 works. Only "
    "FINISHED commands fold, which is why the prompt you are typing at never does.\n"
    ":octicons-horizontal-rule-16: ==enabled== Whether folding is available at all. When false the "
    "actions below do nothing and no fold markers are drawn. <br/>\n"
    ":octicons-horizontal-rule-16: ==show_markers== Whether the gutter's block column, left of the grid, "
    "draws fold markers: a boxed minus on a foldable prompt line whose output is showing, a boxed plus "
    "once it is collapsed, and a bar down the side of the block joining the two. Clicking anywhere on "
    "that column toggles the fold. Note that the column costs one column of terminal width; it is "
    "reserved while this, `gutter.exit_status` or `gutter.user_marks` is on. <br/>\n"
    ":octicons-horizontal-rule-16: ==auto_collapse_on_new_command== Whether a command's output is "
    "collapsed automatically as soon as the next prompt appears, so that only the command you are "
    "running now is shown in full. <br/>\n"
    ":octicons-horizontal-rule-16: ==on_jump_into_fold== What a Vi-mode jump that targets a line "
    "inside a collapsed block does: `expand` opens the block so the cursor lands on what it was "
    "aiming at, `skip` leaves it collapsed and stops at its prompt line instead. Plain motions "
    "(`j`, `k`, `{{`, `}}`, ++ctrl+d++, ++ctrl+u++) always step over a collapsed block regardless of "
    "this setting. <br/>\n"
    "\n"
    "The actions ==CollapseAllFolds==, ==ExpandAllFolds==, ==CollapseLastFold==, ==ToggleLastFold== and "
    "==ToggleFold== can be bound to keys; ==ScrollMarkUp== and ==ScrollMarkDown== already move the "
    "viewport from one prompt to the next.\n"
    "\n"
};

constexpr StringLiteral GutterWeb {
    "configuration controls the gutter: the strip left of the grid that carries per-line timestamps, "
    "line numbers and the block column.\n"
    "``` yaml\n"
    "gutter:\n"
    "  exit_status: true\n"
    "  user_marks: true\n"
    "  block_column_width: 2\n"
    "  bold_on_hover: true\n"
    "  line_numbers: off\n"
    "  line_number_width: 6\n"
    "  timestamps: false\n"
    "  timestamp_format: '%H:%M:%S'\n"
    "```\n"
    "The gutter's width follows this configuration and never its content, so a growing line number "
    "cannot resize the page in the middle of a command's output. The block column is reserved while "
    "fold markers (`folding.show_markers`), exit status or user marks are on, `block_column_width` cells "
    "wide.\n"
    ":octicons-horizontal-rule-16: ==exit_status== Colour each command's fold controls by how it ended "
    "-- the colour scheme's `block_status` colours -- draw a triangle beside a running command, and a "
    "dot beside a finished one that shows no fold control: one with nothing to fold, or any one while "
    "fold markers are off. Hovering the block column shows the exit code, duration, start time, working "
    "directory and command line. <br/>\n"
    ":octicons-horizontal-rule-16: ==user_marks== Draw a diamond beside every row marked with vi-mode "
    "`mm`. <br/>\n"
    ":octicons-horizontal-rule-16: ==block_column_width== The block column's width in cells, 1 or 2. At "
    "2, the default, its fold controls and marks are drawn across both cells at twice the size, and a "
    "click anywhere in the column -- the blank strip left of it included -- lands on it. <br/>\n"
    ":octicons-horizontal-rule-16: ==bold_on_hover== Draw the fold under the pointer -- its line, its box "
    "and its end -- one stroke heavier, as well as at full strength: a failed command's, already heavy, "
    "heavier still. Its colours are the color scheme's `fold_marker.hover` and `block_status_hover`. "
    "<br/>\n"
    ":octicons-horizontal-rule-16: ==line_numbers== `off`, `absolute` (every row's number, counted from "
    "the session's first row, so the numbers stay put while history scrolls away), `relative` (every "
    "row's distance from the cursor row, counting a collapsed block as one row -- exactly what `j` and "
    "`k` count) or `hybrid` (the cursor row's own number, every other row's distance), as vim's `nu` and "
    "`rnu`. In vi mode the distance counts from the vi cursor. <br/>\n"
    ":octicons-horizontal-rule-16: ==line_number_width== Columns the line numbers take, 3 to 10. A "
    "longer number shows its last digits after an ellipsis. <br/>\n"
    ":octicons-horizontal-rule-16: ==timestamps== Show the time each row was first reached. A row shows "
    "it only where it differs from the row above, so a long output does not repeat one second. <br/>\n"
    ":octicons-horizontal-rule-16: ==timestamp_format== A `std::format` chrono specification in local "
    "time, such as `%H:%M` or `%a %H:%M:%S`. It must begin with a `%` conversion, so a leading literal "
    "such as `[%H:%M]` is refused, and so is a leading width or alignment such as `8%H` or `>%H`, which "
    "`std::format` would pad the timestamp with. Every character it spells must take exactly one "
    "column, so a double-width character (a CJK ideograph, say) or a combining mark is refused as well. "
    "A format that cannot be drawn is reported when the configuration loads, and the default is kept. "
    "<br/>\n"
    "\n"
    "In vi normal mode, `za`, `zo`, `zc`, `zM` and `zR` toggle, open and close the block under the "
    "cursor, and close or open all of them, as in vim.\n"
};

constexpr StringLiteral CommandBlocksWeb {
    "configuration controls what the terminal remembers about each shell command it saw run.\n"
    "``` yaml\n"
    "command_blocks:\n"
    "  max_records: 1000\n"
    "  pager: less -R\n"
    "  pager_format: sgr\n"
    "```\n"
    "A command block is one prompt and the output of the command run at it, delimited by the OSC 133 "
    "marks a shell with shell integration emits -- or, without one, by systemd's OSC 3008 contexts. For "
    "each block the terminal keeps its command line, exit status, duration, start time and working "
    "directory, even after the block's rows have left the scrollback.\n"
    ":octicons-horizontal-rule-16: ==max_records== How many blocks one session remembers; the oldest are "
    "forgotten first. At least 1, at most 1000000. A change applies to sessions opened afterwards. <br/>\n"
    ":octicons-horizontal-rule-16: ==pager== The program the OpenCommandOutput action (\"Open Output in "
    "Pager\", Ctrl+Shift+G) opens a command's output with when its binding names none. It is read as a "
    "profile's `shell:` is: a string naming a program runs as it is; otherwise its first word is the "
    "program and the rest its arguments -- on Windows passed on as written, so `C:\\Tools\\less.exe -R` "
    "needs no quoting. A binding's own `program:` is read the same way. The output is written to a "
    "private file that is appended as the last argument and deleted when the pane closes. Defaults to "
    "`less -R` (`more` on Windows). <br/>\n"
    ":octicons-horizontal-rule-16: ==pager_format== Whether the pager renders colours: `sgr` writes the "
    "output with its SGR sequences, `plain` as text only. Defaults to `sgr`, which both `less -R` and "
    "Windows `more` (inside the terminal's pseudo console) render. A binding's own `format:` wins, and a "
    "binding that names its own `program:` gets `sgr`. <br/>\n"
};

constexpr StringLiteral ScrollbarWeb {
    "configuration allows you to customize the appearance and behavior of the visual scrollbar in the "
    "terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    scrollbar:\n"
    "      position: Right\n"
    "      hide_in_alt_screen: true\n"
    "      marks: [failures, commands, user_marks]\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==position==  This option specifies the position of the scrollbar in the "
    "terminal window. It can be set to one of the following values: Left, Right, Hidden. The default is "
    "Right for new configurations; a `contour.yml` written by an earlier version keeps `Hidden`, so set "
    "`position: Right` there to see the marks. Hidden turns the scrollbar, and with it the command marks, "
    "off. The scrollbar floats over the edge of the terminal, takes no column from it, and is shown only "
    "while there is history to scroll through. <br/>\n"
    ":octicons-horizontal-rule-16: ==hide_in_alt_screen== This boolean option determines whether the "
    "scrollbar should be hidden when the terminal is in the alternate screen mode. If set to true, the "
    "scrollbar will be hidden when the terminal switches to the alternate screen. If set to false, the "
    "scrollbar will remain visible even in the alternate screen mode. <br/>\n"
    ":octicons-horizontal-rule-16: ==marks== Which marks the scrollbar shows as thin ticks; clicking a tick "
    "scrolls its line to the top of the view, opening it if it is folded. A list of `failures` (commands "
    "that exited with an error), `commands` (every other command, finished or still running) and "
    "`user_marks` (lines marked with `mm` in vi normal mode); `[]` shows none. Command ticks need the OSC "
    "133 marks a shell with shell integration emits -- or, without one, systemd's OSC 3008 contexts. "
    "Ticks are hidden whenever the scrollbar is. <br/>\n"
    "\n"
};

constexpr StringLiteral NotifyOnCommandFinishWeb {
    "configuration tells you when a command you are not watching finishes: a desktop notification, the "
    "bell, or both.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    notify_on_command_finish:\n"
    "      when: unfocused\n"
    "      min_duration: 10\n"
    "      outcome: any\n"
    "      action: notify\n"
    "      clear_on: focus\n"
    "```\n"
    "Commands are known from the OSC 133 marks shell integration emits; a shell without it reports none, "
    "so nothing is notified. A notification reads `✓ make finished`, `✗ make failed (exit 2)` or "
    "`✗ make killed (SIGSEGV)`, with how long it took, where it ran and the tab's name below. A newer "
    "finish in the same pane replaces the older notification, and clicking it brings the pane forward "
    "where the desktop reports clicks.\n"
    ":octicons-horizontal-rule-16: ==when== `never`, `unfocused` (unless it is the focused pane of the "
    "active window), `hidden` (only while the pane is off screen: another tab is active, a sibling pane "
    "is zoomed, or the window is minimised) or `always`. <br/>\n"
    ":octicons-horizontal-rule-16: ==min_duration== Commands that ran for fewer seconds than this never "
    "notify. <br/>\n"
    ":octicons-horizontal-rule-16: ==outcome== `any`, or `failure` for commands that exited non-zero or "
    "died of a signal. <br/>\n"
    ":octicons-horizontal-rule-16: ==action== `notify` (a desktop notification), `bell` (the profile's "
    "bell sound and window alert) or `notify_bell` (both). <br/>\n"
    ":octicons-horizontal-rule-16: ==clear_on== When a notification still on screen is withdrawn: "
    "`focus` (the pane is focused), `next` (the pane's next command starts) or `never`. Only a desktop "
    "notification can be withdrawn, which is what Linux shows; on Windows and macOS the notice is a tray "
    "message, which stays until it times out whatever `clear_on` says. <br/>\n"
    "\n"
};

constexpr StringLiteral StickyHeaderWeb {
    "configuration pins the command line of the block you are reading over the top row of the terminal, "
    "once that command has scrolled out of view above its own output.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    sticky_header:\n"
    "      mode: scrolled\n"
    "      show_evicted: true\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==mode== When the header appears: `never`, `scrolled` (only while the "
    "viewport is scrolled back into history; the default) or `always` (also while following live output, so "
    "a long build keeps its command in view). It needs prompt marks (OSC 133;A), never appears on the "
    "alternate screen, shows the command's exit status and duration on its right, and a click on it scrolls "
    "to the command. A shell that marks only where its commands run (`;C`, `;D`) gets no header. <br/>\n"
    ":octicons-horizontal-rule-16: ==show_evicted== Whether the header still names a command whose own line "
    "has already been dropped from the scrollback, as `⋯ <command> — earlier output evicted`, so a long "
    "output does not lose its header when its prompt is evicted. At the very top of the scrollback it shows "
    "whatever `mode` says (even `never`); anywhere else it shows wherever `mode` would show a header. <br/>\n"
    "\n"
};

constexpr StringLiteral MouseWeb {
    "configuration allows you to control the behavior of the mouse in the terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    mouse:\n"
    "      hide_while_typing: true\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==hide_while_typing== This boolean option determines whether the mouse "
    "cursor should be hidden while typing in the terminal. When set to true, the mouse cursor will be hidden "
    "when you start typing. When set to false, the mouse cursor will remain visible while typing. <br/>\n"
    "\n"
};

constexpr StringLiteral PermissionsWeb {
    "configuration allows you to control the access permissions for specific VT sequences in the terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    permissions:\n"
    "      change_font: ask\n"
    "      capture_buffer: ask\n"
    "      display_host_writable_statusline: ask\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==change_font== This option determines the access permission for changing "
    "the font using the VT sequence `OSC 50 ; Pt ST`. The possible values are: allow, deny, ask. <br/>\n"
    ":octicons-horizontal-rule-16: ==capture_buffer== This option determines the access permission for "
    "capturing the screen buffer using the VT sequence `CSI > Pm ; Ps ; Pc ST`. The response can be read "
    "from stdin as the sequence `OSC 314 ; <screen capture> ST`. The possible values are: allow, deny, "
    "ask.<br/>\n"
    ":octicons-horizontal-rule-16: ==display_host_writable_statusline== This option determines the access "
    "permission for displaying the \"Host Writable Statusline\" programmatically using the VT sequence "
    "`DECSSDT 2`. The possible values are: allow, deny, ask. <br/>\n"
    "\n"
};

constexpr StringLiteral HighlightDoubleClickerWordWeb {
    "configuration option enables the highlighting of a word and its matches when double-clicked on the "
    "primary screen in the terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    highlight_word_and_matches_on_double_click: true\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==change_font==  When this option is enabled (true), the following "
    "behavior occurs: <br/>\n"
    "    - Double-clicking on a word in the primary screen will select and highlight the double-clicked "
    "word. <br/>\n"
    "    - Additionally, all other occurrences of the same word will also be highlighted without being "
    "selected. <br/>\n"
    "    - This feature is implemented by initiating a search for the double-clicked word. <br/>\n"
    "    - You can use the FocusNextSearchMatch and FocusPreviousSearchMatch actions to navigate to the next "
    "or previous occurrence of the same word, even if it is outside the current viewport. <br/>\n"
    "\n"
};

constexpr StringLiteral FontsWeb {
    "section of the configuration allows you to customize the font settings for the terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    font:\n"
    "      size: 12\n"
    "      locator: native\n"
    "      text_shaping:\n"
    "        engine: native\n"
    "      builtin_box_drawing: true\n"
    "      max_fallback_count: 16\n"
    "      render_mode: gray\n"
    "      strict_spacing: false\n"
    "      regular:\n"
    "        family: \"monospace\"\n"
    "        weight: regular\n"
    "        slant: normal\n"
    "        features: []\n"
    "      emoji: \"emoji\"\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==size== Specifies the initial font size in pixels. The default value is "
    "12. <br/>\n"
    ":octicons-horizontal-rule-16: ==locator==  Determines the font locator engine to use for locating font "
    "files and font fallback. Possible values are `native` and `mock`.<br/> `native` will use the "
    "operating-system native font location service (e.g. CoreText on macOS and DirectWrite on Windows), "
    "whereas `mock` is solely used for testing the software (not recommended by end-users)<br/>\n"
    ":octicons-horizontal-rule-16: ==text_shaping.engine== Selects the text shaping and font rendering "
    "engine. Supported values are native, DirectWrite, CoreText, and OpenShaper.  <br/>\n"
    ":octicons-horizontal-rule-16: ==builtin_box_drawing== Specifies whether to use built-in textures for "
    "pixel-perfect box drawing. If disabled, the font's provided box drawing characters will be used. The "
    "default value is true.<br/>\n"
    ":octicons-horizontal-rule-16: ==max_fallback_count== Maximum number of fallback fonts per font style. "
    "Set to -1 for unlimited, 0 to disable fallbacks entirely. The default value is 16.<br/>\n"
    ":octicons-horizontal-rule-16: ==render_mode== Specifies the font render mode, which tells the font "
    "rasterizer engine what rendering technique to use. Available modes are lcd, light, gray, color and "
    "monochrome.  <br/>\n"
    ":octicons-horizontal-rule-16: ==strict_spacing== Indicates whether only monospace fonts should be "
    "included in the font and font fallback list. The default value is false.  <br/>\n"
    ":octicons-horizontal-rule-16: ==regular== Defines the regular font style with the following parameters: "
    " <br/>\n"
    ":octicons-horizontal-rule-16: ==regular.family==  Specifies the font family name, such as "
    "\"monospace\", \"Courier New\", or \"Fira Code\". <br/>\n"
    ":octicons-horizontal-rule-16: ==regular.weight==  Specifies the font weight, such as thin, extra_light, "
    "light, demilight, book, normal, medium, demibold, bold, extra_bold, black, or extra_black. <br/>\n"
    ":octicons-horizontal-rule-16: ==regular.slant==  Specifies the font slant, which can be normal, italic, "
    "or oblique.\n"
    ":octicons-horizontal-rule-16: ==regular.features== Sets optional font features to be enabled. This is "
    "usually a 4-letter code, such as ss01 or ss02. Refer to your font's documentation for supported "
    "features. By default, no features are enabled. <br/>\n"
    ":octicons-horizontal-rule-16: ==emoji== Specifies the font to be used for displaying Unicode symbols "
    "with emoji presentation. The default value is \"emoji\". <br/>\n"
    "\n"

};

constexpr StringLiteral DrawBoldTextWithBrightColorsWeb {
    "Specifies whether bold text should be rendered in bright colors for indexed colors. If disabled, normal "
    "colors will be used for bold text. The default value is false.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "   draw_bold_text_with_bright_colors: false\n"
    "```\n"
    "\n"
};

constexpr StringLiteral ModeInsertWeb {
    "section of the configuration allows you to customize the appearance and behavior of the terminal "
    "cursor.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "   cursor:\n"
    "    shape: \"bar\"\n"
    "    blinking: false\n"
    "    blinking_interval: 500\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==shape== Specifies the shape of the cursor. You can choose from the "
    "following options: <br/>\n"
    "-block: A filled rectangle. <br/>\n"
    "-rectangle: Just the outline of a block. <br/>\n"
    "-underscore: A line under the text. <br/>\n"
    "-bar: The well-known i-Beam cursor. <br/>\n"
    ":octicons-horizontal-rule-16: ==blinking== Determines whether the cursor should blink over time. If set "
    "to true, the cursor will blink; if set to false, the cursor will remain static. <br/>\n"
    ":octicons-horizontal-rule-16: ==blinking_interval== Specifies the blinking interval in milliseconds. "
    "This value defines how quickly the cursor alternates between being visible and invisible when blinking "
    "is enabled. <br/>\n"
    "\n"
};

constexpr StringLiteral ModeNormalWeb { "section in the configuration allows you to customize the appearance "
                                        "and behavior of the cursor specifically in vi-like normal mode.\n"
                                        "``` yaml\n"
                                        "profiles:\n"
                                        "  profile_name:\n"
                                        "    normal_mode:\n"
                                        "      cursor:\n"
                                        "        shape: block\n"
                                        "        blinking: false\n"
                                        "        blinking_interval: 500\n"
                                        "```\n"
                                        "\n" };
constexpr StringLiteral ModeVisualWeb {
    "section in the configuration allows you to customize the appearance and behavior of the cursor "
    "specifically in vi-like normal mode.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    visual_mode:\n"
    "      cursor:\n"
    "        shape: block\n"
    "        blinking: false\n"
    "        blinking_interval: 500\n"
    "```\n"
    "\n"
};

constexpr StringLiteral HighlightTimeoutWeb {
    "option in the configuration determines the duration in milliseconds for which the yank highlight is "
    "shown in vi mode. After yanking (copying) text in vi mode, the yanked text is typically highlighted "
    "momentarily to provide visual feedback. This configuration option allows you to specify the duration of "
    "this highlight.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    vi_mode_highlight_timeout: 300\n"
    "```\n"
    "\n"
};

constexpr StringLiteral ModalCursorScrollOffWeb {
    "option in the configuration sets the scrolloff value for cursor movements in normal and visual (block) "
    "modes. The scrolloff value determines the minimum number of lines to keep visible above and below the "
    "cursor when scrolling. In other words, it controls the amount of margin or padding around the cursor "
    "during scrolling operations.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    vi_mode_scrolloff: 8\n"
    "```\n"
    "\n"
};

constexpr StringLiteral StatusLineWeb {

    "section in the configuration file allows you to customize the behavior and appearance of the status "
    "line in the terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    status_line:\n"
    "      display: none\n"
    "      position: bottom\n"
    "      sync_to_window_title: false\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==display== specifies whether the status line should be shown or not. The "
    "possible values are none (status line is not shown) and indicator (status line is shown). In the "
    "example, the status line is set to none, meaning it will not be displayed initially. <br/>\n"
    ":octicons-horizontal-rule-16: ==position== determines the placement of the status line. It can be set "
    "to top or bottom. In the example, the status line is set to bottom, indicating that it will appear at "
    "the bottom of the terminal window if enabled. <br/>\n"
    ":octicons-horizontal-rule-16: ==sync_to_window_title== controls whether the window title should be "
    "synchronized with the Host Writable status line. If the Host Writable status line is denied, enabling "
    "this option will update the window title accordingly. By default, this option is set to false. <br/>\n"
    ":octicons-horizontal-rule-16: ==extension== names an entry of the profile's `extensions` section that "
    "takes the whole status line over. By default it is null, which keeps the template-driven status line. "
    "<br/>\n"
    "\n"
};

constexpr StringLiteral ExtensionsWeb {
    "section in the configuration file declares the WebAssembly extensions that may draw into the status "
    "line "
    "of a profile. None is configured by default.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    extensions:\n"
    "      git:    {{ run_command: ask }}\n"
    "      plasma: {{ config: {{ speed: \"2\" }} }}\n"
    "      mine:   {{ path: ~/wasm/mine.wasm, run_command: deny }}\n"
    "    status_line:\n"
    "      extension: plasma\n"
    "      indicator:\n"
    "        right: \"{{Extension:git}} | {{Clock:Bold}}\"\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==path== is the module to load; a leading `~` is the home directory. "
    "Without it, the entry's name selects an extension bundled with Contour. <br/>\n"
    ":octicons-horizontal-rule-16: ==config== holds text values the extension reads by key. <br/>\n"
    ":octicons-horizontal-rule-16: ==run_command== decides whether the extension may run the commands its "
    "manifest declares: `allow`, `deny` or `ask`. The default is `ask`. <br/>\n"
    "\n"
    "An extension is used by naming it in a status line template as `{{Extension:name}}`, or, with "
    "`status_line.extension`, by letting it take the whole line over.\n"
    "\n"
};

constexpr StringLiteral BackgroundWeb {
    "section in the configuration file allows you to customize the background settings for the terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    background:\n"
    "      opacity: 1.0\n"
    "      blur: false\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==opacity== specifies the background opacity to use. The value ranges "
    "from 0.0 to 1.0, where 0.0 represents fully transparent and 1.0 represents fully opaque. You can adjust "
    "this value to control the transparency level of the terminal's background. <br/>\n"
    ":octicons-horizontal-rule-16: ==blur== determines whether the transparent background should be blurred "
    "on platforms that support it. Currently, only Windows 10 is supported for background blurring. By "
    "default, this option is set to false, meaning no background blurring will be applied. <br/>\n"
    "\n"
};

constexpr StringLiteral ColorsWeb {

    "section in the configuration file allows you to specify the colorscheme to use for the terminal. You "
    "can use one of the predefined color palettes as a setting for colors entry.\n"
    "List of predefined colorschemes: `contour`(default colors), `monokai`, `one-light`, `one-dark`, "
    "`gruvbox-light`, `gruvbox-dark`, `solarized-light`, `solarized-dark`, `papercolor-light`, "
    "`papercolor-dark`.\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    colors: \" default \"\n"
    "```\n"
    "\n"
    "To make the terminal's color scheme dependant on OS appearance (dark and light "
    "mode) settings,\n"
    "you need to specify two color schemes:\n"
    "\n"
    "```yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    colors:\n"
    "      dark: \"some_dark_scheme_name\"\n"
    "      light: \"some_light_scheme_name\"\n"
    "```\n"
    "\n"
    "With this, the terminal will use the color scheme as specified in `dark` when OS "
    "dark mode is on,\n"
    "and `light`'s color scheme otherwise.\n"
    "\n"
    "A configuration reload applies the colors to running sessions too, replacing any a program set "
    "there (OSC 4, 10, 11 and friends), just as switching to another profile does.\n"
    "\n"

};

constexpr StringLiteral HyperlinkDecorationWeb {
    "section in the configuration file allows you to configure the styling and colorization of hyperlinks "
    "when they are displayed in the terminal and when they are hovered over by the cursor.\n"
    "\n"
    "Possible values: underline, dotted-underline, double-underline, curly-underline, dashed-underline, "
    "overline, crossed-out, framed, encircle (if implemented)\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    hyperlink_decoration:\n"
    "      mode: always\n"
    "      normal: dotted-underline\n"
    "      hover: underline\n"
    "```\n"
    "\n"
    ":octicons-horizontal-rule-16: ==mode== when links are decorated: `always`, `hover` (only the one under "
    "the pointer), `modifier` (all of them while the follow modifier is held), `never`. <br/>\n"

};

constexpr StringLiteral LinkDetectionWeb {
    "section decides which links are found automatically in printed text, per screen. OSC 8 hyperlinks "
    "are always links, and hint mode always offers everything.\n"
    "`primary_screen` is the normal shell screen; `other_screens` is the alternate screen (vim, less, "
    "htop, ...) and every further page.\n"
    "`urls` also covers your own patterns, unless they open a `file:` URL, which follows `file_paths`.\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    link_detection:\n"
    "      primary_screen:\n"
    "        urls: true\n"
    "        file_paths: true\n"
    "      other_screens:\n"
    "        urls: true\n"
    "        file_paths: false\n"
    "```\n"
    "\n"
};

// Read through std::format like the configuration text, so a literal brace is doubled.
constexpr StringLiteral FileOpenerWeb {
    "opens detected file paths on Ctrl+click and with hint mode's Open.\n"
    "`command` is a command line with `{{path}}`, `{{line}}` and `{{column}}`, split into arguments\n"
    "once, with no shell involved; its program is written out, without a placeholder.\n"
    "`placement` is `detached` (default), `split` or `tab`.\n"
    "Without a command, a file opens with the system's default application; a directory always opens in the\n"
    "file manager.\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    file_opener:\n"
    "      command: 'code --goto {{path}}:{{line}}:{{column}}'\n"
    "      placement: detached\n"
    "```\n"
    "\n"
};

constexpr StringLiteral OptionKeyAsAltConfig {
    "{comment} Tells Contour how to handle Option-Key events on MacOS.\n"
    "{comment} This value is ignored on other platforms.\n"
    "{comment}\n"
    "{comment} Default: false\n"
    "option_as_alt: {}\n"
    "\n"
};

constexpr StringLiteral OptionKeyAsAltWeb {
    "section tells Contour how to handle Option-Key events on MacOS.\n"
    "This value is ignored on other platforms.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    option_as_alt: false\n"
    "```\n"
    "\n"
};

constexpr StringLiteral FullscreenWeb {
    "configuration option determines whether the terminal's screen should be put into fullscreen mode when "
    "the terminal profile is activated. Fullscreen mode expands the terminal window to occupy the entire "
    "screen, providing a distraction-free environment for your terminal sessions.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    fullscreen: false\n"
    "```\n"
    "\n"
};

constexpr StringLiteral ShowTitleBarWeb { "configuration option determines whether or not the title bar will "
                                          "be shown when the terminal profile is activated.\n"
                                          "``` yaml\n"
                                          "profiles:\n"
                                          "  profile_name:\n"
                                          "    show_title_bar: true\n"
                                          "```\n"
                                          "\n"

};

constexpr StringLiteral WindowShadowWeb {
    "configuration option sets how far the drop shadow around a frameless window reaches.\n"
    "One of `None`, `Small`, `Medium`, `Large` or `VeryLarge` — the same names and sizes Breeze "
    "offers, so a Contour window matches the rest of a Plasma desktop.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    window_shadow: Large\n"
    "```\n"
    "Only KWin can act on this. With "
    "[`show_title_bar`](profiles.md#show_title_bar) set to `true` the window manager draws its own "
    "shadow along with its decoration, and no other Wayland compositor or X11 window manager offers "
    "a way for an undecorated window to publish one — so everywhere else, every value behaves as "
    "`None`. Windows and macOS draw a system shadow of their own instead.\n"
    "\n"
};

constexpr StringLiteral DimUnfocusedWeb {
    "configuration option dims a pane while it is not the focused one (an inactive pane of a split, or "
    "any pane of an unfocused window) by blending it toward its own background color. The value is the "
    "blend amount between 0.0 (dimming disabled; the default) and 1.0 (fully blended to the "
    "background).\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    dim_unfocused: 0.3\n"
    "```\n"
    "\n"
};

constexpr StringLiteral ShowIndicatorOnResizeWeb {
    "configuration option determines whether or not the size indicator will be shown when terminal will "
    "resized.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    size_indicator_on_resize: true\n"
    "```\n"
    "\n"
};

constexpr StringLiteral SearchCaseSensitivityWeb {
    "Which case policy the find bar opens with.\n"
    "\n"
    "- `smart` (default): the search is case-insensitive until the term contains an uppercase "
    "letter, at which point it becomes exact. So `error` also finds `Error`, while `Error` finds only "
    "itself. The test is Unicode-aware, so it works for non-Latin scripts too.\n"
    "- `insensitive`: never case-sensitive.\n"
    "- `sensitive`: always case-sensitive.\n"
    "\n"
    "The find bar's `Aa` button cycles the same three at any time; this only selects the one it "
    "starts from.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    search_case_sensitivity: smart\n"
    "```\n"
    "\n"
};

constexpr StringLiteral InsertAfterYankWeb {
    "This configuration option determines whether the terminal should automatically switch from normal mode "
    "to insert mode after executing a yank command. When enabled, the terminal will enter insert mode, "
    "allowing for immediate text input. If disabled, the terminal will remain in normal mode, maintaining "
    "command functionality.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    insert_after_yank: false\n"
    "```\n"
    "\n"
};

constexpr StringLiteral CopyLastMarkRangeOffsetWeb {
    "configuration option is an advanced setting that is useful when using the CopyPreviousMarkRange feature "
    "with multiline prompts. It allows you to specify an offset value that is added to the current cursor's "
    "line number minus 1 (i.e., the line above the current cursor).\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    copy_last_mark_range_offset: 0\n"
    "```\n"
    "\n"
};

constexpr StringLiteral SmoothLineScrollingWeb {
    "Defines the number of milliseconds to wait before actually executing the LF (linefeed) control code\n"
    "in case DEC mode `DECSCLM` is enabled.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    slow_scrolling_time: 0\n"
    "```\n"
    "\n"

};

constexpr StringLiteral SmoothScrollingWeb {
    "Enables pixel-based smooth scrolling.\n"
    "When enabled, scrolling will be performed in pixel increments rather than jumping by full lines,\n"
    "providing a smoother visual experience similar to GNOME Terminal/VTE.\n"
    "This applies to mouse wheel and trackpad scrolling on the primary screen.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    smooth_scrolling: true\n"
    "```\n"
    "\n"
};

constexpr StringLiteral MomentumScrollingWeb {
    "Enables momentum (inertia) scrolling for touchpad gestures.\n"
    "When enabled, lifting your finger from the touchpad after a swipe will cause\n"
    "the terminal to continue scrolling with decelerating velocity, mimicking\n"
    "the natural scrolling behaviour of macOS and iOS.\n"
    "This governs touchpad finger-lift inertia only. Mouse-wheel smoothness is controlled by\n"
    "`smooth_scrolling`, which glides discrete wheel notches with the same physics.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    momentum_scrolling: true\n"
    "```\n"
    "\n"
};

using Shell = DocumentationEntry<ShellConfig, ShellWeb>;
using EscapeSandbox = DocumentationEntry<EscapeSandboxConfig, EscapeSandboxWeb>;
using SshHostConfig = DocumentationEntry<SshHostConfigConfig, SshHostConfigWeb>;
using Maximized = DocumentationEntry<MaximizedConfig, MaximizedWeb>;
using Fullscreen = DocumentationEntry<FullscreenConfig, FullscreenWeb>;
using ShowTitleBar = DocumentationEntry<ShowTitleBarConfig, ShowTitleBarWeb>;
using WindowShadow = DocumentationEntry<WindowShadowConfig, WindowShadowWeb>;
using DimUnfocused = DocumentationEntry<DimUnfocusedConfig, DimUnfocusedWeb>;
using ShowIndicatorOnResize = DocumentationEntry<ShowIndicatorOnResizeConfig, ShowIndicatorOnResizeWeb>;
using Mouse = DocumentationEntry<MouseConfig, MouseWeb>;
using SearchCaseSensitivity = DocumentationEntry<SearchCaseSensitivityConfig, SearchCaseSensitivityWeb>;
using InsertAfterYank = DocumentationEntry<InsertAfterYankConfig, InsertAfterYankWeb>;
using CopyLastMarkRangeOffset = DocumentationEntry<CopyLastMarkRangeOffsetConfig, CopyLastMarkRangeOffsetWeb>;
using WMClass = DocumentationEntry<WMClassConfig, WMClassWeb>;
using TabLabel = DocumentationEntry<TabLabelConfig, TabLabelWeb>;
using TabColor = DocumentationEntry<TabColorConfig, TabColorWeb>;
using PixelReporting = DocumentationEntry<PixelReportingConfig, PixelReportingWeb>;
using AccessibilityAnnouncements =
    DocumentationEntry<AccessibilityAnnouncementsConfig, AccessibilityAnnouncementsWeb>;
using AccessibilityCaretReporting =
    DocumentationEntry<AccessibilityCaretReportingConfig, AccessibilityCaretReportingWeb>;
using HyperlinkHoverTooltip = DocumentationEntry<HyperlinkHoverTooltipConfig, HyperlinkHoverTooltipWeb>;
using ProgressTimeout = DocumentationEntry<ProgressTimeoutConfig, ProgressTimeoutWeb>;
using NotificationCloseTimeout =
    DocumentationEntry<NotificationCloseTimeoutConfig, NotificationCloseTimeoutWeb>;
using TabBarPosition = DocumentationEntry<TabBarPositionConfig, TabBarPositionWeb>;
using TabBarVisibility = DocumentationEntry<TabBarVisibilityConfig, TabBarVisibilityWeb>;
using UiStyle = DocumentationEntry<UiStyleConfig, UiStyleWeb>;
using WindowControlStyle = DocumentationEntry<WindowControlStyleConfig, WindowControlStyleWeb>;
using UiFontFamily = DocumentationEntry<UiFontFamilyConfig, UiFontFamilyWeb>;
using UiFontSize = DocumentationEntry<UiFontSizeConfig, UiFontSizeWeb>;
using Margins = DocumentationEntry<MarginsConfig, MarginsWeb>;
using TerminalSize = DocumentationEntry<TerminalSizeConfig, TerminalSizeWeb>;
using TerminalId = DocumentationEntry<TerminalIdConfig, TerminalIdWeb>;
using History = DocumentationEntry<HistoryConfig, HistoryWeb>;
using Folding = DocumentationEntry<FoldingConfig, FoldingWeb>;
using Gutter = DocumentationEntry<GutterConfig, GutterWeb>;
using OscContext = DocumentationEntry<OscContextConfig, OscContextWeb>;
using CommandBlocks = DocumentationEntry<CommandBlocksConfig, CommandBlocksWeb>;
using Scrollbar = DocumentationEntry<ScrollbarConfig, ScrollbarWeb>;
using StickyHeader = DocumentationEntry<StickyHeaderConfig, StickyHeaderWeb>;
using NotifyOnCommandFinish = DocumentationEntry<NotifyOnCommandFinishConfig, NotifyOnCommandFinishWeb>;
using StatusLine = DocumentationEntry<StatusLineConfig, StatusLineWeb>;
using OptionKeyAsAlt = DocumentationEntry<OptionKeyAsAltConfig, OptionKeyAsAltWeb>;
using Fonts = DocumentationEntry<FontsConfig, FontsWeb>;
using Permissions = DocumentationEntry<PermissionsConfig, PermissionsWeb>;
using DrawBoldTextWithBrightColors =
    DocumentationEntry<DrawBoldTextWithBrightColorsConfig, DrawBoldTextWithBrightColorsWeb>;
constexpr StringLiteral BlinkStyleConfig {
    "{comment} Determines the visual style of cell blink animation (SGR 5/6).\n"
    "{comment}\n"
    "{comment} Valid values are:\n"
    "{comment}  - classic: Abrupt on/off toggle.\n"
    "{comment}  - smooth:  Continuous cosine-based pulse.\n"
    "{comment}  - linger:  Like smooth but stays visible longer.\n"
    "blink_style: {}\n"
    "\n"
};

constexpr StringLiteral BlinkStyleWeb {
    "Determines the visual style of cell blink animation (SGR attributes 5 and 6).\n"
    "\n"
    "Valid values are:\n"
    "- `classic` - abrupt on/off toggle\n"
    "- `smooth` - continuous cosine-based pulse\n"
    "- `linger` - like smooth but stays visible longer\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    blink_style: smooth\n"
    "```\n"
    "\n"
};

using BlinkStyle = DocumentationEntry<BlinkStyleConfig, BlinkStyleWeb>;

constexpr StringLiteral ScreenTransitionStyleConfig {
    "{comment} Determines the visual style of screen transitions between primary and alternate screens.\n"
    "{comment}\n"
    "{comment} Valid values are:\n"
    "{comment}  - classic: Instant switch (no animation).\n"
    "{comment}  - fade:    Smooth crossfade over a configurable duration.\n"
    "screen_transition: {}\n"
    "\n"
};

constexpr StringLiteral ScreenTransitionStyleWeb {
    "Determines the visual style of screen transitions between primary and alternate screens.\n"
    "\n"
    "When switching between primary and alternate screen buffers (e.g. entering/leaving vim, htop),\n"
    "this setting controls whether the transition is instant or animated.\n"
    "\n"
    "Valid values are:\n"
    "- `classic` - instant switch (no animation)\n"
    "- `fade` - smooth fade-out/fade-in (default 250ms)\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    screen_transition: fade\n"
    "```\n"
    "\n"
};

using ScreenTransitionStyle = DocumentationEntry<ScreenTransitionStyleConfig, ScreenTransitionStyleWeb>;

constexpr StringLiteral ScreenTransitionDurationConfig {
    "{comment} Duration in milliseconds for the screen transition animation.\n"
    "{comment} Only applies when screen_transition is set to \"fade\".\n"
    "screen_transition_duration: {}\n"
    "\n"
};

constexpr StringLiteral ScreenTransitionDurationWeb {
    "Duration in milliseconds for the screen transition animation.\n"
    "\n"
    "Only applies when `screen_transition` is set to `fade`.\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    screen_transition_duration: 250\n"
    "```\n"
    "\n"
};

using ScreenTransitionDuration =
    DocumentationEntry<ScreenTransitionDurationConfig, ScreenTransitionDurationWeb>;

constexpr StringLiteral CursorMotionAnimationDurationConfig {
    "{comment} Duration in milliseconds for cursor motion animation.\n"
    "{comment} Set to 0 to disable cursor motion animation.\n"
    "cursor_motion_animation_duration: {}\n"
    "\n"
};

constexpr StringLiteral CursorMotionAnimationDurationWeb {
    "Duration in milliseconds for the cursor motion animation when the cursor moves between cells.\n"
    "\n"
    "Set to `0` to disable cursor motion animation (instant jumps).\n"
    "\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    cursor_motion_animation_duration: 100\n"
    "```\n"
    "\n"
};

using CursorMotionAnimationDuration =
    DocumentationEntry<CursorMotionAnimationDurationConfig, CursorMotionAnimationDurationWeb>;

using Colors = DocumentationEntry<ColorsConfig, ColorsWeb>;
using ModalCursorScrollOff = DocumentationEntry<ModalCursorScrollOffConfig, ModalCursorScrollOffWeb>;
using ModeInsert = DocumentationEntry<ModeInsertConfig, ModeInsertWeb>;
using ModeNormal = DocumentationEntry<ModeNormalConfig, ModeNormalWeb>;
using ModeVisual = DocumentationEntry<ModeVisualConfig, ModeVisualWeb>;
using SmoothLineScrolling = DocumentationEntry<SmoothLineScrollingConfig, SmoothLineScrollingWeb>;
using SmoothScrolling = DocumentationEntry<SmoothScrollingConfig, SmoothScrollingWeb>;
using MomentumScrolling = DocumentationEntry<MomentumScrollingConfig, MomentumScrollingWeb>;
using HighlightTimeout = DocumentationEntry<HighlightTimeoutConfig, HighlightTimeoutWeb>;
using HighlightDoubleClickerWord =
    DocumentationEntry<HighlightDoubleClickerWordConfig, HighlightDoubleClickerWordWeb>;
using Background = DocumentationEntry<BackgroundConfig, BackgroundWeb>;
using Bell = DocumentationEntry<BellConfig, BellWeb>;
using FrozenDecMode = DocumentationEntry<FrozenDecModeConfig, Dummy>;
using Live = DocumentationEntry<LiveConfig, LiveWeb>;
using PlatformPlugin = DocumentationEntry<PlatformPluginConfig, PlatformPluginWeb>;
using Renderer = DocumentationEntry<RendererConfig, RendererWeb>;
using CommandPaletteRecentCount =
    DocumentationEntry<CommandPaletteRecentCountConfig, CommandPaletteRecentCountWeb>;
using PTYReadBufferSize = DocumentationEntry<PTYReadBufferSizeConfig, PTYReadBufferSizeWeb>;
using PTYBufferObjectSize = DocumentationEntry<PTYBufferObjectSizeConfig, PTYBufferObjectSizeWeb>;
using ReflowOnResize = DocumentationEntry<ReflowOnResizeConfig, ReflowOnResizeWeb>;
using SnapToRows = DocumentationEntry<SnapToRowsConfig, SnapToRowsWeb>;
using RememberWindowPosition = DocumentationEntry<RememberWindowPositionConfig, RememberWindowPositionWeb>;
using TabSwitchOnHorizontalWheel =
    DocumentationEntry<TabSwitchOnHorizontalWheelConfig, TabSwitchOnHorizontalWheelWeb>;
using GuiConfigLocked = DocumentationEntry<GuiConfigLockedConfig, GuiConfigLockedWeb>;
using Theme = DocumentationEntry<ThemeConfig, ThemeWeb>;
using TextScalingMethod = DocumentationEntry<TextScalingMethodConfig, TextScalingMethodWeb>;
using GraphemeClustering = DocumentationEntry<GraphemeClusteringConfig, GraphemeClusteringWeb>;
using ColorSchemes = DocumentationEntry<ColorSchemesConfig, Dummy>;
using Profiles = DocumentationEntry<ProfilesConfig, ProfilesWeb>;
using DefaultProfiles = DocumentationEntry<StringLiteral { "default_profile: {}\n" }, DefaultProfilesWeb>;
using Layouts = DocumentationEntry<LayoutsConfig, LayoutsWeb>;
using Hosts = DocumentationEntry<HostsConfig, HostsWeb>;
using DefaultLayout = DocumentationEntry<StringLiteral { "default_layout: {}\n" }, DefaultLayoutWeb>;
using WordDelimiters = DocumentationEntry<WordDelimitersConfig, WordDelimitersWeb>;
using ExtendedWordDelimiters = DocumentationEntry<ExtendedWordDelimitersConfig, ExtendedWordDelimitersWeb>;
using BypassMouseProtocolModifiers =
    DocumentationEntry<BypassMouseProtocolModifiersConfig, BypassMouseProtocolModifiersWeb>;
using OnMouseSelection = DocumentationEntry<OnMouseSelectionConfig, OnMouseSelectionWeb>;
using MouseBlockSelectionModifiers =
    DocumentationEntry<MouseBlockSelectionModifiersConfig, MouseBlockSelectionModifiersWeb>;
using InputMappings = DocumentationEntry<InputMappingsConfig, Dummy>;
using SpawnNewProcess = DocumentationEntry<SpawnNewProcessConfig, SpawnNewProcessWeb>;
using EarlyExitThreshold = DocumentationEntry<EarlyExitThresholdConfig, EarlyExitThresholdWeb>;
using Images = DocumentationEntry<ImagesConfig, ImagesWeb>;
using Daemon = DocumentationEntry<DaemonConfig, DaemonWeb>;
using ExperimentalFeatures = DocumentationEntry<ExperimentalFeaturesConfig, StringLiteral { "" }>;
using DefaultColors = DocumentationEntry<DefaultColorsConfig, Dummy>;
using HyperlinkDecoration = DocumentationEntry<HyperlinkDecorationConfig, HyperlinkDecorationWeb>;
using LinkDetection = DocumentationEntry<LinkDetectionConfig, LinkDetectionWeb>;
using FileOpener = DocumentationEntry<FileOpenerConfig, FileOpenerWeb>;
using HyperlinkDecorationColors = DocumentationEntry<HyperlinkDecorationColorsConfig, Dummy>;
using YankHighlight = DocumentationEntry<YankHighlightConfig, Dummy>;
using NormalModeCursorline = DocumentationEntry<NormalModeCursorlineConfig, Dummy>;
using Selection = DocumentationEntry<SelectionConfig, Dummy>;
using SearchHighlight = DocumentationEntry<SearchHighlightConfig, Dummy>;
using SearchHighlightFocused = DocumentationEntry<SearchHighlightFocusedConfig, Dummy>;
using WordHighlightCurrent = DocumentationEntry<WordHighlightCurrentConfig, Dummy>;
using WordHighlight = DocumentationEntry<WordHighlightConfig, Dummy>;
using HintLabel = DocumentationEntry<HintLabelConfig, Dummy>;
using HintMatch = DocumentationEntry<HintMatchConfig, Dummy>;
using HintPatterns = DocumentationEntry<HintPatternsConfig, Dummy>;
using Extensions = DocumentationEntry<ExtensionsConfig, ExtensionsWeb>;
using HintScrollbackLines = DocumentationEntry<HintScrollbackLinesConfig, Dummy>;
using IndicatorStatusLine = DocumentationEntry<IndicatorStatusLineConfig, Dummy>;
using FoldMarker = DocumentationEntry<FoldMarkerConfig, Dummy>;
using FoldMarkerHeader = DocumentationEntry<FoldMarkerHeaderConfig, Dummy>;
using FoldMarkerDefault = DocumentationEntry<FoldMarkerDefaultConfig, Dummy>;
using FoldMarkerHover = DocumentationEntry<FoldMarkerHoverConfig, Dummy>;
using ContextTint = DocumentationEntry<ContextTintConfig, Dummy>;
using ContextTintHeader = DocumentationEntry<ContextTintHeaderConfig, Dummy>;
using BlockStatus = DocumentationEntry<BlockStatusConfig, Dummy>;
using BlockStatusHeader = DocumentationEntry<BlockStatusHeaderConfig, Dummy>;
using BlockStatusHover = DocumentationEntry<BlockStatusHoverConfig, Dummy>;
using BlockStatusHoverHeader = DocumentationEntry<BlockStatusHoverHeaderConfig, Dummy>;
using GutterText = DocumentationEntry<GutterTextConfig, Dummy>;
using GutterTextSet = DocumentationEntry<GutterTextSetConfig, Dummy>;
using IndentedEntry = DocumentationEntry<IndentedEntryConfig, Dummy>;
using StickyHeaderColors = DocumentationEntry<StickyHeaderColorsConfig, Dummy>;
using StickyHeaderColorsHeader = DocumentationEntry<StickyHeaderColorsHeaderConfig, Dummy>;
using InputMethodEditor = DocumentationEntry<InputMethodEditorConfig, Dummy>;
using InputMethodEditorSupport = DocumentationEntry<InputMethodEditorSupportConfig, Dummy>;
using NormalColors = DocumentationEntry<NormalColorsConfig, Dummy>;
using BrightColors = DocumentationEntry<BrightColorsConfig, Dummy>;
using DimColors = DocumentationEntry<DimColorsConfig, Dummy>;
using GitDrawings = DocumentationEntry<GitDrawingsConfig, Dummy>;
using BoxArcStyle = DocumentationEntry<BoxArcStyleConfig, Dummy>;
using BrailleStyle = DocumentationEntry<BrailleStyleConfig, Dummy>;

} // namespace contour::config::documentation
