// Stellar ReShade bridge — a ReShade add-on (ReShade 6.8.0 add-on API) that lets a host application switch ReShade
// effects and draw them into its own texture, through a small C ABI.
//
// Threading model:
//   * Every ReShade call that CHANGES state (technique toggles, effects on/off, preset, search paths) runs on the render
//     thread inside ReShade's own present path (the reshade_present event, source/runtime.cpp:936, after update_effects
//     at :730). The exported rsb_request_* functions only enqueue requests.
//   * Requests are applied only while ReShade is not loading, in request order, re-checking before each one
//     (requests.hpp). Saves are deferred until ReShade lists techniques and is not loading, at most one per frame, with
//     temporary overrides reverted around them (overrides.hpp).
//   * The exported getters read a snapshot. reshade_present advances its frame counter every frame and rebuilds its
//     contents only when something changed (an event, an applied request, a change in technique count, loading state or
//     effects state) and at least every snapshot_refresh_frames frames.
//   * The Unity render-event callback (rsb_render_event_func) runs on the host's render thread and is the only other
//     place that touches the runtime. It assumes that thread is also the one presenting the swap chain, so ReShade's
//     destroy_effect_runtime (source/runtime.cpp:687, raised from the swap chain's reset/destroy) cannot run during it.
//   * Locks are held only to swap or copy small data. The add-on never calls back into the host, and no C++ exception
//     crosses the ABI or a ReShade callback.
//
// Loading state: see loading_state.hpp and on_reloaded_effects below.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <reshade.hpp>
#include "bridge_state.hpp"
#include "effect_files.hpp"
#include "loading_state.hpp"
#include "overrides.hpp"
#include "requests.hpp"

using namespace reshade::api;
using namespace stellar_rsb;

extern "C" __declspec(dllexport) const char *NAME = "Stellar ReShade bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Lets the Stellar framework switch ReShade effects and draw them into photos.";

static constexpr int bridge_abi_version = 1;

static constexpr int snapshot_refresh_frames = 60; // safety net for changes no event reports

static std::atomic<effect_runtime *> g_runtime{ nullptr };
static SnapshotStore g_snapshot;
static std::atomic<bool> g_reset_render_state{ false }; // set at init; the render thread drops overrides and saves
static std::atomic<DWORD> g_render_thread{ 0 };
static bool g_was_loading = true; // render thread only

// ---- offscreen render (host render thread) ----
static std::mutex g_render_lock;
static void *g_pending_texture = nullptr; // holds one COM reference (AddRef in rsb_queue_render)
static std::atomic<int> g_last_render{ 0 };
static std::atomic<bool> g_counting{ false };
static std::atomic<int> g_drawn{ 0 };

// ---------------------------------------------------------------------------------------------------------------------
// Snapshot.

struct SnapshotFill { Snapshot *snap; size_t index; };

static void fill_snapshot(effect_runtime *r, Snapshot &snap)
{
    SnapshotFill fill{ &snap, 0 };
    r->enumerate_techniques(nullptr, [](effect_runtime *rt, effect_technique t, void *u) {
        try
        {
            auto *f = static_cast<SnapshotFill *>(u);
            if (f->snap->techniques.size() <= f->index)
                f->snap->techniques.emplace_back();
            TechniqueState &s = f->snap->techniques[f->index];
            read_reshade_string(s.name, [&](char *b, size_t *n) { rt->get_technique_name(t, b, n); });
            // The effect FILE name exactly as ReShade reports it (e.g. "Clarity.fx"); hosts use it to find the source.
            read_reshade_string(s.effect, [&](char *b, size_t *n) { rt->get_technique_effect_name(t, b, n); });
            s.enabled = rt->get_technique_state(t);
            f->index++;
        }
        catch (...) {} // never unwind through ReShade's enumeration loop
    }, &fill);
    snap.technique_count = fill.index;
    snap.loading = is_loading_now(fill.index);
    snap.effects_enabled = r->get_effects_state();
    read_reshade_string(snap.preset_path, [&](char *b, size_t *n) { r->get_current_preset_path(b, n); });
}

// Render thread: publishes a fresh snapshot when something changed, otherwise only advances the frame counter.
static void publish_snapshot(effect_runtime *r, size_t count)
{
    static Snapshot s_work;
    static size_t s_count = static_cast<size_t>(-1);
    static bool s_loading = true, s_enabled = false;
    static int s_frames_since_rebuild = 0;

    const bool loading = is_loading_now(count);
    const bool enabled = r->get_effects_state();
    if (g_snapshot_dirty.exchange(false) || count != s_count || loading != s_loading || enabled != s_enabled ||
        ++s_frames_since_rebuild >= snapshot_refresh_frames)
    {
        fill_snapshot(r, s_work);
        s_count = s_work.technique_count;
        s_loading = s_work.loading;
        s_enabled = s_work.effects_enabled;
        s_frames_since_rebuild = 0;
        g_snapshot.publish(s_work);
    }
    else
    {
        g_snapshot.tick();
    }
    g_was_loading = loading;
}

// ---------------------------------------------------------------------------------------------------------------------
// ReShade events.

static void on_init(effect_runtime *r)
{
    try
    {
        g_loading = true; // the first present after init reloads everything (source/runtime.cpp:567, 3663-3664)
        g_known_empty = false;
        g_reload_requested = false; // a reset dropped ReShade's pending reload list (source/runtime.cpp:3507)
        g_reset_render_state = true;
        g_snapshot_dirty = true;
        record_search_paths_at_init(r);
        g_runtime = r;
    }
    catch (...) {}
}

static void on_destroy(effect_runtime *r)
{
    try
    {
        effect_runtime *expected = r;
        if (g_runtime.compare_exchange_strong(expected, nullptr))
        {
            g_loading = true;
            g_known_empty = false;
            g_snapshot_dirty = true;
            g_snapshot.reset();
        }
    }
    catch (...) {}
}

// ReShade fires this in three places (all on the render thread, inside update_effects or a reload it calls):
//   * a full reload STARTS: old effects destroyed, nothing loaded yet (source/runtime.cpp:3476-3480);
//   * a single-effect reload STARTS (source/runtime.cpp:3461-3465);
//   * the effect-creation queue just became empty: a reload or a technique enable has finished (source/runtime.cpp:3797-3800).
// At a start the technique list is empty; at the end it is not (the created effect has at least the technique whose
// enable queued it). An empty list therefore identifies a start.
static void on_reloaded_effects(effect_runtime *r)
{
    try
    {
        if (r != g_runtime.load())
            return;
        g_snapshot_dirty = true;
        if (count_techniques(r) != 0)
        {
            g_loading = false; // creation finished and techniques are listed
            g_known_empty = false;
            return;
        }
        // A reload start. While the bridge's own full reload is pending, this is that reload or happens in the same
        // update_effects pass right before it: reload_effect_next_frame(nullptr) REPLACES the pending list and blocks
        // later single-effect requests (source/runtime_api.cpp:1449-1460); entries added afterwards by render_effects
        // (source/runtime.cpp:4040-4041) are processed in the same sorted loop, which ends with the full reload
        // (source/runtime.cpp:3671-3687); and every full reload clears the list (source/runtime.cpp:3507). No present
        // runs in between, and the full reload's files are looked up right after its start event
        // (source/runtime.cpp:3480, 3490 -> 3411), with the config the bridge already reloaded.
        g_reload_requested = false;
        if (!may_have_effect_files(r))
        {
            g_loading = false; // nothing to load: ReShade is done, with no techniques
            g_known_empty = true;
        }
        else
        {
            g_loading = true; // a load is starting
            g_known_empty = false;
        }
    }
    catch (...)
    {
        g_loading = true;
        g_known_empty = false;
    }
}

// A technique changed by anyone but the bridge (overlay, hotkey, another add-on) ends any temporary override on it.
// ReShade raises this only while not loading (source/runtime.cpp:3337, 3359).
static bool on_set_technique_state(effect_runtime *r, effect_technique t, bool)
{
    try
    {
        g_snapshot_dirty = true;
        if (g_applying.load() || r != g_runtime.load() || GetCurrentThreadId() != g_render_thread.load() || g_overrides.empty())
            return false;
        TechniqueKey key;
        read_key(r, t, key);
        g_overrides.forget(key);
    }
    catch (...) {}
    return false; // never block the change
}

static bool on_set_effects_state(effect_runtime *, bool) { g_snapshot_dirty = true; return false; }
static void on_set_preset_path(effect_runtime *, const char *) { g_snapshot_dirty = true; }
static bool on_reorder_techniques(effect_runtime *, size_t, effect_technique *) { g_snapshot_dirty = true; return false; }

// Render thread, inside ReShade's present (source/runtime.cpp:936, right before _effects_rendered_this_frame resets).
static void on_reshade_present(effect_runtime *r)
{
    try
    {
        if (r != g_runtime.load())
            return;
        g_render_thread = GetCurrentThreadId();
        if (g_reset_render_state.exchange(false))
        {
            g_overrides.clear();
            g_save_pending = false;
        }

        size_t count = count_techniques(r);
        if (count != 0)
        {
            g_loading = false; // techniques can be listed only when ReShade is not loading
            g_known_empty = false;
        }

        if (!is_loading_now(count))
        {
            static std::vector<Request> s_requests; // render-thread buffer, reused to avoid per-frame allocation
            if (g_was_loading)
                g_overrides.reapply(r); // a reload restored the preset's states; temporary overrides come back
            bool saved = try_pending_save(r); // a save carried over from an earlier frame
            s_requests.clear();
            g_requests.take_all(s_requests);
            if (!s_requests.empty())
                saved = apply_requests(r, s_requests, saved);
            if (!saved)
                try_pending_save(r); // at most one bridge save per frame
            s_requests.clear();
            if (g_was_loading || g_snapshot_dirty.load())
                count = count_techniques(r); // requests or the reapply may have started a load
        }
        // While loading, requests stay queued (in order) and a pending save waits for a later frame.

        publish_snapshot(r, count);
    }
    catch (...)
    {
        // Never let an exception escape into ReShade.
    }
}

static void on_render_technique(effect_runtime *, effect_technique, command_list *, resource_view, resource_view)
{
    if (g_counting.load())
        g_drawn++;
}

static void release_texture(void *texture) noexcept
{
    if (texture != nullptr)
        static_cast<IUnknown *>(texture)->Release();
}

// Owns one COM reference; releases it when it goes out of scope.
struct TextureRef
{
    void *texture = nullptr;
    ~TextureRef() { release_texture(texture); }
};

// Host render-thread callback (Unity GL.IssuePluginEvent). x64: one calling convention. Consumes the queued texture
// and releases the reference rsb_queue_render took.
static void render_event(int)
{
    TextureRef queued;
    try
    {
        {
            std::lock_guard<std::mutex> lock(g_render_lock);
            queued.texture = g_pending_texture;
            g_pending_texture = nullptr;
        }
        effect_runtime *r = g_runtime.load();
        if (!r) { g_last_render = -1; return; }
        if (!queued.texture) { g_last_render = -2; return; }

        device *d = r->get_device();
        const resource res{ reinterpret_cast<uint64_t>(queued.texture) };
        resource_view rtv{}, rtv_srgb{};
        if (!d->create_resource_view(res, resource_usage::render_target, resource_view_desc(format::r8g8b8a8_unorm, 0, 1, 0, 1), &rtv))
        {
            g_last_render = -3;
            return;
        }
        if (!d->create_resource_view(res, resource_usage::render_target, resource_view_desc(format::r8g8b8a8_unorm_srgb, 0, 1, 0, 1), &rtv_srgb))
            rtv_srgb = rtv;

        g_drawn = 0;
        g_counting = true;
        r->render_effects(r->get_command_queue()->get_immediate_command_list(), rtv, rtv_srgb);
        g_counting = false;

        d->destroy_resource_view(rtv);
        if (rtv_srgb.handle != rtv.handle)
            d->destroy_resource_view(rtv_srgb);
        g_last_render = g_drawn.load();
    }
    catch (...)
    {
        g_counting = false;
        g_last_render = -3;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// C ABI. Every export catches everything; nothing throws across the boundary.

extern "C" __declspec(dllexport) int rsb_version() { return bridge_abi_version; }

extern "C" __declspec(dllexport) int rsb_ready() { return g_runtime.load() != nullptr ? 1 : 0; }

extern "C" __declspec(dllexport) int rsb_is_loading()
{
    try { return g_snapshot.loading(); } catch (...) { return 1; }
}

extern "C" __declspec(dllexport) int rsb_snapshot_frames()
{
    try { return g_snapshot.frames(); } catch (...) { return 0; }
}

extern "C" __declspec(dllexport) int rsb_get_enabled()
{
    try { return g_snapshot.effects_enabled(); } catch (...) { return 0; }
}

extern "C" __declspec(dllexport) void rsb_request_enabled(int on)
{
    try
    {
        Request q;
        q.kind = RequestKind::effects_enabled;
        q.on = on != 0;
        report_push(g_requests.push(std::move(q)));
    }
    catch (...) {}
}

extern "C" __declspec(dllexport) int rsb_technique_count()
{
    try { return g_snapshot.technique_count(); } catch (...) { return 0; }
}

// Returns 1 and fills the buffers (NUL-terminated, truncated to fit), or 0 if `i` is out of range.
// `effect` receives the effect FILE name exactly as ReShade reports it (e.g. "Clarity.fx").
extern "C" __declspec(dllexport) int rsb_technique_at(int i, char *name, int nameLen, char *effect, int effectLen, int *enabled)
{
    try { return g_snapshot.technique_at(i, name, nameLen, effect, effectLen, enabled); } catch (...) { return 0; }
}

// effect = effect file name, or null/"" for any effect. save = 0: temporary (never written to the preset);
// save = 1: saved to the current preset.
extern "C" __declspec(dllexport) void rsb_request_technique(const char *effect, const char *name, int on, int save)
{
    try
    {
        if (!name || !*name)
            return;
        Request q;
        q.kind = RequestKind::technique;
        q.effect = effect ? effect : "";
        q.text = name;
        q.on = on != 0;
        q.save = save != 0;
        report_push(g_requests.push(std::move(q)));
    }
    catch (...) {}
}

// Copies the current preset path (UTF-8, NUL-terminated, truncated to fit). Returns its full length in bytes.
extern "C" __declspec(dllexport) int rsb_get_preset(char *buf, int len)
{
    try { return g_snapshot.preset(buf, len); } catch (...) { return 0; }
}

extern "C" __declspec(dllexport) void rsb_request_preset(const char *path)
{
    try
    {
        if (!path || !*path)
            return;
        Request q;
        q.kind = RequestKind::preset;
        q.text = path;
        report_push(g_requests.push(std::move(q)));
    }
    catch (...) {}
}

// Each argument is a ';'-separated list of paths; a null or empty argument leaves that setting unchanged.
extern "C" __declspec(dllexport) void rsb_request_search_paths(const char *effects, const char *textures)
{
    try
    {
        Request q;
        q.kind = RequestKind::search_paths;
        q.text = effects ? effects : "";
        q.text2 = textures ? textures : "";
        if (q.text.empty() && q.text2.empty())
            return;
        report_push(g_requests.push(std::move(q)));
    }
    catch (...) {}
}

// Queues `d3d11Texture` (an ID3D11Texture2D*) for the next render event and takes a COM reference to it, released when
// the render event consumes it or a later call replaces it. `w` and `h` are unused: the bridge draws into mip 0 /
// layer 0 at the texture's own size. They stay in the signature for ABI compatibility.
extern "C" __declspec(dllexport) void rsb_queue_render(void *d3d11Texture, uint32_t w, uint32_t h)
{
    (void)w;
    (void)h;
    void *previous = nullptr;
    try
    {
        if (d3d11Texture != nullptr)
            static_cast<IUnknown *>(d3d11Texture)->AddRef();
        {
            std::lock_guard<std::mutex> lock(g_render_lock);
            previous = g_pending_texture;
            g_pending_texture = d3d11Texture;
            g_last_render = 0;
        }
        release_texture(previous);
    }
    catch (...) {}
}

// Meaningful only after the render event for that queue has run.
// -1 no runtime, -2 nothing queued, -3 view creation failed, else the number of techniques drawn (0 = nothing drawn).
extern "C" __declspec(dllexport) int rsb_last_render() { return g_last_render.load(); }

extern "C" __declspec(dllexport) void *rsb_render_event_func() { return reinterpret_cast<void *>(&render_event); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!reshade::register_addon(module))
            return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy);
        reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
        reshade::register_event<reshade::addon_event::reshade_set_technique_state>(on_set_technique_state);
        reshade::register_event<reshade::addon_event::reshade_set_effects_state>(on_set_effects_state);
        reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(on_set_preset_path);
        reshade::register_event<reshade::addon_event::reshade_reorder_techniques>(on_reorder_techniques);
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
