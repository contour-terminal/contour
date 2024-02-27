# Configuring Contour

Contour offers a wide range of configuration options that can be customized, including color scheme, shell, initial working directory, and more.
The configuration options can be categorized into several groups:

- Global options: These settings determine the overall behavior of the terminal and apply to all profiles.<br/>
- Profiles: With profiles, you can configure the terminal more granularly and create multiple profiles that can be easily switched between.<br/>
- Color scheme: Contour allows you to define different color schemes for the terminal and choose which one to use for each of the profiles. <br/>


On Unix systems, the main configuration file is located at  `~/.config/contour/contour.yml` and is both read from and auto-generated there. On Windows systems, the file is located at  `%LocalAppData%\contour\contour.yml`.

!!! note "Please note that on Unix systems, the environment variable `XDG_CONFIG_HOME` (by default set to `~/.config`) is taken into account."

By default, on Unix systems, Contour is executed with the following arguments `contour config ~/.config/contour/contour.yml profile main`. If the configuration file includes a `default_profile` variable, it will be used as the default profile. Otherwise, the first profile listed in the file will be the default one.
## How to

### Load specific configuration file
`contour config /path/to/file/with/configuration.yml`
### Set profile for current session
you can utilize the `profile` parameter with the `contour` command <br/>
`contour profile one_of_profiles`


## Global options

Let's go through the different sections of the global configurations in the file:

### `platform_plugin`
option allows you to override the auto-detected platform plugin to be loaded. You can specify values like `auto`, `xcb`, `cocoa`, `direct2d`, or `winrt` to determine the platform plugin. The default value is `auto`. <br/>
### `renderer`
section contains configuration options related to the VT Renderer, which is responsible for rendering the terminal onto the screen. It includes the `backend` option to specify  the rendering backend, with possible values of `default`, `software`, or `OpenGL`. The other options in this section control the tile mapping and caching for performance optimization. <br/>
### `word_delimiters`
option defines the delimiters to be used when selecting words in the terminal. It is a string of characters that act as delimiters. <br/>
### `read_buffer_size`
option specifies the default PTY read buffer size in bytes. It is an advanced option and should be used with caution. The default value is `16384`. <br/>
### `pty_buffer_size`
option sets the size in bytes per PTY Buffer Object. It is an advanced option for internal storage and should be changed carefully. The default value is `1048576`. <br/>
### `default_profile`
option determines the default profile to use in the terminal. <br/>
### 'early_exit_threshold' 
option determines the early threshold time. If contour atempts to close earlier than specified threshold, additional message will be printed that contour terminated too early and additional key press is required to close contour. <br/>
### `spawn_new_process`
flag determines whether a new process should be spawned when creating a new terminal. The default value is `false`. <br/>
### `reflow_on_resize`
option controls whether or not the lines in the terminal should be reflowed when a resize event occurs. The default value is `true`. <br/>
### `bypass_mouse_protocol_modifier`
option specifies the keyboard modifier (e.g., Shift) that can be used to bypass the terminal's mouse protocol and select screen content. <br/>
### `mouse_block_selection_modifier`
option determines the modifier (e.g., Control) that needs to be pressed to initiate block selection using the left mouse button. <br/>
### `on_mouse_select`
option selects the action to perform when a text selection has been made. Possible values include `None`, `CopyToClipboard`, and `CopyToSelectionClipboard`. <br/>
### `live_config`
option determines whether the instance should reload the configuration files whenever they change. The default value is `false`. <br/>
### `images`
section contains configuration options related to inline images. It includes options like `sixel_scrolling`, `sixel_register_count`, `max_width`, and `max_height` to control various aspects of image rendering and limits. <br/>
### `input_mapping`
This section sets user defined key bindings

### Defaut global parameters

```yaml
platform_plugin: auto
renderer:
    backend: OpenGL
    tile_hashtable_slots: 4096
    tile_cache_count: 4000
    tile_direct_mapping: true
word_delimiters: " /\\()\"'-.,:;<>~!@#$%^&*+=[]{}~?|│"
read_buffer_size: 16384
pty_buffer_size: 1048576
default_profile: main
spawn_new_process: false
reflow_on_resize: true
bypass_mouse_protocol_modifier: Shift
mouse_block_selection_modifier: Control
on_mouse_select: CopyToSelectionClipboard
live_config: false
images:
    sixel_scrolling: true
    sixel_register_count: 4096
    max_width: 0
    max_height: 0

```

The default profile is automatically the top (first) defined profile in the configuration file, but can be explicitly set to an order-independant name using `default_profile` configuration key.


## Profiles
Profiles is the main part of user specific customizations, you can create more than one profile and chose which you want to use during startup or define in configuration file.


By default each profile inherites values from `default_profile`. This means that you can specify only values that you want to change in respect to default profile, for example you can create new profile to use `bash` as a shell preserving other configuration from `main` profile
```
profiles:
    main:
    # default profile here
    bash:
        shell: "/usr/bin/bash"

```

For the full list of options see generated configuration file on your system or [Profiles](profiles.md) section of documentation.


## Color Schemes
In contour you can specify different colors inside terminal, for example text background and foreground, cursor properties, selection colors and plenty others.
You can configure your color profiles, whereas a color can be expressed in standard web format, with a leading # followed by red/green/blue values, 7 characters in total. You may alternatively use 0x as prefix instead of #. For example 0x102030 is equal to '#102030'.

Syntax for color shemes repeat the one of profiles. First color scheme inside configuration file must be named `default`, each other color schemes inherit values from `default` color scheme. Example of configuration for `color_schemes`
```
color_schemes:
    default:
    # values for default color scheme
    different_selection:
      selection:
        background: '#fff0f0'
```

For the full list of options see generated configuration file on your system or [Colors](colors.md) section of documentation.
