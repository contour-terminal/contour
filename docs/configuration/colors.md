Color scheme configure colors that are used inside terminal. To specify which color scheme you want to use you need to define the name of the scheme inside profile configuration or inline colors inside profile.

## Color scheme configuration

### `default`
section defines the default colors used in the terminal.
``` yaml
color_schemes:
  default:
    default:
      background: '#1a1716'
      foreground: '#d0d0d0'
      bright_foreground: '#ffffff'
      dimmed_foreground: '#808080'


```
:octicons-horizontal-rule-16: ==background==  determines the default background color of the terminal. <br/>
:octicons-horizontal-rule-16: ==foreground==  option sets the default foreground text color of the terminal. <br/>
:octicons-horizontal-rule-16: ==bright\_foreground==  option sets the default foreground text color of the terminal when text text is instructed to be bold (/bright). This is only used if profile option `draw_bold_text_with_bright_colors` is set to `true`<br/>
:octicons-horizontal-rule-16: ==dimmed\_foreground==  option sets the default foreground text color of the terminal when text text is instructed to be dimmed. <br/>



### `background_image`
section in the configuration file allows you to control various aspects of the background image feature.
``` yaml
color_schemes:
  default:
    background_image:
      path: '/path/to/image.png'
      opacity: 0.5
      blur: false

```
:octicons-horizontal-rule-16: ==path== To specify the image file to use as the background, you need to provide the full path to the image. By default, the path option is set to an empty string, indicating that background image support is disabled. <br/>
:octicons-horizontal-rule-16: ==opacity== option controls the opacity of the background image. It determines how transparent or intense the image appears. The default value is 0.5, which provides a moderately transparent background. You can adjust this value to make the image more or less prominent, depending on your preferences. <br/>
:octicons-horizontal-rule-16: ==blur== option applies a blur effect to the background image. This can help reduce distractions and keep the focus on the terminal contents. By default, the blur option is set to false, indicating that background image blurring is disabled. If you want to enable it, set the value to true.




### `cursor`
section in the configuration file let you customize the color of the cursor and optionally override the text color.
``` yaml
color_schemes:
  default:
    cursor:
      default: CellForeground
      text: CellBackground
```
:octicons-horizontal-rule-16: ==default== option allows you to specify the color of the cursor shape. By default, the default value is set to CellForeground, which means the cursor color will be the same as the cell's foreground color. You can change this value to any valid RGB color or use the special options CellForeground or CellBackground. <br/>
:octicons-horizontal-rule-16: ==text== allows you to specify the color of the characters that would be covered by the cursor shape. This is useful for ensuring readability when the cursor overlaps with the text. By default, the text value is set to CellBackground, which means the overridden text color will be the same as the cell's background color. Similarly to the default option, you can specify any valid RGB color or use the CellForeground or CellBackground options.



### `hyperlink_decoration`
allows you to customize the colors used for hyperlink decoration when hovering over them.
``` yaml
color_schemes:
  default:
    hyperlink_decoration:
      normal: '#f0f000'
      hover: '#ff0000'
```
:octicons-horizontal-rule-16: ==normal==  defines the color to be used for hyperlink decoration in its normal state (not being hovered). <br/>
:octicons-horizontal-rule-16: ==hover==  determines the color to be applied to hyperlink decoration when it is being hovered over.



### `vi_mode_highlight`
customize the colors used for highlighting in Vi mode.
``` yaml
color_schemes:
  default:
    vi_mode_highlight:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#ffa500'
      background_alpha: 0.5
```
:octicons-horizontal-rule-16: ==foreground==  section specifies the color to be used for the foreground of the highlighted text. <br/>
:octicons-horizontal-rule-16: ==foreground_alpha== option determines the transparency level of the foreground color. It accepts a value between 0.0 (fully transparent) and 1.0 (fully opaque). This option allows you to control the visibility of the highlighted text. <br/>
:octicons-horizontal-rule-16: ==background== option in the vi_mode_highlight section defines the color to be used for the background of the highlighted text. <br/>
:octicons-horizontal-rule-16: ==background_alpha== option controls the transparency level of the background color. It accepts a value between 0.0 (fully transparent) and 1.0 (fully opaque).



### `vi_mode_cursorline`
customize the colors used for the cursor line in Vi mode. Options similar to `vi_mode_highlight`.
``` yaml
color_schemes:
  default:
    vi_mode_cursorline:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#ffa500'
      background_alpha: 0.5
```



### `selection`
section customize the colors used for text selection. Options similar to `vi_mode_highlight`.
``` yaml
color_schemes:
  default:
    selection:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#4040f0'
      background_alpha: 0.5
```




### `search_highlight`
section customize the colors used for search highlight. Options similar to `vi_mode_highlight`.
``` yaml
color_schemes:
  default:
    search_highlight:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#4040f0'
      background_alpha: 0.5
```



### `search_highlight_focused`
section customize the colors used for focused search highlight. Options similar to `vi_mode_highlight`.
``` yaml
color_schemes:
  default:
    search_highlight_focused:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#4040f0'
      background_alpha: 0.5
```



### `word_highlight_current`
Coloring for the word that is highlighted due to double-clicking it. Options similar to `vi_mode_highlight`.
``` yaml
color_schemes:
  default:
    word_highlight_current:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#4040f0'
      background_alpha: 0.5
```



### `word_highlight_other`
Coloring for the word that is highlighted due to double-clicking another word that matches this word. Options similar to `vi_mode_highlight`.
``` yaml
color_schemes:
  default:
    word_highlight_other:
      foreground: CellForeground
      foreground_alpha: 1.0
      background: '#4040f0'
      background_alpha: 0.5
```



### `indicator_statusline`
Defines the colors to be used for the Indicator status line. Configuration entry consist of following sections, namely `default`, `inactive`, `insert_mode`, `normal_mode`, `visual_mode`. Each section have `foreground` and `background` options. You can specify only the sections you want to customize after configuring the `default` section.
Minimal configuration looks like this:
``` yaml
color_schemes:
  default:
    indicator_statusline:
        default:
          foreground: '#808080'
          background: '#000000'
```
Complete configuration looks like this:
``` yaml
color_schemes:
  default:
    indicator_statusline:
        default:
          foreground: '#FFFFFF'
          background: '#0270C0'
        inactive:
          foreground: '#FFFFFF'
          background: '#0270C0'
        normal_mode:
          foreground: '#0f0002'
          background: '#0270C0'
        visual_mode:
          foreground: '#ffffff'
          background: '#0270C0'
```



### `normal`
Normal colors
``` yaml
color_schemes:
  default:
    normal:
      black:   '#000000'
      red:     '#c63939'
      green:   '#00a000'
      yellow:  '#a0a000'
      blue:    '#4d79ff'
      magenta: '#ff66ff'
      cyan:    '#00a0a0'
      white:   '#c0c0c0'
```



### `bright`
Bright colors
``` yaml
color_schemes:
  default:
    normal:
      black:   '#707070'
      red:     '#ff0000'
      green:   '#00ff00'
      yellow:  '#ffff00'
      blue:    '#0000ff'
      magenta: '#ff00ff'
      cyan:    '#00ffff'
      white:   '#ffffff'
```

## Default color scheme


``` yaml
color_schemes:
    default:
        default:
            background: '#1a1716'
            foreground: '#d0d0d0'
        background_image:
            opacity: 0.5
            blur: false
        cursor:
            default: CellForeground
            text: CellBackground
        hyperlink_decoration:
            normal: '#f0f000'
            hover: '#ff0000'
        vi_mode_highlight:
            foreground: CellForeground
            foreground_alpha: 1.0
            background: '#ffa500'
            background_alpha: 0.5
        vi_mode_cursorline:
            foreground: '#ffffff'
            foreground_alpha: 0.2
            background: '#808080'
            background_alpha: 0.4
        selection:
            foreground: CellForeground
            foreground_alpha: 1.0
            background: '#4040f0'
            background_alpha: 0.5
        search_highlight:
            foreground: CellBackground
            background: CellForeground
            foreground_alpha: 1.0
            background_alpha: 1.0
        search_highlight_focused:
            foreground: CellBackground
            background: CellForeground
            foreground_alpha: 1.0
            background_alpha: 1.0
        word_highlight_current:
            foreground: CellForeground
            background: '#909090'
            foreground_alpha: 1.0
            background_alpha: 0.5
        word_highlight_other:
            foreground: CellForeground
            background: '#909090'
            foreground_alpha: 1.0
            background_alpha: 0.5
        indicator_statusline:
            foreground: '#808080'
            background: '#000000'
        indicator_statusline_inactive:
            foreground: '#808080'
            background: '#000000'
        input_method_editor:
            foreground: '#FFFFFF'
            background: '#FF0000'
        normal:
            black:   '#000000'
            red:     '#c63939'
            green:   '#00a000'
            yellow:  '#a0a000'
            blue:    '#4d79ff'
            magenta: '#ff66ff'
            cyan:    '#00a0a0'
            white:   '#c0c0c0'
        bright:
            black:   '#707070'
            red:     '#ff0000'
            green:   '#00ff00'
            yellow:  '#ffff00'
            blue:    '#0000ff'
            magenta: '#ff00ff'
            cyan:    '#00ffff'
            white:   '#ffffff'
```
