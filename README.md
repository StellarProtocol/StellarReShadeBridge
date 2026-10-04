# Stellar ReShade Bridge

A [ReShade](https://reshade.me) add-on that lets the Stellar framework switch ReShade effects on and off, change
presets and effect folders, and draw the active effects into its own texture — so a photo taken in game can carry the
same ReShade look as the screen.

The add-on file is `Stellar.ReShadeBridge.addon64`, version **1.1.0** (`rsb_addon_version`). It is built against the ReShade
**6.8.0** add-on API.

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
  The add-on checks again before **each** request: turning on a technique whose effect was never built makes ReShade
  build it, which counts as loading, so the rest of the batch waits for a later frame. A search-path change also makes
  the rest wait until ReShade has actually started reloading with the new paths.
- **Loading.** ReShade has no public "loading" flag, so the add-on tracks it: loading starts when ReShade initialises
  and whenever ReShade starts reloading effects; it ends when techniques can be listed again. When ReShade starts a
  reload and has **no** effect files at all, that counts as loaded (with no techniques), so search paths can still be
  set on a fresh install. Whether effect files exist is checked against ReShade's own `EffectSearchPaths`; whenever
  that cannot be decided for certain the add-on keeps waiting rather than change ReShade's state during a load.
- **Saved and temporary changes.** A technique change with `save = 1` is written to the current preset
  (`save_current_preset`) on the first frame where ReShade is not loading — that frame or a later one — at most once
  per frame. A change with `save = 0` is a **temporary override**: the add-on remembers it as *(effect file, technique)
  → original state* and never writes it to a preset itself. Before every save the add-on performs, it puts overridden
  techniques back to their original state, saves, and re-applies the overrides, all within the same frame; it does the
  same around a preset switch, because ReShade saves the old preset itself before switching. After a switch the
  originals are re-read from the new preset. A temporary request back to the original state ends the override, a saved
  request on the same technique replaces it, and a change made by anyone else (the ReShade overlay, a hotkey, another
  add-on) ends it too. When ReShade reloads effects and restores the preset's states, the overrides are applied again.
- The read functions return a snapshot. Its frame counter advances every presented frame; its contents are rebuilt
  when something changed (and at least once a second), so the read functions never block on ReShade.
- The add-on never calls back into the host and no C++ exception crosses the C boundary.
- When the request queue is full (4096 requests waiting, for example while ReShade never finishes loading), new
  requests are dropped and the add-on writes one warning to ReShade's log.

### Known limits

- **Temporary overrides can still reach a preset through ReShade itself.** ReShade writes every technique's current
  state when *it* saves: when the user switches presets from the overlay or a preset hotkey, when "auto save" is on and
  the user toggles a technique or changes a value in the overlay or with a hotkey, and when the user saves in the
  overlay. Any temporary override active at that moment is written to the preset.
- **A change made while ReShade is loading can be overwritten.** ReShade does not report technique changes while it is
  loading, so if someone else toggles an overridden technique during a load, the add-on does not notice and puts its
  override back when the load ends.
- **No effects, no preset switch.** While ReShade has no techniques at all, `rsb_request_preset` is ignored (with a
  warning in ReShade's log): switching would make ReShade save the old preset with an empty technique list.
- **Search paths changed outside the add-on.** If `EffectSearchPaths` is changed while the game runs by anything other
  than this add-on (editing `ReShade.ini`, or ReShade's settings page) and the new folders hold no effect files, the
  add-on cannot be sure ReShade has nothing to load and keeps waiting until techniques appear again or the game is
  restarted. The same holds when ReShade's effect files all fail to compile or contain no techniques.

## C ABI

All functions are `extern "C"`, exported from the add-on, and safe to call from any thread unless noted. Strings are
UTF-8. Functions that fill a buffer write a NUL-terminated string truncated to fit and return the full length in bytes.

| Function | Returns | Meaning |
|---|---|---|
| `int rsb_version()` | `1` | ABI version of this add-on. Additions keep it at `1`; detect newer functions with `GetProcAddress`. |
| `int rsb_addon_version(char* buf, int len)` | length | Add-on version, e.g. `1.1.0` (since 1.1.0). |
| `int rsb_ready()` | `0`/`1` | `1` once ReShade has handed the add-on an effect runtime. |
| `int rsb_is_loading()` | `0`/`1` | `1` while ReShade is loading effects (requests wait). `0` once it has finished, including when it has no effects at all. |
| `int rsb_snapshot_frames()` | count | Number of frames the snapshot has been updated for (grows by one per presented frame; stops at `2147483647`). |
| `int rsb_get_enabled()` | `0`/`1` | Whether effects are globally enabled. |
| `void rsb_request_enabled(int on)` | — | Queue: turn all effects on (`1`) or off (`0`). |
| `int rsb_technique_count()` | count | Techniques in the latest snapshot. |
| `int rsb_technique_at(int i, char* name, int nameLen, char* effect, int effectLen, int* enabled)` | `1`/`0` | Fills technique `i`'s name, its effect **file name** exactly as ReShade reports it (e.g. `Clarity.fx`) and its enabled state; `0` if `i` is out of range. |
| `void rsb_request_technique(const char* effect, const char* name, int on, int save)` | — | Queue: enable/disable the technique `name` in the effect file `effect` (as reported by `rsb_technique_at`). A null or empty `effect` matches the technique name in any effect. `save = 1` saves the change to the current preset; `save = 0` is a temporary override that is never saved. |
| `int rsb_get_preset(char* buf, int len)` | length | Path of the current preset. |
| `void rsb_request_preset(const char* path)` | — | Queue: switch to the preset at `path` (ignored by ReShade if it is not a valid preset). Request a preset only when `rsb_technique_count() > 0`; otherwise it is ignored and logged to ReShade's log. |
| `void rsb_request_search_paths(const char* effects, const char* textures)` | — | Queue: set `EffectSearchPaths` / `TextureSearchPaths` in ReShade's config, then reload all effects. Each argument is a `;`-separated list; a null or empty argument leaves that setting unchanged. |
| `void rsb_queue_render(void* d3d11Texture, uint32_t w, uint32_t h)` | — | Queue an `ID3D11Texture2D` (RGBA8, render-target capable) to draw the active effects into. The add-on holds a COM reference to it until the render event uses it or a later call replaces it. `w`/`h` are unused (the whole texture is drawn); they stay for ABI compatibility. Resets `rsb_last_render` to `0`. |
| `int rsb_last_render()` | code | Meaningful only after the render event (`rsb_render_event_func`) for that queued texture has run. Result of the last render: `-1` no ReShade runtime, `-2` nothing queued, `-3` render-target view could not be created, otherwise the number of techniques drawn (`0` = nothing drawn yet, for example while ReShade compiles effects for a new texture size). |
| `void* rsb_render_event_func()` | pointer | Render-event callback (`void (*)(int)`) for Unity's `GL.IssuePluginEvent`. It runs on the render thread and draws the queued texture. It assumes that thread also presents the swap chain, so ReShade cannot destroy its effect runtime while the callback runs. |

Notes:

- ReShade draws effects once per frame. A render into the queued texture uses that frame's effect pass, so the screen
  shows one frame without effects.
- The first render at a new texture size can draw nothing while ReShade prepares the effects for that size; repeat on
  later frames until `rsb_last_render()` is above `0`.
- ReShade leaves the alpha channel at `0` after drawing effects; set it yourself if you need an opaque image.
- The add-on does not report which techniques use the depth buffer. A host that needs this derives it from the effect
  source file named by `rsb_technique_at`.

## Isolated capture (since 1.1.0)

`rsb_queue_render` draws with the game's own effect runtime. ReShade compiles a separate permutation per target size, but
an effect's named textures exist once per runtime: a texture the screen-size permutation created first is reused at the
screen size when a larger target is drawn. Multi-pass effects then read only the top-left part of a large photo and
magnify it. The isolated capture avoids this by drawing the photo in a **separate effect runtime** whose back buffer has
the photo's size, so every effect texture is created at that size.

How it is built:

- The runtime is created with ReShade's `create_effect_runtime` on the game's Direct3D 11 device and immediate context.
  Its "swap chain" is an object the add-on implements around one texture of the photo's size; it is never presented and
  has no window. (A real swap chain cannot be used: ReShade hooks swap-chain creation and would attach a second automatic
  runtime with the game's settings.)
- **Effects.** The add-on writes the runtime's config file at the path the host passes, plus a copy of the current preset
  next to it (`<config name>.preset.ini`). The config copies `EffectSearchPaths`, `TextureSearchPaths`,
  `PreprocessorDefinitions`, `PerformanceMode`, `IntermediateCachePath`, `NoEffectCache` and `NoDebugInfo` from the game
  runtime's config, sets `SkipLoadingDisabledEffects=1`, `NoReloadOnInit=0` and `PresetPath` to the copy, and turns off
  every shortcut key, preset shortcut and gamepad navigation. The copy's technique list is the game runtime's **live**
  technique states at that moment (temporary overrides and unsaved toggles included), in the game's order, with the
  isolated technique requests (below) applied on top. Only effects with an enabled technique are compiled. Uniform values
  and per-effect preprocessor definitions come from the preset **file**: values changed in the overlay and not saved yet
  are not in the photo. The user's preset is never written; the two files stay on disk after the session.
- **Depth.** The isolated runtime has no depth buffer. ReShade's built-in depth add-on does not attach one to it (it keeps
  its data on the game's device wrapper, which the new runtime does not share), so depth-based effects see an empty
  depth texture. That is not an error.
- **Effect Runtime Sync.** When ReShade's built-in "Effect Runtime Sync" add-on would synchronise runtimes (its
  `[ADDON] SyncEffectRuntimes` setting, or a VR runtime loaded while that setting is absent), it would copy the isolated
  runtime's preset and technique changes onto the game's runtime. The add-on then refuses to begin (`-4`).
- Nothing changes on screen: the game's runtime is not used for the photo.

### Functions

| Function | Returns | Meaning |
|---|---|---|
| `int rsb_isolated_begin(uint32_t width, uint32_t height, const char* configPath)` | `1` / code | Queue the creation of an isolated runtime for `width` x `height` (at most 16384 each; not smaller than 160 x 120). `configPath` is an absolute UTF-8 path of a `.ini` file the add-on may overwrite; the preset copy is written next to it. Returns `1` when queued, `-2` for bad arguments, `-10` while a session is starting or active. |
| `int rsb_isolated_state()` | state | `0` idle, `1` starting, `2` loading, `3` ready, `4` nothing to draw (effects are off globally, or no technique would be enabled; no runtime was created), `5` ending; errors: `-1` no game runtime or not Direct3D 11, `-2` bad argument (also: relative config path, or it would overwrite the current preset), `-3` larger than the device allows, `-4` Effect Runtime Sync active, `-5` texture or view creation failed (for example out of video memory), `-6` config or preset copy could not be written, `-7` ReShade refused to create the runtime, `-8` the game's runtime or device went away (window resize, shutdown), `-9` internal error. An error state stays until the next begin or end. |
| `void rsb_isolated_request_technique(const char* effect, const char* name, int on)` | — | Enable or disable a technique in the isolated runtime only (for example depth effects off for a shaped photo). Same matching as `rsb_request_technique`. Requests made before `rsb_isolated_begin` decide which effects are compiled; later ones are applied on the next event (enabling a technique whose effect was not compiled makes the runtime load again). Requests are cleared by `rsb_isolated_end`. The game's runtime is not touched. |
| `void rsb_isolated_queue_render(void* d3d11Texture)` | — | Queue an `ID3D11Texture2D` to draw into: exactly `width` x `height`, one sample, an RGBA8 format (`R8G8B8A8_*`), default usage. The add-on holds a COM reference until the event uses it. Resets `rsb_isolated_last_render` to `0`. |
| `int rsb_isolated_last_render()` | code | After the event that used the texture: number of techniques drawn (`0` = nothing drawn), or `-1` no isolated runtime, `-2` nothing queued, `-4` not ready (loading; queue again later), `-5` texture does not match, `-9` internal error. |
| `void rsb_isolated_end()` | — | Queue the destruction of the isolated runtime, its texture and its swap chain. State becomes `5`; the next event frees the video memory and sets `0`. |
| `void* rsb_isolated_event_func()` | pointer | Render-event callback (`void (*)(int)`, the id is ignored) for `GL.IssuePluginEvent`. All device work of the isolated capture happens inside it, on the render thread. |

### Per-frame protocol

1. Optionally `rsb_isolated_request_technique(...)` for photo-only technique changes.
2. `rsb_isolated_begin(w, h, path)`.
3. Every frame, issue `rsb_isolated_event_func()` once and read `rsb_isolated_state()` afterwards. Each event presents the
   isolated runtime at most once. While the state is `1` the add-on waits for the game's runtime to finish loading; while
   it is `2` ReShade compiles effects on its worker threads and creates one effect per event. Compiling at a new size can
   take seconds (effects are cached per size in `IntermediateCachePath`, so later sessions at the same size are faster):
   use a generous timeout, then end and fall back.
4. When the state is `3`: render the photo into the texture, `rsb_isolated_queue_render(texture)`, and issue the event
   once in the same frame. After the readback, `rsb_isolated_last_render()` gives the result. `-4` means the runtime
   started loading again; repeat on a later frame.
5. More renders in the same session are allowed (one per event). The add-on presents the isolated runtime before each
   render after the first, because ReShade draws a runtime's effects at most once per present.
6. `rsb_isolated_end()`, then issue the event once more to free everything. States `4` and errors need `rsb_isolated_end`
   (or a new `rsb_isolated_begin`) too.

Notes:

- The isolated runtime is also destroyed when the game's runtime is destroyed (for example on a window resize) or its
  device is; the state then becomes `-8`.
- ReShade leaves the alpha channel at `0`, as with `rsb_queue_render`.
- Effects that accumulate over frames (eye adaptation, temporal filters) start from a fresh state in the isolated
  runtime, and frame-time uniforms see the time between the runtime's presents.
- ReShade does not offer extra add-ons a way to tell this runtime apart. A third-party add-on that assumes every runtime's
  device carries its own data (created when the game's device was created) may fail on the isolated runtime.

## Building

Windows with the MSVC toolchain and the ReShade 6.8.0 headers:

```bat
git clone --depth 1 --branch v6.8.0 https://github.com/crosire/reshade.git rs
cl /nologo /LD /std:c++17 /EHsc /O2 /DNDEBUG /I rs\include src\bridge.cpp /Fe:Stellar.ReShadeBridge.addon64 /link user32.lib
```

The `build` workflow does the same on every push and uploads the add-on as an artifact.

## Licence

MIT — see [LICENSE](LICENSE).
