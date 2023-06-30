# Configuring Contour

Contour offers a wide range of configuration options that can be customized, including color scheme, shell, initial working directory, and more. <br/>
The configuration options can be categorized into several groups:

:octicons-circle-24: Global options: These settings determine the overall behavior of the terminal and apply to all profiles.<br/>
:octicons-circle-24: Profiles: With profiles, you can configure the terminal more granularly and create multiple profiles that can be easily switched between.<br/>
:octicons-circle-24: Color scheme: Contour allows you to define different color schemes for the terminal and choose which one to use for each of profiles. <br/>


On Unix systems, the main configuration file is located at  `~/.config/contour/contour.yml` and is both read from and auto-generated there. <br/>
On Windows systems, the file is located at  `%LocalAppData%\contour\contour.yml`.

Please note that on Unix systems, the environment variable `XDG_CONFIG_HOME` (by default set to `~/.config`) is taken into account.

If you want to load a specific configuration file, you can use the command `contour config /path/to/file/with/configuration.yml`. <br/>
Additionally, you can specify the profile to use in the current session by providing the `profile` value to contour:  `contour profile one_of_profiles`. <br/>
By default, on Unix systems, Contour is executed with the following arguments: `contour config ~/.config/contour/contour.yml profile default_profile`. If the `default_profile` variable is specified in the configuration file, it will be used as the default profile. If not, the first profile in the file will be used as the default.

## Global options

Let's go through the different sections of the global configurations in the file:

`platform_plugin`: This option allows you to override the auto-detected platform plugin to be loaded. You can specify values like `auto`, `xcb`, `cocoa`, `direct2d`, or `winrt` to determine the platform plugin. The default value is `auto`. <br/>
`renderer`: This section contains configuration options related to the VT Renderer, which is responsible for rendering the terminal onto the screen. It includes the `backend` option to specify  the rendering backend, with possible values of `default`, `software`, or `OpenGL`. The other options in this section control the tile mapping and caching for performance optimization. <br/>
`word_delimiters`: This option defines the delimiters to be used when selecting words in the terminal. It is a string of characters that act as delimiters. <br/>
`read_buffer_size`: This option specifies the default PTY read buffer size in bytes. It is an advanced option and should be used with caution. The default value is `16384`. <br/>
`pty_buffer_size`: This option sets the size in bytes per PTY Buffer Object. It is an advanced option for internal storage and should be changed carefully. The default value is `1048576`. <br/>
`default_profile`: This option determines the default profile to use in the terminal. <br/>
`spawn_new_process`: This flag determines whether a new process should be spawned when creating a new terminal. The default value is `false`. <br/>
`reflow_on_resize`: This option controls whether or not the lines in the terminal should be reflowed when a resize event occurs. The default value is `true`. <br/>
`bypass_mouse_protocol_modifier`: This option specifies the keyboard modifier (e.g., Shift) that can be used to bypass the terminal's mouse protocol and select screen content. <br/>
`mouse_block_selection_modifier`: This option determines the modifier (e.g., Control) that needs to be pressed to initiate block selection using the left mouse button. <br/>
`on_mouse_select`: This option selects the action to perform when a text selection has been made. Possible values include `None`, `CopyToClipboard`, and `CopyToSelectionClipboard`. <br/>
`live_config`: This option determines whether the instance should reload the configuration files whenever they change. The default value is `false`. <br/>
`images`: This section contains configuration options related to inline images. It includes options like `sixel_scrolling`, `sixel_register_count`, `max_width`, and `max_height` to control various aspects of image rendering and limits. <br/>
`input_mapping` : This section sets user defined key bindings

The default profile is automatically the top (first) defined profile in the configuration file, but can be explicitly set to an order-independant name using `default_profile` configuration key.


## Profiles
Profiles is the main part of user specific customizations, you can create more than one profile and chose which you want to use during startup or define in configuration file.

For the full list of options see generated configuration file on your system or [Profiles](profiles.md) section of documentation.

By default each profile inherites values from `default_profile`. This means that you can specify only values that you want to change in respect to default profile, for example you can create new profile to use `bash` as a shell preserving other configuration from `main` profile
```
profiles:
    main:
    # default profile here
    bash:
        shell: "/usr/bin/bash"

```
