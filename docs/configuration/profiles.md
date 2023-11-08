
All profiles inside configuration files share parent node `profiles`. To create profile you need to specify child node
```yaml
profiles:
    main:
    #default configuration
    profile_for_windows:
    # windows specific entries
    profile_for_macos:
    # macos specific entries

```

## Profile configuration

### `shell`
configuratin section allows you to specify the process to be started inside the terminal. It provides flexibility to override the default login shell and supports logging in to a remote host.
``` yaml
profiles:
  profile_name:
    shell: "/bin/bash"
    arguments: ["some", "optional", "arguments", "for", "the", "shell"]
```
:octicons-horizontal-rule-16: ==arguments== (optional) Allows you to provide additional command-line arguments to the shell executable. These arguments will be passed to the shell when it is started inside the terminal.

### `escape_sandbox`
option in the configuration file allows you to control the sandboxing behavior when the terminal is executed from within Flatpak. This configuration is relevant only if the terminal is running in a Flatpak environment.
``` yaml
profiles:
  profile_name:
    escape_sandbox: true
```

### `ssh`

With this key, you can bypass local PTY and process execution and directly connect via TCP/IP to a remote SSH server.

```yaml
profiles:
  profile_name:
    ssh:
      host: remote-server.example.com
      port: 22
      user: "CustomUserName"
      private_key: "path/to/key"
      public_key: "path/to/key.pub"
      known_hosts: "~/.ssh/known_hosts"
      forward_agent: false
```

Note, only `host` option is required. Everything else is defaulted.
Keep in mind, that the user's `~/.ssh/config` will be parsed with respect to the supported options above.
These values can be overridden in the local Contour configuration as follows:

:octicons-horizontal-rule-16: ==ssh.host== SSH server to establish the connection to.
:octicons-horizontal-rule-16: ==ssh.port== SSH port (defaults to `22`). Only specify this value if it is deviating from the default value `22`.
:octicons-horizontal-rule-16: ==ssh.private_key== Path to private key to use for key based authentication.
:octicons-horizontal-rule-16: ==ssh.public_key== Path to public key that belongs to the private key. When using key based authentication, it depends on the underlying backend, if the public key is also required. OpenSSL for example does not require it.
:octicons-horizontal-rule-16: ==ssh.known_hosts== Path to `known_hosts` file. This defaults to and usually is located in `~/.ssh/known_hosts`.
:octicons-horizontal-rule-16: ==ssh.forward_agent== Boolean, indicating wether or not the local SSH auth agent should be requested to be forwarded. Note: this is currently not working due to an issue related to the underlying library being used, but is hopefully resolved soon.

Note, custom environment variables may be passed as well, when connecting to an SSH server using this builtin-feature. Mind,
that the SSH server is not required to accept all environment variables.

If an OpenSSH server is used, have a look at the `AcceptEnv` configuration setting in the `sshd_config`
configuration file on the remote SSH server, to configure what environment variables are permitted to be sent.

### `copy_last_mark_range_offset`
configuration option is an advanced setting that is useful when using the CopyPreviousMarkRange feature with multiline prompts. It allows you to specify an offset value that is added to the current cursor's line number minus 1 (i.e., the line above the current cursor).
``` yaml
profiles:
  profile_name:
    copy_last_mark_range_offset: 0
```


### `initial_working_directory`
configuration option allows you to specify the initial working directory when a new terminal is spawned. The value specified here determines the directory in which the terminal will be opened.
``` yaml
profiles:
  profile_name:
    initial_working_directory: "~"
```


### `show_title_bar`
configuration option determines whether or not the title bar will be shown when the terminal profile is activated.
``` yaml
profiles:
  profile_name:
    show_title_bar: true
```


### `size_indicator_on_resize`
configuration option determines whether or not the size indicator will be shown when terminal will resized.
``` yaml
profiles:
  profile_name:
    size_indicator_on_resize: true
```


### `fullscreen`
configuration option determines whether the terminal's screen should be put into fullscreen mode when the terminal profile is activated. Fullscreen mode expands the terminal window to occupy the entire screen, providing a distraction-free environment for your terminal sessions.
``` yaml
profiles:
  profile_name:
    fullscreen: false
```



### `maximized`
configuration option determines whether the terminal window should be maximized when the specified profile is activated. Maximizing a window expands it to fill the entire available space on the screen, excluding the taskbar or other system elements.
``` yaml
profiles:
  profile_name:
    maximized: false
```



### `bell`

Configuration section permits tuning the behavior of the terminal bell.

``` yaml
profiles:
  profile_name:
    bell:
      sound: "default"
      alert: false
```
:octicons-horizontal-rule-16: ==sound== This option determines the sound of `BEL` (also `\a` or `0x07`) to `off` or `default` or sound generated by a file located at `path`. <br />
:octicons-horizontal-rule-16: ==alert== This option determines whether or not a window alert will be raised each time a bell is ringed. Useful for tiling window managers like i3 or sway.


### `wm_class`
Configuration option defines the class part of the `WM_CLASS` property of the terminal window. The `WM_CLASS` property is a standard X11 property used to identify and classify windows by their class and instance.
``` yaml
profiles:
  profile_name:
    wm_class: "contour"
```



### `environment`
configuration option allows you to define environment variables that will be passed to the shell when the terminal is started. Environment variables are used to customize the behavior and configuration of programs running in the terminal.
``` yaml
profiles:
  profile_name:
    environment:
      TERM: contour
      COLORTERM: truecolor
```
:octicons-horizontal-rule-16: ==TERM==  The TERM variable is commonly used to specify the terminal type or emulation. In this case, it is set to "contour".  <br/>
:octicons-horizontal-rule-16: ==COLORTERM== The COLORTERM variable is used to indicate support for true color (24-bit color) in the terminal. Setting it to "truecolor" indicates that the terminal supports true color rendering.



### `terminal_id`
configuration option allows you to specify the terminal type that will be advertised by the terminal emulator. The terminal type indicates the set of capabilities and features that the terminal supports, enabling compatibility with different applications and systems.
``` yaml
profiles:
  profile_name:
    terminal_id: VT525
```



### `terminal_size`
 configuration option allows you to specify the initial size of the terminal window in terms of the number of columns and lines.
``` yaml
profiles:
  profile_name:
    terminal_size:
      columns: 80
      lines: 25
```
:octicons-horizontal-rule-16: ==columns== This option specifies the number of columns (characters) to be displayed in the terminal window. In the provided example, the value is set to 80. <br/>
:octicons-horizontal-rule-16: ==lines== This option specifies the number of lines to be displayed in the terminal window. In the provided example, the value is set to 25. <br/>



### `history`
configuration allows you to customize the behavior and settings related to the terminal's history, including the number of preserved lines, auto-scrolling, and scroll events.
``` yaml
profiles:
  profile_name:
    history:
      limit: 1000
      auto_scroll_on_update: true
      scroll_multiplier: 3
```
:octicons-horizontal-rule-16: ==limit== This option specifies the number of lines to preserve in the terminal's history. A value of -1 indicates unlimited history, meaning that all lines are preserved. In the provided example, the limit is set to 1000. <br/>
:octicons-horizontal-rule-16: ==auto_scroll_on_update== This boolean option determines whether the terminal automatically scrolls down to the bottom when new content is added. If set to true, the terminal will scroll down on screen updates. If set to false, the terminal will maintain the current scroll position. In the provided example, auto_scroll_on_update is set to true.  <br/>
:octicons-horizontal-rule-16: ==scroll_multiplier== This option defines the number of lines to scroll when the ScrollUp or ScrollDown events occur. By default, scrolling up or down moves three lines at a time. You can adjust this value as needed. In the provided example, scroll_multiplier is set to 3. <br/>



### `scrollbar`
configuration allows you to customize the appearance and behavior of the visual scrollbar in the terminal.
``` yaml
profiles:
  profile_name:
    scrollbar:
      position: Hidden
      hide_in_alt_screen: true
```
:octicons-horizontal-rule-16: ==position==  This option specifies the position of the scrollbar in the terminal window. It can be set to one of the following values: Left, Right, Hidden. <br/>
:octicons-horizontal-rule-16: ==hide_in_alt_screen== This boolean option determines whether the scrollbar should be hidden when the terminal is in the alternate screen mode. If set to true, the scrollbar will be hidden when the terminal switches to the alternate screen. If set to false, the scrollbar will remain visible even in the alternate screen mode. <br/>



### `mouse`
configuration allows you to control the behavior of the mouse in the terminal.
``` yaml
profiles:
  profile_name:
    mouse:
      hide_while_typing: true
```
:octicons-horizontal-rule-16: ==hide_while_typing== This boolean option determines whether the mouse cursor should be hidden while typing in the terminal. When set to true, the mouse cursor will be hidden when you start typing. When set to false, the mouse cursor will remain visible while typing. <br/>


### `permissions`
configuration allows you to control the access permissions for specific VT sequences in the terminal.
``` yaml
profiles:
  profile_name:
    permissions:
      change_font: ask
      capture_buffer: ask
      display_host_writable_statusline: ask
```
:octicons-horizontal-rule-16: ==change_font== This option determines the access permission for changing the font using the VT sequence `OSC 50 ; Pt ST`. The possible values are: allow, deny, ask. <br/>
:octicons-horizontal-rule-16: ==capture_buffer== This option determines the access permission for capturing the screen buffer using the VT sequence `CSI > Pm ; Ps ; Pc ST`. The response can be read from stdin as the sequence `OSC 314 ; <screen capture> ST`. The possible values are: allow, deny, ask.<br/>
:octicons-horizontal-rule-16: ==display_host_writable_statusline== This option determines the access permission for displaying the "Host Writable Statusline" programmatically using the VT sequence `DECSSDT 2`. The possible values are: allow, deny, ask. <br/>



### `highlight_word_and_matches_on_double_click`
configuration option enables the highlighting of a word and its matches when double-clicked on the primary screen in the terminal.
``` yaml
profiles:
  profile_name:
    highlight_word_and_matches_on_double_click: true
```
:octicons-horizontal-rule-16: ==change_font==  When this option is enabled (true), the following behavior occurs: <br/>
    - Double-clicking on a word in the primary screen will select and highlight the double-clicked word. <br/>
    - Additionally, all other occurrences of the same word will also be highlighted without being selected. <br/>
    - This feature is implemented by initiating a search for the double-clicked word. <br/>
    - You can use the FocusNextSearchMatch and FocusPreviousSearchMatch actions to navigate to the next or previous occurrence of the same word, even if it is outside the current viewport. <br/>



### `font`
section of the configuration allows you to customize the font settings for the terminal.
``` yaml
profiles:
  profile_name:
    font:
      size: 12
      dpi_scale: 1.0
      locator: native
      text_shaping:
        engine: native
      builtin_box_drawing: true
      render_mode: gray
      strict_spacing: true
      regular:
        family: "monospace"
        weight: regular
        slant: normal
        features: []
      emoji: "emoji"
```
:octicons-horizontal-rule-16: ==size== Specifies the initial font size in pixels. The default value is 12. <br/>
:octicons-horizontal-rule-16: ==dpi_scale== Allows applying a DPI scaling factor on top of the system's configured DPI. The default value is 1.0. <br/>
:octicons-horizontal-rule-16: ==locator==  Determines the font locator engine to use for locating font files and font fallback. Possible values are native, fontconfig, CoreText, and DirectWrite.<br/>
:octicons-horizontal-rule-16: ==text_shaping.engine== Selects the text shaping and font rendering engine. Supported values are native, DirectWrite, CoreText, and OpenShaper.  <br/>
:octicons-horizontal-rule-16: ==builtin_box_drawing== Specifies whether to use built-in textures for pixel-perfect box drawing. If disabled, the font's provided box drawing characters will be used. The default value is true.<br/>
:octicons-horizontal-rule-16: ==render_mode== Specifies the font render mode, which tells the font rasterizer engine what rendering technique to use. Available modes are lcd, light, gray, and monochrome.  <br/>
:octicons-horizontal-rule-16: ==strict_spacing== Indicates whether only monospace fonts should be included in the font and font fallback list. The default value is true.  <br/>
:octicons-horizontal-rule-16: ==regular== Defines the regular font style with the following parameters:  <br/>
:octicons-horizontal-rule-16: ==regular.family==  Specifies the font family name, such as "monospace", "Courier New", or "Fira Code". <br/>
:octicons-horizontal-rule-16: ==regular.weight==  Specifies the font weight, such as thin, extra_light, light, demilight, book, normal, medium, demibold, bold, extra_bold, black, or extra_black. <br/>
:octicons-horizontal-rule-16: ==regular.slant==  Specifies the font slant, which can be normal, italic, or oblique.
:octicons-horizontal-rule-16: ==regular.features== Sets optional font features to be enabled. This is usually a 4-letter code, such as ss01 or ss02. Refer to your font's documentation for supported features. By default, no features are enabled. <br/>
:octicons-horizontal-rule-16: ==emoji== Specifies the font to be used for displaying Unicode symbols with emoji presentation. The default value is "emoji". <br/>



### `draw_bold_text_with_bright_colors`
Specifies whether bold text should be rendered in bright colors for indexed colors. If disabled, normal colors will be used for bold text. The default value is false.
``` yaml
profiles:
  profile_name:
   draw_bold_text_with_bright_colors: false
```



### `cursor`
section of the configuration allows you to customize the appearance and behavior of the terminal cursor.
``` yaml
profiles:
  profile_name:
   cursor:
    shape: "bar"
    blinking: false
    blinking_interval: 500
```
:octicons-horizontal-rule-16: ==shape== Specifies the shape of the cursor. You can choose from the following options: <br/>
-block: A filled rectangle. <br/>
-rectangle: Just the outline of a block. <br/>
-underscore: A line under the text. <br/>
-bar: The well-known i-Beam cursor. <br/>
:octicons-horizontal-rule-16: ==blinking== Determines whether the cursor should blink over time. If set to true, the cursor will blink; if set to false, the cursor will remain static. <br/>
:octicons-horizontal-rule-16: ==blinking_interval== Specifies the blinking interval in milliseconds. This value defines how quickly the cursor alternates between being visible and invisible when blinking is enabled. <br/>


### `normal_mode`
section in the configuration allows you to customize the appearance and behavior of the cursor specifically in vi-like normal mode.
``` yaml
profiles:
  profile_name:
    normal_mode:
      cursor:
        shape: block
        blinking: false
        blinking_interval: 500
```



### `visual_mode`
section in the configuration allows you to customize the appearance and behavior of the cursor specifically in vi-like normal mode.
``` yaml
profiles:
  profile_name:
    normal_mode:
      cursor:
        shape: block
        blinking: false
        blinking_interval: 500
```



### `vi_mode_highlight_timeout`
option in the configuration determines the duration in milliseconds for which the yank highlight is shown in vi mode. After yanking (copying) text in vi mode, the yanked text is typically highlighted momentarily to provide visual feedback. This configuration option allows you to specify the duration of this highlight.
``` yaml
profiles:
  profile_name:
    vi_mode_highlight_timeout: 300
```



### `vi_mode_scrolloff`
option in the configuration sets the scrolloff value for cursor movements in normal and visual (block) modes. The scrolloff value determines the minimum number of lines to keep visible above and below the cursor when scrolling. In other words, it controls the amount of margin or padding around the cursor during scrolling operations.
``` yaml
profiles:
  profile_name:
    vi_mode_scrolloff: 8
```



### `status_line`
section in the configuration file allows you to customize the behavior and appearance of the status line in the terminal.
``` yaml
profiles:
  profile_name:
    status_line:
      display: none
      position: bottom
      sync_to_window_title: false
```
:octicons-horizontal-rule-16: ==display== specifies whether the status line should be shown or not. The possible values are none (status line is not shown) and indicator (status line is shown). In the example, the status line is set to none, meaning it will not be displayed initially. <br/>
:octicons-horizontal-rule-16: ==position== determines the placement of the status line. It can be set to top or bottom. In the example, the status line is set to bottom, indicating that it will appear at the bottom of the terminal window if enabled. <br/>
:octicons-horizontal-rule-16: ==sync_to_window_title== controls whether the window title should be synchronized with the Host Writable status line. If the Host Writable status line is denied, enabling this option will update the window title accordingly. By default, this option is set to false. <br/>



### `background`
section in the configuration file allows you to customize the background settings for the terminal.
``` yaml
profiles:
  profile_name:
    background:
      opacity: 1.0
      blur: false
```
:octicons-horizontal-rule-16: ==opacity== specifies the background opacity to use. The value ranges from 0.0 to 1.0, where 0.0 represents fully transparent and 1.0 represents fully opaque. You can adjust this value to control the transparency level of the terminal's background. <br/>
:octicons-horizontal-rule-16: ==blur== determines whether the transparent background should be blurred on platforms that support it. Currently, only Windows 10 is supported for background blurring. By default, this option is set to false, meaning no background blurring will be applied. <br/>



### `colors`
section in the configuration file allows you to specify the colorscheme to use for the terminal.
``` yaml
profiles:
  profile_name:
    colors: "default"
```

To make the terminal's color scheme dependant on OS appearance (dark and light mode) settings,
you need to specify two color schemes:

```yaml
profiles:
  profile_name:
    colors:
      dark: "some_dark_scheme_name"
      light: "some_light_scheme_name"
```

With this, the terminal will use the color scheme as specified in `dark` when OS dark mode is on,
and `light`'s color scheme otherwise.

### `hyperlink_decoration:`
section in the configuration file allows you to configure the styling and colorization of hyperlinks when they are displayed in the terminal and when they are hovered over by the cursor.
``` yaml
profiles:
  profile_name:
    hyperlink_decoration:
      normal: dotted
      hover: underline
```

## Default profile
``` yaml
profiles:
    main:
        escape_sandbox: true
        copy_last_mark_range_offset: 0
        initial_working_directory: "~"
        show_title_bar: true
        fullscreen: false
        maximized: false
        wm_class: "contour"
        terminal_id: VT525
        terminal_size:
            columns: 80
            lines: 25
        history:
            limit: 1000
            auto_scroll_on_update: true
            scroll_multiplier: 3
        scrollbar:
            position: Hidden
            hide_in_alt_screen: true
        mouse:
            hide_while_typing: true
        permissions:
            change_font: ask
            capture_buffer: ask
            display_host_writable_statusline: ask
        highlight_word_and_matches_on_double_click: true
        font:
            size: 12
            dpi_scale: 1.0
            locator: native
            text_shaping:
                engine: native
            builtin_box_drawing: true
            render_mode: gray
            strict_spacing: true
            regular:
                family: "monospace"
                weight: regular
                slant: normal
                features: []
            emoji: "emoji"
        draw_bold_text_with_bright_colors: false
        cursor:
            shape: "bar"
            blinking: false
            blinking_interval: 500
        normal_mode:
            cursor:
                shape: block
                blinking: false
                blinking_interval: 500
        visual_mode:
            cursor:
                shape: block
                blinking: false
                blinking_interval: 500
        vi_mode_highlight_timeout: 300
        vi_mode_scrolloff: 8
        status_line:
            display: none
            position: bottom
            sync_to_window_title: false
        background:
            opacity: 1.0
            blur: false
        colors: "default"
        hyperlink_decoration:
            normal: dotted
            hover: underline

```
