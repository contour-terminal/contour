# Bundled fonts

JetBrains Mono, the default terminal font on macOS and Linux (Windows uses Cascadia Mono and ships no font).

- Version: v2.304 (variable fonts)
- Source: <https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304>
- Archive: <https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip>
- Licence: SIL Open Font License 1.1, see `OFL.txt`

The two variable fonts are taken unmodified from `fonts/variable/` of the archive. `OFL.txt` is the
archive's, with one trailing space removed (line 21) for the repository's editorconfig check; its
text is unchanged. The table gives the archive's hash of it.

| File | sha256 |
| --- | --- |
| `JetBrainsMono-2.304.zip` | `6f6376c6ed2960ea8a963cd7387ec9d76e3f629125bc33d1fdcd7eb7012f7bbf` |
| `JetBrainsMono[wght].ttf` | `662a196d58f1183bf2d77428b6d5283fe3f45161ab021bea4036bc98e5cac016` |
| `JetBrainsMono-Italic[wght].ttf` | `f115aaa12113718c02ce72864fe6823b87241bc23d3e44cf1220155f861063f2` |
| `OFL.txt` | `30f0c136e3c88e422d0791acd97238870f9054a9729bc34cf2ff0d4ed8cac4ad` |

At startup the application registers every `*.ttf` in this directory (installed at
`Contents/Resources/fonts` in the macOS bundle, `<prefix>/share/contour/fonts` on Linux) with the
platform font registry, before the first font lookup.
