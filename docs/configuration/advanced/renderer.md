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
`default` is accepted as a legacy alias of `auto`.

```yml
renderer:
    backend: auto
```

### `renderer.gpu`

Which GPU renders the terminal. Takes effect at the next start. Also available in the settings page
as "Rendering GPU".

| Value | Meaning |
|-------|---------|
| `auto` | **Default.** Use the system's default GPU (Contour does not intervene). On most laptops this is the power-saving integrated GPU. |
| `integrated` | The integrated GPU, if present. |
| `discrete` | The discrete GPU, if present; otherwise the integrated one. |
| `vvvv:dddd` | A specific GPU by PCI vendor:device id, e.g. `10de:2820`. If it is absent, `auto` is used. |

Vulkan and Direct3D honour this on Linux and Windows (Qt 6.10 or newer). OpenGL honours it on Linux
only, through the driver's own variables (`DRI_PRIME`, or NVIDIA's PRIME offload variables); programs
started inside the terminal do not inherit them. If the chosen GPU cannot render, Contour warns and uses
`auto` for that session.

```yml
renderer:
    gpu: auto
```

### `renderer.tile_hashtable_slots`

Defines the number of hashtable slots to map to the texture tiles.
Larger values may increase performance, but too large may also decrease.
This value is rounded up to a value equal to the power of two.

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
