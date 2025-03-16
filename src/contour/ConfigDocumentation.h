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

template <auto T>
struct Wrap
{
};

template <StringLiteral Config, StringLiteral Web>
struct DocumentationEntry
{
};

constexpr StringLiteral Dummy { "{comment} fmt formatted doc {} \n" };

constexpr StringLiteral ShellConfig {
    "{comment} You can override the process to be started inside the terminal."
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

constexpr StringLiteral ShowTitleBarConfig { "{comment} When this profile is *activated*, this flag decides\n"
                                             "{comment} whether or not the title bar will be shown\n"
                                             "show_title_bar: {}\n"
                                             "\n" };

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

constexpr StringLiteral SeachModeSwitchConfig {
    "{comment} Whether or not to switch from search mode into insert on exit. If this value is set to true,\n"
    "{comment} it will go back to insert mode, otherwise it will go back to normal mode.\n"
    "search_mode_switch: {}\n"
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
    "{comment} Advanced value that is useful when CopyPreviousMarkRange is used \n"
    "{comment} with multiline-prompts. This offset value is being added to the \n"
    "{comment} current cursor's line number minus 1 (i.e. the line above the current cursor). \n"
    "copy_last_mark_range_offset: {}\n"
    "\n"
};

constexpr StringLiteral WMClassConfig {
    "{comment} Defines the class part of the WM_CLASS property of the window.\n"
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
                                             "    columns: {} \n"
                                             "    lines: {} \n"
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
    "    {comment} Boolean indicating whether or not to scroll down to the bottom on screen updates.\n"
    "    auto_scroll_on_update: {}\n"
    "    {comment} Number of lines to scroll on ScrollUp & ScrollDown events.\n"
    "    scroll_multiplier: {}\n"
    "\n"

};

constexpr StringLiteral ScrollbarConfig {

    "scrollbar:\n"
    "    {comment} scroll bar position: Left, Right, Hidden (ignore-case)\n"
    "    position: {}\n"
    "    {comment} whether or not to hide the scrollbar when in alt-screen.\n"
    "    hide_in_alt_screen: {}\n"
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
};

constexpr StringLiteral PermissionsConfig {

    "\n"
    "{comment} Some VT sequences should need access permissions.\n"
    "{comment} \n"
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

constexpr StringLiteral HighlightTimeoutConfig {
    "{comment} Time duration in milliseconds for which yank highlight is shown.\n"
    "vi_mode_highlight_timeout: {}\n"
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
    "    indicator:\n"
    "        left: \"{}\"\n"
    "        middle: \"{}\"\n"
    "        right: \"{}\"\n"
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
    "    {comment} Default: 1.0\n"
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
    "{comment}\n"
    "{comment} Example:\n"
    "{comment}\n"
    "{comment}     frozen_dec_modes:\n"
    "{comment}         2026: false\n"
    "{comment}         2027: true\n"
    "{comment}\n"
    "{comment} Default: (empty object)\n"
    "{comment}frozen_dec_modes:\n"
    "\n"
};

constexpr StringLiteral LiveConfig {
    "{comment} Determines whether the instance is reloading the configuration files "
    "whenever it is changing or not. \n"
    "live_config: {} \n"
    "\n"
};

constexpr StringLiteral PlatformPluginConfig {
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

constexpr StringLiteral RendererConfig {
    "renderer:\n"
    "    {comment} Backend to use for rendering the terminal onto the screen \n"
    "    {comment} Possible values are: \n"
    "    {comment} - default     Uses the default rendering option as decided by the terminal. \n"
    "    {comment} - software    Uses software-based rendering. \n"
    "    {comment} - OpenGL      Use (possibly) hardware accelerated OpenGL \n"
    "    backend: {} \n"
    "\n"
    "    {comment} Enables/disables the use of direct-mapped texture atlas tiles for \n"
    "    {comment} the most often used ones (US-ASCII, cursor shapes, underline styles) \n"
    "    {comment} You most likely do not want to touch this. \n"
    "    {comment} \n"
    "    tile_direct_mapping: {} \n"
    "\n"
    "    {comment} Number of hashtable slots to map to the texture tiles. \n"
    "    {comment} Larger values may increase performance, but too large may also decrease. \n"
    "    {comment} This value is rounded up to a value equal to the power of two. \n"
    "    {comment} \n"
    "    tile_hashtable_slots: {} \n"
    "\n"
    "    {comment} Number of tiles that must fit at lest into the texture atlas. \n"
    "    {comment} \n"
    "    {comment} This does not include direct mapped tiles (US-ASCII glyphs, \n"
    "    {comment} cursor shapes and decorations), if tile_direct_mapping is set to true). \n"
    "    {comment} \n"
    "    {comment} Value must be at least as large as grid cells available in the terminal view. \n"
    "    {comment} This value is automatically adjusted if too small. \n"
    "    {comment} \n"
    "    tile_cache_count: {} \n"
    "\n"
};

constexpr StringLiteral PTYReadBufferSizeConfig { "{comment} Default PTY read buffer size. \n"
                                                  "{comment} \n"
                                                  "{comment} This is an advance option. Use with care! \n"
                                                  "read_buffer_size: {} \n"
                                                  "\n" };

constexpr StringLiteral PTYBufferObjectSizeConfig {
    "{comment} Size in bytes per PTY Buffer Object. \n "
    "{comment} \n"
    "{comment} This is an advanced option of an internal storage. Only change with care! \n"
    "pty_buffer_size: {} \n"
    "\n"
};

constexpr StringLiteral ReflowOnResizeConfig {
    "\n"
    "{comment} Whether or not to reflow the lines on terminal resize events. \n"
    "reflow_on_resize: {} \n"
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

constexpr StringLiteral WordDelimitersConfig { "{comment} Word delimiters when selecting word-wise. \n"
                                               "word_delimiters: \"{}\" \n"
                                               "\n" };

constexpr StringLiteral ExtendedWordDelimitersConfig {
    "{comment} Word delimiters for second selection when selecting word-wise. \n"
    "{comment} Setting allows you to set less strict boundaried between words, for example \n"
    "{comment} if you want to select whole ip address during selection set delimieters to \" \" (space) \n"
    "extended_word_delimiters: \"{}\" \n"
    "\n"
};

constexpr StringLiteral BypassMouseProtocolModifiersConfig {
    "{comment} This keyboard modifier can be used to bypass the terminal's mouse protocol, \n"
    "{comment} which can be used to select screen content even if the an application \n"
    "{comment} mouse protocol has been activated (Default: Shift). \n"
    "{comment} \n"
    "{comment} The same modifier values apply as with input modifiers (see below). \n"
    "bypass_mouse_protocol_modifier: {} \n"
    "\n"
};

constexpr StringLiteral OnMouseSelectionConfig {
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

constexpr StringLiteral MouseBlockSelectionModifiersConfig {
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

constexpr StringLiteral InputMappingsConfig {
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
    "{comment} - SearchReverse     Initiates search mode (starting to search at current cursor position, "
    "moving "
    "upwards).\n"
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
    "{comment} - SetTabName        Ask the user to assign a name to the active tab.\n"
    "\n"
    "input_mapping:\n"
};

constexpr StringLiteral SpawnNewProcessConfig {
    "\n"
    "{comment} Flag to determine whether to spawn new process or not when creating new terminal \n"
    "spawn_new_process: {} \n"
};

constexpr unsigned DefaultEarlyExitThreshold = 5u;
constexpr StringLiteral EarlyExitThresholdConfig { "\n"
                                                   "{comment} Time in seconds to check for early threshold \n"
                                                   "early_exit_threshold: {} \n" };

constexpr StringLiteral ImagesConfig {
    "images:\n"
    "    {comment} Enable or disable sixel scrolling (SM/RM ?80 default) \n"
    "    sixel_scrolling: {} \n"
    "\n"
    "    {comment} Configures the maximum number of color registers available when rendering Sixel "
    "graphics. \n"
    "    sixel_register_count: {} \n"
    "\n"
    "    {comment} maximum width in pixels of an image to be accepted (0 defaults to system screen pixel "
    "width) "
    "\n"
    "    max_width: {} \n"
    "    {comment} maximum height in pixels of an image to be accepted (0 defaults to system screen pixel "
    "height) \n"
    "    max_height: {} \n"
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
    "{comment} color to pick for hyperlinks decoration, when hovering\n"
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

constexpr StringLiteral InputMethodEditorConfig { "\n"
                                                  "{comment} Colors for the IME (Input Method Editor) area.\n"
                                                  "input_method_editor:\n"
                                                  "    foreground: {}\n"
                                                  "    background: {}\n" };

constexpr StringLiteral NormalColorsConfig { "\n"
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

constexpr StringLiteral PlatformPluginWeb {
    "option allows you to override the auto-detected platform plugin to be loaded. You can specify values "
    "like `auto`, `xcb`, `cocoa`, `direct2d`, or `winrt` to determine the platform plugin. The default value "
    "is `auto`."
};

constexpr StringLiteral RendererWeb {
    "section contains configuration options related to the VT Renderer, which is responsible for rendering "
    "the terminal onto the screen. It includes the `backend` option to specify  the rendering backend, with "
    "possible values of `default`, `software`, or `OpenGL`. The other options in this section control the "
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

constexpr StringLiteral PTYReadBufferSizeWeb {
    "option specifies the default PTY read buffer size in bytes. It is an advanced option and should be used "
    "with caution. The default value is `16384`."
};

constexpr StringLiteral PTYBufferObjectSizeWeb {
    "option sets the size in bytes per PTY Buffer Object. It is an advanced option for internal storage and "
    "should be changed carefully. The default value is `1048576`."
};

constexpr StringLiteral DefaultProfilesWeb {
    "option determines the default profile to use in the terminal."
};

constexpr StringLiteral EarlyExitThresholdWeb {
    "option determines the early threshold time. If contour atempts to close earlier than specified "
    "threshold, additional message will be printed that contour terminated too early and additional key "
    "press is required to close contour."
};

constexpr StringLiteral SpawnNewProcessWeb { "flag determines whether a new process should be spawned when "
                                             "creating a new terminal. The default value is `false`." };

constexpr StringLiteral ReflowOnResizeWeb {
    "option controls whether or not the lines in the terminal should be reflowed when a resize event occurs. "
    "The default value is `true`."
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
    "`sixel_scrolling`, `sixel_register_count`, `max_width`, and `max_height` to control various aspects of "
    "image rendering and limits."
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
    ":octicons-horizontal-rule-16: ==ssh.forward_agent== Boolean, indicating wether or not the local SSH "
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
    "This is particularily useful on operating systems (like MacOS) that draw the border frame into the main "
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
    "      auto_scroll_on_update: true\n"
    "      scroll_multiplier: 3\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==limit== This option specifies the number of lines to preserve in the "
    "terminal's history. A value of -1 indicates unlimited history, meaning that all lines are preserved. In "
    "the provided example, the limit is set to 1000. <br/>\n"
    ":octicons-horizontal-rule-16: ==auto_scroll_on_update== This boolean option determines whether the "
    "terminal automatically scrolls down to the bottom when new content is added. If set to true, the "
    "terminal will scroll down on screen updates. If set to false, the terminal will maintain the current "
    "scroll position. In the provided example, auto_scroll_on_update is set to true.  <br/>\n"
    ":octicons-horizontal-rule-16: ==scroll_multiplier== This option defines the number of lines to scroll "
    "when the ScrollUp or ScrollDown events occur. By default, scrolling up or down moves three lines at a "
    "time. You can adjust this value as needed. In the provided example, scroll_multiplier is set to 3. "
    "<br/>\n"
    "\n"
};

constexpr StringLiteral ScrollbarWeb {
    "configuration allows you to customize the appearance and behavior of the visual scrollbar in the "
    "terminal.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    scrollbar:\n"
    "      position: Hidden\n"
    "      hide_in_alt_screen: true\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==position==  This option specifies the position of the scrollbar in the "
    "terminal window. It can be set to one of the following values: Left, Right, Hidden. <br/>\n"
    ":octicons-horizontal-rule-16: ==hide_in_alt_screen== This boolean option determines whether the "
    "scrollbar should be hidden when the terminal is in the alternate screen mode. If set to true, the "
    "scrollbar will be hidden when the terminal switches to the alternate screen. If set to false, the "
    "scrollbar will remain visible even in the alternate screen mode. <br/>\n"
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
    "      dpi_scale: 1.0\n"
    "      locator: native\n"
    "      text_shaping:\n"
    "        engine: native\n"
    "      builtin_box_drawing: true\n"
    "      render_mode: gray\n"
    "      strict_spacing: true\n"
    "      regular:\n"
    "        family: \"monospace\"\n"
    "        weight: regular\n"
    "        slant: normal\n"
    "        features: []\n"
    "      emoji: \"emoji\"\n"
    "```\n"
    ":octicons-horizontal-rule-16: ==size== Specifies the initial font size in pixels. The default value is "
    "12. <br/>\n"
    ":octicons-horizontal-rule-16: ==dpi_scale== Allows applying a DPI scaling factor on top of the system's "
    "configured DPI. The default value is 1.0. <br/>\n"
    ":octicons-horizontal-rule-16: ==locator==  Determines the font locator engine to use for locating font "
    "files and font fallback. Possible values are `native` and `mock`.<br/> `native` will use the "
    "operating-system native font location service (e.g. CoreText on macOS and DirectWrite on Windows), "
    "whereas `mock` is solely used for testing the software (not recommended by end-users)<br/>\n"
    ":octicons-horizontal-rule-16: ==text_shaping.engine== Selects the text shaping and font rendering "
    "engine. Supported values are native, DirectWrite, CoreText, and OpenShaper.  <br/>\n"
    ":octicons-horizontal-rule-16: ==builtin_box_drawing== Specifies whether to use built-in textures for "
    "pixel-perfect box drawing. If disabled, the font's provided box drawing characters will be used. The "
    "default value is true.<br/>\n"
    ":octicons-horizontal-rule-16: ==render_mode== Specifies the font render mode, which tells the font "
    "rasterizer engine what rendering technique to use. Available modes are lcd, light, gray, and "
    "monochrome.  <br/>\n"
    ":octicons-horizontal-rule-16: ==strict_spacing== Indicates whether only monospace fonts should be "
    "included in the font and font fallback list. The default value is true.  <br/>\n"
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
    "can use one of the predefined color palettes as a setting for colors entry. \n"
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
    "      normal: dotted\n"
    "      hover: underline\n"
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

constexpr StringLiteral SeachModeSwitchWeb {
    "The configuration option determines whether the editor should automatically switch from search mode "
    "back to insert mode upon exiting a search. If enabled, the terminal will return to insert mode, "
    "allowing for immediate text input. If disabled, the terminal will remain in normal mode.\n"
    "``` yaml\n"
    "profiles:\n"
    "  profile_name:\n"
    "    search_mode_switch: true\n"
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

using Shell = DocumentationEntry<ShellConfig, ShellWeb>;
using EscapeSandbox = DocumentationEntry<EscapeSandboxConfig, EscapeSandboxWeb>;
using SshHostConfig = DocumentationEntry<SshHostConfigConfig, SshHostConfigWeb>;
using Maximized = DocumentationEntry<MaximizedConfig, MaximizedWeb>;
using Fullscreen = DocumentationEntry<FullscreenConfig, FullscreenWeb>;
using ShowTitleBar = DocumentationEntry<ShowTitleBarConfig, ShowTitleBarWeb>;
using ShowIndicatorOnResize = DocumentationEntry<ShowIndicatorOnResizeConfig, ShowIndicatorOnResizeWeb>;
using Mouse = DocumentationEntry<MouseConfig, MouseWeb>;
using SeachModeSwitch = DocumentationEntry<SeachModeSwitchConfig, SeachModeSwitchWeb>;
using InsertAfterYank = DocumentationEntry<InsertAfterYankConfig, InsertAfterYankWeb>;
using CopyLastMarkRangeOffset = DocumentationEntry<CopyLastMarkRangeOffsetConfig, CopyLastMarkRangeOffsetWeb>;
using WMClass = DocumentationEntry<WMClassConfig, WMClassWeb>;
using Margins = DocumentationEntry<MarginsConfig, MarginsWeb>;
using TerminalSize = DocumentationEntry<TerminalSizeConfig, TerminalSizeWeb>;
using TerminalId = DocumentationEntry<TerminalIdConfig, TerminalIdWeb>;
using History = DocumentationEntry<HistoryConfig, HistoryWeb>;
using Scrollbar = DocumentationEntry<ScrollbarConfig, ScrollbarWeb>;
using StatusLine = DocumentationEntry<StatusLineConfig, StatusLineWeb>;
using OptionKeyAsAlt = DocumentationEntry<OptionKeyAsAltConfig, OptionKeyAsAltWeb>;
using Fonts = DocumentationEntry<FontsConfig, FontsWeb>;
using Permissions = DocumentationEntry<PermissionsConfig, PermissionsWeb>;
using DrawBoldTextWithBrightColors =
    DocumentationEntry<DrawBoldTextWithBrightColorsConfig, DrawBoldTextWithBrightColorsWeb>;
using Colors = DocumentationEntry<ColorsConfig, ColorsWeb>;
using ModalCursorScrollOff = DocumentationEntry<ModalCursorScrollOffConfig, ModalCursorScrollOffWeb>;
using ModeInsert = DocumentationEntry<ModeInsertConfig, ModeInsertWeb>;
using ModeNormal = DocumentationEntry<ModeNormalConfig, ModeNormalWeb>;
using ModeVisual = DocumentationEntry<ModeVisualConfig, ModeVisualWeb>;
using SmoothLineScrolling = DocumentationEntry<SmoothLineScrollingConfig, SmoothLineScrollingWeb>;
using HighlightTimeout = DocumentationEntry<HighlightTimeoutConfig, HighlightTimeoutWeb>;
using HighlightDoubleClickerWord =
    DocumentationEntry<HighlightDoubleClickerWordConfig, HighlightDoubleClickerWordWeb>;
using Background = DocumentationEntry<BackgroundConfig, BackgroundWeb>;
using Bell = DocumentationEntry<BellConfig, BellWeb>;
using FrozenDecMode = DocumentationEntry<FrozenDecModeConfig, Dummy>;
using Live = DocumentationEntry<LiveConfig, LiveWeb>;
using PlatformPlugin = DocumentationEntry<PlatformPluginConfig, PlatformPluginWeb>;
using Renderer = DocumentationEntry<RendererConfig, RendererWeb>;
using PTYReadBufferSize = DocumentationEntry<PTYReadBufferSizeConfig, PTYReadBufferSizeWeb>;
using PTYBufferObjectSize = DocumentationEntry<PTYBufferObjectSizeConfig, PTYBufferObjectSizeWeb>;
using ReflowOnResize = DocumentationEntry<ReflowOnResizeConfig, ReflowOnResizeWeb>;
using ColorSchemes = DocumentationEntry<ColorSchemesConfig, Dummy>;
using Profiles = DocumentationEntry<ProfilesConfig, ProfilesWeb>;
using DefaultProfiles = DocumentationEntry<StringLiteral { "default_profile: {}\n" }, DefaultProfilesWeb>;
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
using ExperimentalFeatures = DocumentationEntry<ExperimentalFeaturesConfig, StringLiteral { "" }>;
using DefaultColors = DocumentationEntry<DefaultColorsConfig, Dummy>;
using HyperlinkDecoration = DocumentationEntry<HyperlinkDecorationConfig, HyperlinkDecorationWeb>;
using YankHighlight = DocumentationEntry<YankHighlightConfig, Dummy>;
using NormalModeCursorline = DocumentationEntry<NormalModeCursorlineConfig, Dummy>;
using Selection = DocumentationEntry<SelectionConfig, Dummy>;
using SearchHighlight = DocumentationEntry<SearchHighlightConfig, Dummy>;
using SearchHighlightFocused = DocumentationEntry<SearchHighlightFocusedConfig, Dummy>;
using WordHighlightCurrent = DocumentationEntry<WordHighlightCurrentConfig, Dummy>;
using WordHighlight = DocumentationEntry<WordHighlightConfig, Dummy>;
using IndicatorStatusLine = DocumentationEntry<IndicatorStatusLineConfig, Dummy>;
using InputMethodEditor = DocumentationEntry<InputMethodEditorConfig, Dummy>;
using NormalColors = DocumentationEntry<NormalColorsConfig, Dummy>;
using BrightColors = DocumentationEntry<BrightColorsConfig, Dummy>;
using DimColors = DocumentationEntry<DimColorsConfig, Dummy>;

} // namespace contour::config::documentation
