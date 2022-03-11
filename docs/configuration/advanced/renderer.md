# Advanced Configuration: Renderer

### `renderer.backend`

Currently two rendering backends are supported. `OpenGL`, the default,
and `software`, which will force a fall back to a software-emulated OpenGL
driver. Specifying `default` will automatically pick the default.

```yml
renderer:
    backend: OpenGL
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

You most likely do not wnat to touch this and leave it enabled.

Default: true

```yml
renderer:
    tile_direct_mapping: true
```
