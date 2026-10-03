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
  ReShade's own present (`reshade_present` event), in request order, and only while ReShade is not loading effects.
  A preset switch or a search-path change ends that frame's batch; the remaining requests follow on later frames.
- **Loading.** ReShade has no public "loading" flag, so the add-on tracks it: loading starts when ReShade initialises
  and whenever the add-on asks it to reload effects; `reshade_reloaded_effects` and a listable technique list end it.
  When ReShade has finished and simply has **no** effect files, that counts as loaded (with no techniques), so search
  paths and presets can still be set on a fresh install. Whether effect files exist is checked against ReShade's own
  `EffectSearchPaths`; when that cannot be decided for certain (for example paths with environment macros) the add-on
  keeps waiting rather than change ReShade's state during a load.
- **Saved and temporary changes.** A technique change with `save = 1` is written to the current preset
  (`save_current_preset`, at most once per frame). A change with `save = 0` is a **temporary override**: the add-on
  remembers it as *(effect file, technique) → original state* and never lets it reach a preset file. Before every save
  the add-on performs, it puts overridden techniques back to their original state, saves, and re-applies the overrides,
  all within the same frame; it does the same around a preset switch, because ReShade saves the old preset itself before
  switching. After a switch the originals are re-read from the new preset. A temporary request back to the original
  state ends the override, a saved request on the same technique replaces it, and a change made by anyone else (the
  ReShade overlay, a hotkey, another add-on) ends it too. When ReShade reloads effects and restores the preset's states,
  the overrides are applied again.
  Limit: when the **user** switches presets from the ReShade overlay, ReShade saves the old preset before the add-on
  can step in, so active temporary overrides can be written to that preset.
- The read functions return a snapshot that is refreshed every frame, so they never block on ReShade.
- The add-on never calls back into the host and no C++ exception crosses the C boundary.

## C ABI

All functions are `extern "C"`, exported from the add-on, and safe to call from any thread unless noted. Strings are
UTF-8. Functions that fill a buffer write a NUL-terminated string truncated to fit and return the full length in bytes.

| Function | Returns | Meaning |
|---|---|---|
| `int rsb_version()` | `1` | ABI version of this add-on. |
| `int rsb_ready()` | `0`/`1` | `1` once ReShade has handed the add-on an effect runtime. |
| `int rsb_is_loading()` | `0`/`1` | `1` while ReShade is loading effects (requests wait). `0` once it has finished, including when it has no effects at all. |
| `int rsb_snapshot_frames()` | count | Number of snapshots taken so far (grows by one per presented frame). |
| `int rsb_get_enabled()` | `0`/`1` | Whether effects are globally enabled. |
| `void rsb_request_enabled(int on)` | — | Queue: turn all effects on (`1`) or off (`0`). |
| `int rsb_technique_count()` | count | Techniques in the latest snapshot. |
| `int rsb_technique_at(int i, char* name, int nameLen, char* effect, int effectLen, int* enabled)` | `1`/`0` | Fills technique `i`'s name, its effect **file name** exactly as ReShade reports it (e.g. `Clarity.fx`) and its enabled state; `0` if `i` is out of range. |
| `void rsb_request_technique(const char* effect, const char* name, int on, int save)` | — | Queue: enable/disable the technique `name` in the effect file `effect` (as reported by `rsb_technique_at`). A null or empty `effect` matches the technique name in any effect. `save = 1` saves the change to the current preset; `save = 0` is a temporary override that is never saved. |
| `int rsb_get_preset(char* buf, int len)` | length | Path of the current preset. |
| `void rsb_request_preset(const char* path)` | — | Queue: switch to the preset at `path` (ignored by ReShade if it is not a valid preset). |
| `void rsb_request_search_paths(const char* effects, const char* textures)` | — | Queue: set `EffectSearchPaths` / `TextureSearchPaths` in ReShade's config, then reload all effects. Each argument is a `;`-separated list; a null or empty argument leaves that setting unchanged. |
| `void rsb_queue_render(void* d3d11Texture, uint32_t w, uint32_t h)` | — | Queue an `ID3D11Texture2D` (RGBA8, render-target capable) to draw the active effects into. Resets `rsb_last_render` to `0`. |
| `int rsb_last_render()` | code | Meaningful only after the render event (`rsb_render_event_func`) for that queued texture has run. Result of the last render: `-1` no ReShade runtime, `-2` nothing queued, `-3` render-target view could not be created, otherwise the number of techniques drawn (`0` = nothing drawn yet, for example while ReShade compiles effects for a new texture size). |
| `void* rsb_render_event_func()` | pointer | Render-event callback (`void (*)(int)`) for Unity's `GL.IssuePluginEvent`. It runs on the render thread and draws the queued texture. |

Notes:

- ReShade draws effects once per frame. A render into the queued texture uses that frame's effect pass, so the screen
  shows one frame without effects.
- The first render at a new texture size can draw nothing while ReShade prepares the effects for that size; repeat on
  later frames until `rsb_last_render()` is above `0`.
- ReShade leaves the alpha channel at `0` after drawing effects; set it yourself if you need an opaque image.
- The add-on does not report which techniques use the depth buffer. A host that needs this derives it from the effect
  source file named by `rsb_technique_at`.

## Building

Windows with the MSVC toolchain and the ReShade 6.8.0 headers:

```bat
git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git rs
cl /nologo /LD /std:c++17 /EHsc /O2 /DNDEBUG /I rs\include src\bridge.cpp /Fe:Stellar.ReShadeBridge.addon64 /link user32.lib
```

The `build` workflow does the same on every push and uploads the add-on as an artifact.

## Licence

MIT — see [LICENSE](LICENSE).
