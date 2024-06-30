#pragma once

#include <algorithm>
#include <string_view>
namespace contour::config::documentation
{

template <std::size_t N>
struct StringLiteral
{
    constexpr StringLiteral(const char (&str)[N]): value {} { std::copy_n(str, N, value); }

    char value[N]; // NOLINT
};

constexpr StringLiteral Dummy { "{comment} fmt formatted doc {} \n" };

constexpr StringLiteral Shell {
    "{comment} You can override the process to be started inside the terminal."
    "{comment} If nothing is specified, the users' default login shell will be used.\n"
    "{comment} But you may as well log in to a remote host.\n"
    "shell: {}\n"
    "arguments: {}\n"
    "\n"
};

constexpr StringLiteral InitialWorkingDirectory {
    "{comment} Sets initial working directory when spawning a new terminal.\n"
    "{comment} A leading ~ is expanded to the user's home directory.\n"
    "{comment} Default value is the user's home directory.\n"
    "initial_working_directory: {}\n"
    "\n"
};

constexpr StringLiteral EscapeSandbox {
    "{comment} If this terminal is being executed from within Flatpak, enforces sandboxing\n"
    "{comment} then this boolean indicates whether or not that sandbox should be escaped or not.\n"
    "{comment}\n"
    "escape_sandbox: {}\n"
    "\n"
};

constexpr StringLiteral SshHostConfig {
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

constexpr StringLiteral Maximized { "{comment} When this profile is *activated*, this flag decides\n"
                                    "{comment} whether or not to put the window into maximized mode.\n"
                                    "maximized: {}"
                                    "\n" };

constexpr StringLiteral Fullscreen {
    "{comment} When this profile is being *activated*, this flag decides\n"
    "{comment} whether or not to put the terminal's screen into fullscreen mode.\n"
    "{comment} It is activated during startup as well as when switching from another profile to "
    "this one.\n"
    "fullscreen: {}\n"
    "\n"
};

constexpr StringLiteral ShowTitleBar { "{comment} When this profile is *activated*, this flag decides\n"
                                       "{comment} whether or not the title bar will be shown\n"
                                       "show_title_bar: {}\n"
                                       "\n" };

constexpr StringLiteral ShowIndicatorOnResize {
    "{comment} When this profile is *activated*, this flag decides\n"
    "{comment} whether or not the size indicator on resize will be shown.\n"
    "size_indicator_on_resize: {}\n"
    "\n"
};

constexpr StringLiteral MouseHideWhileTyping { "{comment} whether or not to hide mouse when typing\n"
                                               "hide_while_typing: {}\n"
                                               "\n" };

constexpr StringLiteral SeachModeSwitch {
    "{comment} Whether or not to switch from search mode into insert on exit. If this value is set to true,\n"
    "{comment} it will go back to insert mode, otherwise it will go back to normal mode.\n"
    "search_mode_switch: {}\n"
    "\n"
};

constexpr StringLiteral CopyLastMarkRangeOffset {
    "{comment} Advanced value that is useful when CopyPreviousMarkRange is used \n"
    "{comment} with multiline-prompts. This offset value is being added to the \n"
    "{comment} current cursor's line number minus 1 (i.e. the line above the current cursor). \n"
    "copy_last_mark_range_offset: {}\n"
    "\n"
};

constexpr StringLiteral WMClass {
    "{comment} Defines the class part of the WM_CLASS property of the window.\n"
};

constexpr StringLiteral Margins {
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

constexpr StringLiteral TerminalSize { "{comment}Determines the initial terminal size in  characters\n"
                                       "terminal_size:\n"
                                       "    columns: {} \n"
                                       "    lines: {} \n"
                                       "\n" };

constexpr StringLiteral TerminalId { "{comment} Determines the terminal type that is being advertised.\n"
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
                                     "\n" };

constexpr StringLiteral MaxHistoryLineCount { "{comment} Number of lines to preserve (-1 for infinite).\n"
                                              "limit: {}\n"
                                              "\n" };

constexpr StringLiteral HistoryScrollMultiplier {
    "{comment} Number of lines to scroll on ScrollUp & ScrollDown events.\n"
    "scroll_multiplier: {}\n"
    "\n"
};

constexpr StringLiteral ScrollbarPosition {
    "{comment} scroll bar position: Left, Right, Hidden (ignore-case)\n"
    "position: {}\n"
    "\n"
};

constexpr StringLiteral StatusDisplayPosition {
    "{comment} Position to place the status line to, if it is to be shown.\n"
    "{comment} This can be either value `top` or value `bottom`.\n"
    "position: {}\n"
    "\n"
};

constexpr StringLiteral IndicatorStatusLineLeft { "left: \"{}\"\n" };
constexpr StringLiteral IndicatorStatusLineMiddle { "middle: \"{}\"\n" };
constexpr StringLiteral IndicatorStatusLineRight { "right: \"{}\"\n" };

constexpr StringLiteral SyncWindowTitleWithHostWritableStatusDisplay {
    "{comment} Synchronize the window title with the Host Writable status_line if\n"
    "{comment} and only if the host writable status line was denied to be shown.\n"
    "sync_to_window_title: {}\n"
    "\n"
};

constexpr StringLiteral HideScrollbarInAltScreen {
    "{comment} whether or not to hide the scrollbar when in alt-screen.\n"
    "hide_in_alt_screen: {}\n"
    "\n"
};

constexpr StringLiteral AutoScrollOnUpdate {
    "{comment} Boolean indicating whether or not to scroll down to the bottom on screen updates.\n"
    "auto_scroll_on_update: {}\n"
    "\n"
};

constexpr StringLiteral Fonts {
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
    "    {comment} Font render modes tell the font rasterizer engine what rendering technique to use.\n"
    "    {comment}\n"
    "    {comment} Modes available are:\n"
    "    {comment} - lcd          Uses a subpixel rendering technique optimized for LCD displays.\n"
    "    {comment} - light        Uses a subpixel rendering technique in gray-scale.\n"
    "    {comment} - gray         Uses standard gray-scaled anti-aliasing.\n"
    "    {comment} - monochrome   Uses pixel-perfect bitmap rendering.\n"
    "    render_mode: {}\n"
    "\n"
    "    {comment} Indicates whether or not to include *only* monospace fonts in the font and\n"
    "    {comment} font-fallback list (Default: true).\n"
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
};

constexpr StringLiteral CaptureBuffer {
    "{comment} Allows capturing the screen buffer via `CSI > Pm ; Ps ; Pc ST`.\n"
    "{comment} The response can be read from stdin as sequence `OSC 314 ; <screen capture> ST`\n"
    "capture_buffer: {}\n"
    "\n"
};

constexpr StringLiteral ChangeFont { "{comment} Allows changing the font via `OSC 50 ; Pt ST`.\n"
                                     "change_font: {}\n"
                                     "\n" };

constexpr StringLiteral DisplayHostWritableStatusLine {
    "{comment} Allows displaying the \" Host Writable Statusline \" programmatically using `DECSSDT 2`.\n"
    "display_host_writable_statusline: {}\n"
    "\n"
};

constexpr StringLiteral DrawBoldTextWithBrightColors {
    "{comment} Indicates whether or not bold text should be rendered in bright colors,\n"
    "{comment} for indexed colors.\n"
    "{comment} If disabled, normal color will be used instead.\n"
    "draw_bold_text_with_bright_colors: {}\n"
    "\n"
};

constexpr StringLiteral Colors {
    "{comment} Specifies a colorscheme to use (alternatively the colors can be inlined).\n"
    "{comment}\n"
    "{comment} This can be either the name to a single colorscheme to always use,\n"
    "{comment} or a map with two keys (dark and light) to determine the color scheme to use for each.\n"
    "{comment}\n"
    "{comment} The dark color scheme is used when the system is configured to prefer dark mode and light "
    "theme otherwise.\n"
    "\n"
    "colors: {}\n"
};

constexpr StringLiteral ModalCursorScrollOff {
    "{comment} Configures a `scrolloff` for cursor movements in normal and visual (block) modes.\n"
    "{comment}\n"
    "vi_mode_scrolloff: {}\n"
    "\n"
};

constexpr StringLiteral ModeInsert {
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

constexpr StringLiteral ModeNormal { "{comment} vi-like normal-mode specific settings.\n"
                                     "{comment} Note, currently only the cursor can be customized.\n"
                                     "normal_mode:\n"
                                     "    cursor:\n"
                                     "        shape: {}\n"
                                     "        blinking: {}\n"
                                     "        blinking_interval: {}\n"
                                     "\n" };

constexpr StringLiteral ModeVisual { "{comment} vi-like normal-mode specific settings.\n"
                                     "{comment} Note, currently only the cursor can be customized.\n"
                                     "visual_mode:\n"
                                     "    cursor:\n"
                                     "        shape: {}\n"
                                     "        blinking: {}\n"
                                     "        blinking_interval: {}\n"
                                     "\n" };

constexpr StringLiteral SmoothLineScrolling { "{comment} Defines the number of milliseconds to wait before\n"
                                              "{comment} actually executing the LF (linefeed) control code\n"
                                              "{comment} in case DEC mode `DECSCLM` is enabled.\n"
                                              "slow_scrolling_time: {}\n"
                                              "\n" };

constexpr StringLiteral HighlightTimeout {
    "{comment} Time duration in milliseconds for which yank highlight is shown.\n"
    "vi_mode_highlight_timeout: {}\n"
    "\n"
};

constexpr StringLiteral HighlightDoubleClickerWord {
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

constexpr StringLiteral InitialStatusLine {
    "{comment} Either none or indicator.\n"
    "{comment} This only reflects the initial state of the status line, as it can\n"
    "{comment} be changed at any time during runtime by the user or by an application.\n"
    "display: {}\n"
    "\n"
};

constexpr StringLiteral BackgroundOpacity {
    "{comment} Background opacity to use. A value of 1.0 means fully opaque whereas 0.0 means fully\n"
    "{comment} transparent. Only values between 0.0 and 1.0 are allowed.\n"
    "opacity: {}\n"
    "\n"
};

constexpr StringLiteral BackgroundBlur {
    "{comment} Some platforms can blur the transparent background (currently only Windows 10 is "
    "supported).\n"
    "blur: {}\n"
    "\n"
};

constexpr StringLiteral Bell {
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
    "    {comment} Default: 1.0\n"
    "    volume: {}\n"
    "\n"
    "    {comment} If this boolean is true, a window alert will "
    "be raised with each bell\n"
    "    alert: true\n"
    "\n"
};

constexpr StringLiteral FrozenDecMode {
    "{comment} Defines a list of DEC modes to explicitly and permanently disable/enable support for.\n"
    "{comment}\n"
    "{comment} This is a developer-users-only option that may possibly help investigating problems.\n"
    "{comment} This option may also be used by regular users if they're asked to in order to disable\n"
    "{comment} a broken functionality. This is something we hardly try to avoid, of course.\n"
    "{comment}\n"
    "{comment} This can be an object with each key being the DEC mode number and its value a boolean,\n"
    "{comment} indicating whether this DEC mode is permanently set (enabled) or unset (disabled).\n"
    "{comment}\n"
    "{comment} Example:\n"
    "{comment}\n"
    "{comment}     frozen_dec_modes:\n"
    "{comment}         2026: false\n"
    "{comment}         2027: true\n"
    "{comment}\n"
    "{comment} Default: (empty object)\n"
    "{comment}frozen_dec_modes:\n"
};

constexpr StringLiteral Live {
    "{comment} Determines whether the instance is reloading the configuration files "
    "whenever it is changing or not. \n"
    "live_config: {} \n"
    "\n"
};

constexpr StringLiteral PlatformPlugin {
    "{comment} Overrides the auto-detected platform plugin to be loaded. \n"
    "{comment} \n"
    "{comment} Possible (incomplete list of) values are:\n"
    "{comment} - auto        The platform will be auto-detected.\n"
    "{comment} - xcb         Uses XCB plugin (for X11 environment).\n"
    "{comment} - cocoa       Used to be run on Mac OS/X.\n"
    "{comment} - direct2d    Windows platform plugin using Direct2D.\n"
    "{comment} - winrt       Windows platform plugin using WinRT.\n"
    "platform_plugin: {} \n"
    "\n"
};

constexpr StringLiteral RenderingBackend {
    "{comment} Backend to use for rendering the terminal onto the screen \n"
    "{comment} Possible values are: \n"
    "{comment} - default     Uses the default rendering option as decided by the terminal. \n"
    "{comment} - software    Uses software-based rendering. \n"
    "{comment} - OpenGL      Use (possibly) hardware accelerated OpenGL \n"
    "backend: {} \n"
    "\n"
};

constexpr StringLiteral TextureAtlasDirectMapping {
    "{comment} Enables/disables the use of direct-mapped texture atlas tiles for \n"
    "{comment} the most often used ones (US-ASCII, cursor shapes, underline styles) \n"
    "{comment} You most likely do not want to touch this. \n"
    "{comment} \n"
    "tile_direct_mapping: {} \n"
    "\n"
};

constexpr StringLiteral TextureAtlasHashtableSlots {
    "{comment} Number of hashtable slots to map to the texture tiles. \n"
    "{comment} Larger values may increase performance, but too large may also decrease. \n"
    "{comment} This value is rounded up to a value equal to the power of two. \n"
    "{comment} \n"
    "tile_hashtable_slots: {} \n"
    "\n"
};

constexpr StringLiteral TextureAtlasTileCount {
    "{comment} Number of tiles that must fit at lest into the texture atlas. \n"
    "{comment} \n"
    "{comment} This does not include direct mapped tiles (US-ASCII glyphs, \n"
    "{comment} cursor shapes and decorations), if tile_direct_mapping is set to true). \n"
    "{comment} \n"
    "{comment} Value must be at least as large as grid cells available in the terminal view. \n"
    "{comment} This value is automatically adjusted if too small. \n"
    "{comment} \n"
    "tile_cache_count: {} \n"
    "\n"
};

constexpr StringLiteral PTYReadBufferSize { "{comment} Default PTY read buffer size. \n"
                                            "{comment} \n"
                                            "{comment} This is an advance option. Use with care! \n"
                                            "read_buffer_size: {} \n"
                                            "\n" };

constexpr StringLiteral PTYBufferObjectSize {
    "{comment} Size in bytes per PTY Buffer Object. \n "
    "{comment} \n"
    "{comment} This is an advanced option of an internal storage. Only change with care! \n"
    "pty_buffer_size: {} \n"
    "\n"
};

constexpr StringLiteral ReflowOnResize {
    "\n"
    "{comment} Whether or not to reflow the lines on terminal resize events. \n"
    "reflow_on_resize: {} \n"
};

constexpr StringLiteral ColorSchemes {
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

constexpr StringLiteral Profiles {
    "\n"
    "{comment} Terminal Profiles\n"
    "{comment} -----------------\n"
    "{comment}\n"
    "{comment} Dominates how your terminal visually looks like. You will need at least one terminal "
    "profile.\n"
    "profiles:\n"
    "\n"
};

constexpr StringLiteral WordDelimiters { "{comment} Word delimiters when selecting word-wise. \n"
                                         "word_delimiters: \"{}\" \n"
                                         "\n" };

constexpr StringLiteral ExtendedWordDelimiters {
    "{comment} Word delimiters for second selection when selecting word-wise. \n"
    "{comment} Setting allows you to set less strict boundaried between words, for example \n"
    "{comment} if you want to select whole ip address during selection set delimieters to \" \" (space) \n"
    "extended_word_delimiters: \"{}\" \n"
    "\n"
};

constexpr StringLiteral BypassMouseProtocolModifiers {
    "{comment} This keyboard modifier can be used to bypass the terminal's mouse protocol, \n"
    "{comment} which can be used to select screen content even if the an application \n"
    "{comment} mouse protocol has been activated (Default: Shift). \n"
    "{comment} \n"
    "{comment} The same modifier values apply as with input modifiers (see below). \n"
    "bypass_mouse_protocol_modifier: {} \n"
    "\n"
};

constexpr StringLiteral OnMouseSelection {
    "{comment} Selects an action to perform when a text selection has been made. \n"
    "{comment} \n"
    "{comment} Possible values are: \n"
    "{comment} \n"
    "{comment} - None                        Does nothing \n"
    "{comment} - CopyToClipboard             Copies the selection to the primary clipboard. \n"
    "{comment} - CopyToSelectionClipboard    Copies the selection to the selection clipboard. \n"
    "{comment}This is not supported on all platforms. \n"
    "{comment} \n"
    "on_mouse_select: {} \n"
    "\n"
};

constexpr StringLiteral MouseBlockSelectionModifiers {
    "{comment} Modifier to be pressed in order to initiate block-selection \n"
    "{comment} using the left mouse button. \n"
    "{comment} \n"
    "{comment} This is usually the Control modifier, but on OS/X that is not possible, \n"
    "{comment} so Alt or Meta would be recommended instead. \n"
    "{comment} \n"
    "{comment} Supported modifiers: \n"
    "{comment} - Alt \n"
    "{comment} - Control \n"
    "{comment} - Shift \n"
    "{comment} - Meta \n"
    "{comment} \n"
    "mouse_block_selection_modifier: {} \n"
    "\n"
};

constexpr StringLiteral InputMappings {
    "{comment} Key Bindings\n"
    "{comment} ------------\n"
    "{comment}\n"
    "{comment} In this section you can customize key bindings.\n"
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
    "{comment}   Left, Middle, Right, WheelUp, WheelDown\n"
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
    "{comment} - IncreaseFontSize  Increases the font size by 1 pixel.\n"
    "{comment} - IncreaseOpacity   Increases the default-background opacity by 5%.\n"
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
    "{comment} - SearchReverse     Initiates search mode (starting to search at current cursor position, "
    "moving "
    "upwards).\n"
    "{comment} - SendChars         Writes given characters in `chars` member to the applications input.\n"
    "{comment} - ToggleAllKeyMaps  Disables/enables responding to all keybinds (this keybind will be "
    "preserved "
    "when disabling all others).\n"
    "{comment} - ToggleFullScreen  Enables/disables full screen mode.\n"
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
    "input_mapping:\n"
};

constexpr StringLiteral SpawnNewProcess {
    "\n"
    "{comment} Flag to determine whether to spawn new process or not when creating new terminal \n"
    "spawn_new_process: {} \n"
};

constexpr unsigned DefaultEarlyExitThreshold = 5u;
constexpr StringLiteral EarlyExitThreshold { "\n"
                                             "{comment} Time in seconds to check for early threshold \n"
                                             "early_exit_threshold: {} \n" };

constexpr StringLiteral SixelScrolling { "{comment} Enable or disable sixel scrolling (SM/RM ?80 default) \n"
                                         "sixel_scrolling: {} \n" };

constexpr StringLiteral MaxImageSize {
    "\n"
    "{comment} maximum width in pixels of an image to be accepted (0 defaults to system screen pixel "
    "width) "
    "\n"
    "max_width: {} \n"
    "{comment} maximum height in pixels of an image to be accepted (0 defaults to system screen pixel "
    "height) \n"
    "max_height: {} \n"
};

constexpr StringLiteral MaxImageColorRegisters {
    "\n"
    "{comment} Configures the maximum number of color registers available when rendering Sixel "
    "graphics. \n"
    "sixel_register_count: {} \n"
};

constexpr StringLiteral ExperimentalFeatures {
    "\n"
    "{comment} Section of experimental features.\n"
    "{comment} All experimental features are disabled by default and must be explicitly enabled here.\n"
    "{comment} NOTE: Contour currently has no experimental features behind this configuration wall.\n"
    "{comment} experimental:\n"
    "{comment}     {comment} Enables experimental support for feature X/Y/Z\n"
    "{comment}     feature_xyz: true\n"
};

constexpr StringLiteral DefaultColors { "{comment} Default colors\n"
                                        "default:\n"
                                        "    {comment} Default background color (this can be made "
                                        "transparent, see above).\n"
                                        "    background: {}\n"
                                        "    {comment} Default foreground text color.\n"
                                        "    foreground: {}\n" };

constexpr StringLiteral HyperlinkDecoration {
    "\n"
    "{comment} color to pick for hyperlinks decoration, when hovering\n"
    "hyperlink_decoration:\n"
    "    normal: {}\n"
    "    hover: {}\n"
};

constexpr StringLiteral YankHighlight {
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

constexpr StringLiteral NormalModeCursorline {
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

constexpr StringLiteral Selection {
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

constexpr StringLiteral SearchHighlight {
    "\n"
    "{comment} Search match highlighting. Similar to selection highlighting.\n"
    "search_highlight:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral SearchHighlihtFocused {
    "\n"
    "{comment} Search match highlighting (focused term). Similar to selection "
    "highlighting.\n"
    "search_highlight_focused:\n"
    "    foreground: {}\n"
    "    foreground_alpha: {}\n"
    "    background: {}\n"
    "    background_alpha: {}\n"
};

constexpr StringLiteral WordHighlightCurrent {
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

constexpr StringLiteral WordHighlight {
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

constexpr StringLiteral IndicatorStatusLine {
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

constexpr StringLiteral InputMethodEditor { "\n"
                                            "{comment} Colors for the IME (Input Method Editor) area.\n"
                                            "input_method_editor:\n"
                                            "    foreground: {}\n"
                                            "    background: {}\n" };

constexpr StringLiteral NormalColors { "\n"
                                       "{comment} Normal colors\n"
                                       "normal:\n"
                                       "    black:   {}\n"
                                       "    red:     {}\n"
                                       "    green:   {}\n"
                                       "    yellow:  {}\n"
                                       "    blue:    {}\n"
                                       "    magenta: {}\n"
                                       "    cyan:    {}\n"
                                       "    white:   {}\n" };

constexpr StringLiteral BrightColors { "\n"
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

constexpr StringLiteral DimColors {
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

} // namespace contour::config::documentation
