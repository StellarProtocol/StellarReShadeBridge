# Stellar ReShade Bridge

A [ReShade](https://reshade.me) add-on that lets the Stellar framework switch ReShade effects on and off, change
presets and effect folders, and draw the active effects into its own texture — so a photo taken in game can carry the
same ReShade look as the screen.

The add-on file is `Stellar.ReShadeBridge.addon64`. It is built against the ReShade **6.8.0** add-on API.

## Requirements

- **ReShade with full add-on support** (the "ReShade with add-on support" download). The normal ReShade build does not
  load add-ons. ReShade's add-on build is meant for single-player games or games without anti-cheat — use it only where
  that is the case.
- A 64-bit Direct3D 11 game.

## Install

Copy `Stellar.ReShadeBridge.addon64` next to the game's executable (or into the folder set as `AddonPath` in
`ReShade.ini`). ReShade lists it on its **Add-ons** tab as "Stellar ReShade bridge".

## How it works

- The add-on keeps the `effect_runtime` ReShade gives it.
- Every change to ReShade's state is **queued** by the C functions below and **applied on the render thread** inside
  ReShade's own present (`reshade_present` event), in request order. Requests wait while ReShade is still loading
  effects (it reports no techniques then) and are applied once loading has finished.
- Technique changes made with `save = 1` are written to the current preset (`save_current_preset`, at most once per
  frame). Changes made with `save = 0` are temporary: the add-on does not save them, but ReShade itself may still write
  them if the preset is saved for another reason (for example by a later saved change or a preset switch), and it drops
  them if it reloads the preset.
- The read functions return a snapshot that is refreshed every frame, so they never block on ReShade.
- The add-on never calls back into the host and no C++ exception crosses the C boundary.

## C ABI

All functions are `extern "C"`, exported from the add-on, and safe to call from any thread unless noted. Strings are
UTF-8. Functions that fill a buffer write a NUL-terminated string truncated to fit and return the full length in bytes.

| Function | Returns | Meaning |
|---|---|---|
| `int rsb_version()` | `1` | ABI version of this add-on. |
| `int rsb_ready()` | `0`/`1` | `1` once ReShade has handed the add-on an effect runtime. |
| `int rsb_is_loading()` | `0`/`1` | `1` while the latest snapshot lists no techniques (ReShade loading, or nothing loaded yet). |
| `int rsb_snapshot_frames()` | count | Number of snapshots taken so far (grows by one per presented frame). |
| `int rsb_get_enabled()` | `0`/`1` | Whether effects are globally enabled. |
| `void rsb_request_enabled(int on)` | — | Queue: turn all effects on (`1`) or off (`0`). |
| `int rsb_technique_count()` | count | Techniques in the latest snapshot. |
| `int rsb_technique_at(int i, char* name, int nameLen, char* effect, int effectLen, int* enabled)` | `1`/`0` | Fills technique `i`'s name, its effect file name (e.g. `Clarity.fx`) and enabled state; `0` if `i` is out of range. |
| `void rsb_request_technique(const char* name, int on, int save)` | — | Queue: enable/disable every technique with this name. `save = 1` saves the change to the current preset; `save = 0` is temporary. |
| `int rsb_get_preset(char* buf, int len)` | length | Path of the current preset. |
| `void rsb_request_preset(const char* path)` | — | Queue: switch to the preset at `path` (ignored by ReShade if it is not a valid preset). |
| `void rsb_request_search_paths(const char* effects, const char* textures)` | — | Queue: set `EffectSearchPaths` / `TextureSearchPaths` in ReShade's config, then reload all effects. Each argument is a `;`-separated list; a null or empty argument leaves that setting unchanged. |
| `void rsb_queue_render(void* d3d11Texture, uint32_t w, uint32_t h)` | — | Queue an `ID3D11Texture2D` (RGBA8, render-target capable) to draw the active effects into. Resets `rsb_last_render` to `0`. |
| `int rsb_last_render()` | code | Result of the last render: `-1` no ReShade runtime, `-2` nothing queued, `-3` render-target view could not be created, otherwise the number of techniques drawn (`0` = nothing drawn yet, for example while ReShade compiles effects for a new texture size). |
| `void* rsb_render_event_func()` | pointer | Render-event callback (`void (*)(int)`) for Unity's `GL.IssuePluginEvent`. It runs on the render thread and draws the queued texture. |

Notes:

- ReShade draws effects once per frame. A render into the queued texture uses that frame's effect pass, so the screen
  shows one frame without effects.
- The first render at a new texture size can draw nothing while ReShade prepares the effects for that size; repeat on
  later frames until `rsb_last_render()` is above `0`.
- ReShade leaves the alpha channel at `0` after drawing effects; set it yourself if you need an opaque image.

## Building

Windows with the MSVC toolchain and the ReShade 6.8.0 headers:

```bat
git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git rs
cl /nologo /LD /std:c++17 /EHsc /O2 /DNDEBUG /I rs\include src\bridge.cpp /Fe:Stellar.ReShadeBridge.addon64 /link user32.lib
```

The `build` workflow does the same on every push and uploads the add-on as an artifact.

## Licence

MIT — see [LICENSE](LICENSE).
