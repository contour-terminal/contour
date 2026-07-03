# Advanced Configuration: Renderer

### `renderer.backend`

Selects which Qt RHI graphics API drives the terminal display. Supported values:

| Value | Meaning |
|-------|---------|
| `auto` | **Default.** Let Qt pick the platform-native backend: Direct3D 11 on Windows, Metal on macOS, OpenGL on Linux. |
| `OpenGL` | Force the OpenGL backend on every platform. |
| `vulkan` | Force the Vulkan backend (Windows and Linux). |
| `direct3d11` | Force the Direct3D 11 backend (Windows only). |
| `direct3d12` | Force the Direct3D 12 backend (Windows only). |
| `metal` | Force the Metal backend (macOS only). |
| `software` | Force a software-emulated OpenGL rasterizer. |

A backend that the running platform cannot provide (e.g. `metal` on Windows) falls back to `auto`
with a warning. `OpenGL` remains the safe fallback if a native backend misbehaves on your hardware.

```yml
renderer:
    backend: auto
```

### `renderer.tile_hashtable_slots`

Defines the number of hashtable slots to map to the texture tiles.
Larger values may increase performance, but too large may also decrease.
This value is rounted up to a value equal to the power of two.

Default: `4096`

```yml
renderer:
    tile_hashtable_slots: 4096
```

### `renderer.tile_cache_count`

Defines the number of tiles that must fit at lest into the texture atlas.

This does not include direct mapped tiles (US-ASCII glyphs,
cursor shapes and decorations), if `tile_direct_mapping` is set to true).

Value must be at least as large as grid cells available in the terminal view.
This value is automatically adjusted if too small.

Default: `4000`

```yml
renderer:
    tile_cache_count: 4000
```

### `renderer.tile_direct_mapping`

Enables/disables the use of direct-mapped texture atlas tiles for
the most often used ones (US-ASCII, cursor shapes, underline styles)

You most likely do not want to touch this and leave it enabled.

Default: true

```yml
renderer:
    tile_direct_mapping: true
```
