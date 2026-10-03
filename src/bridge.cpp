// Stellar ReShade bridge — a ReShade add-on (ReShade 6.8.0 add-on API) that lets a host application switch ReShade
// effects and draw them into its own texture, through a small C ABI.
//
// Threading model:
//   * Every ReShade call that CHANGES state (technique toggles, effects on/off, preset, search paths) runs on the render
//     thread inside ReShade's own present path (the reshade_present event). The exported rsb_request_* functions only
//     enqueue requests.
//   * Requests are applied only while ReShade is not loading (the technique list is non-empty), all of them, in request
//     order. If any applied request asked to be saved, save_current_preset() is called exactly once for that frame.
//   * The exported getters read a snapshot that the reshade_present callback refreshes every frame.
//   * The Unity render-event callback (rsb_render_event_func) runs on the host's render thread and is the only other
//     place that touches the runtime.
//   * Locks are held only to swap or copy small data. The add-on never calls back into the host, and no C++ exception
//     crosses the ABI or a ReShade callback.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <reshade.hpp>
#include "bridge_state.hpp"

using namespace reshade::api;
using namespace stellar_rsb;

extern "C" __declspec(dllexport) const char *NAME = "Stellar ReShade bridge";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Lets the Stellar framework switch ReShade effects and draw them into photos.";

static constexpr int bridge_abi_version = 1;

static std::atomic<effect_runtime *> g_runtime{ nullptr };
static RequestQueue g_requests;
static SnapshotStore g_snapshot;

// ---- offscreen render (host render thread) ----
struct PendingRender { void *texture; uint32_t width, height; };
static std::mutex g_render_lock;
static PendingRender g_pending_render{};
static std::atomic<int> g_last_render{ 0 };
static std::atomic<bool> g_counting{ false };
static std::atomic<int> g_drawn{ 0 };

// ---------------------------------------------------------------------------------------------------------------------
// ReShade string helpers (size query, then fill).

template <typename Getter>
static void read_string(std::string &out, Getter get)
{
    size_t size = 0;
    get(nullptr, &size); // size includes the terminating NUL
    if (size <= 1) { out.clear(); return; }
    out.resize(size);
    get(&out[0], &size); // size now = characters copied
    out.resize(size);
}

static size_t count_techniques(effect_runtime *r)
{
    size_t n = 0;
    // enumerate_techniques returns nothing while ReShade is loading (runtime_api.cpp: `if (is_loading()) return;`).
    r->enumerate_techniques(nullptr, [](effect_runtime *, effect_technique, void *u) { ++*static_cast<size_t *>(u); }, &n);
    return n;
}

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
            read_string(s.name, [&](char *b, size_t *n) { rt->get_technique_name(t, b, n); });
            read_string(s.effect, [&](char *b, size_t *n) { rt->get_technique_effect_name(t, b, n); });
            s.enabled = rt->get_technique_state(t);
            f->index++;
        }
        catch (...) {} // never unwind through ReShade's enumeration loop
    }, &fill);
    snap.technique_count = fill.index;
    snap.loading = fill.index == 0; // no public is_loading API in 6.8.0 — inferred from "0 techniques enumerated"
    snap.effects_enabled = r->get_effects_state();
    read_string(snap.preset_path, [&](char *b, size_t *n) { r->get_current_preset_path(b, n); });
}

// ---------------------------------------------------------------------------------------------------------------------
// Request application (render thread, inside reshade_present, only while not loading).

struct TechniqueMatch { const char *name; bool on; std::string scratch; };

static void apply_technique(effect_runtime *r, const Request &q)
{
    TechniqueMatch m{ q.text.c_str(), q.on, {} };
    r->enumerate_techniques(nullptr, [](effect_runtime *rt, effect_technique t, void *u) {
        try
        {
            auto *m = static_cast<TechniqueMatch *>(u);
            read_string(m->scratch, [&](char *b, size_t *n) { rt->get_technique_name(t, b, n); });
            if (m->scratch == m->name)
                rt->set_technique_state(t, m->on);
        }
        catch (...) {} // never unwind through ReShade's enumeration loop
    }, &m);
}

// ';'-separated list -> '\0'-separated array for ReShade's config array setter. Returns false if the list has no entries.
static bool to_config_array(const std::string &list, std::string &out)
{
    out.clear();
    size_t start = 0;
    while (start <= list.size())
    {
        size_t end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        if (end > start)
        {
            if (!out.empty()) out.push_back('\0');
            out.append(list, start, end - start);
        }
        start = end + 1;
    }
    return !out.empty();
}

static void apply_search_paths(effect_runtime *r, const Request &q)
{
    std::string array;
    bool changed = false;
    if (to_config_array(q.text, array))
    {
        reshade::set_config_value(r, "GENERAL", "EffectSearchPaths", array.data(), array.size());
        changed = true;
    }
    if (to_config_array(q.text2, array))
    {
        reshade::set_config_value(r, "GENERAL", "TextureSearchPaths", array.data(), array.size());
        changed = true;
    }
    if (changed)
        r->reload_effect_next_frame(nullptr); // nullptr = reload all effects
}

// Applies requests in order while ReShade stays out of loading. A request can start a reload (a preset switch can compile a
// new permutation), so loading is re-checked before each one; the first request that meets a loading runtime and every
// request after it go back to the front of the queue for a later frame. Returns true if an applied request asked to be saved.
static bool apply_requests(effect_runtime *r, std::vector<Request> &requests)
{
    bool save = false;
    for (size_t i = 0; i < requests.size(); ++i)
    {
        if (i != 0 && count_techniques(r) == 0)
        {
            g_requests.put_back_front(requests, i);
            break;
        }
        const Request &q = requests[i];
        switch (q.kind)
        {
        case RequestKind::effects_enabled: r->set_effects_state(q.on); break;
        case RequestKind::technique: apply_technique(r, q); save = save || q.save; break;
        case RequestKind::preset:
            if (save)
            {
                // Re-selecting the same preset reloads it from disk, which would drop the saved toggles applied above, so
                // save them first. That is this frame's one save: the switch ends the batch and the rest waits a frame.
                r->save_current_preset();
                r->set_current_preset_path(q.text.c_str());
                g_requests.put_back_front(requests, i + 1);
                return false;
            }
            r->set_current_preset_path(q.text.c_str());
            break;
        case RequestKind::search_paths: apply_search_paths(r, q); break;
        }
    }
    return save;
}

// ---------------------------------------------------------------------------------------------------------------------
// ReShade events.

static void on_init(effect_runtime *r) { g_runtime = r; }

static void on_destroy(effect_runtime *r)
{
    effect_runtime *expected = r;
    if (g_runtime.compare_exchange_strong(expected, nullptr))
        g_snapshot.reset();
}

// Render thread, inside ReShade's present (runtime.cpp on_present, right before _effects_rendered_this_frame resets).
static void on_reshade_present(effect_runtime *r)
{
    try
    {
        if (r != g_runtime.load())
            return;

        // Render-thread-private buffers, reused every frame to avoid per-frame allocation.
        static std::vector<Request> s_requests;
        static Snapshot s_work;

        if (count_techniques(r) != 0) // not loading: apply everything queued so far, in order
        {
            s_requests.clear();
            g_requests.take_all(s_requests);
            // At most once per frame, only when a saved request was applied (and never while loading).
            if (!s_requests.empty() && apply_requests(r, s_requests) && count_techniques(r) != 0)
                r->save_current_preset();
            s_requests.clear();
        }
        // While loading, requests stay queued (in order) until a later frame.

        fill_snapshot(r, s_work);
        g_snapshot.publish(s_work);
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

// Host render-thread callback (Unity GL.IssuePluginEvent). x64: one calling convention.
static void render_event(int)
{
    try
    {
        PendingRender p;
        {
            std::lock_guard<std::mutex> lock(g_render_lock);
            p = g_pending_render;
            g_pending_render = {};
        }
        effect_runtime *r = g_runtime.load();
        if (!r) { g_last_render = -1; return; }
        if (!p.texture) { g_last_render = -2; return; }

        device *d = r->get_device();
        const resource res{ reinterpret_cast<uint64_t>(p.texture) };
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
        g_requests.push(std::move(q));
    }
    catch (...) {}
}

extern "C" __declspec(dllexport) int rsb_technique_count()
{
    try { return g_snapshot.technique_count(); } catch (...) { return 0; }
}

// Returns 1 and fills the buffers (NUL-terminated, truncated to fit), or 0 if `i` is out of range.
extern "C" __declspec(dllexport) int rsb_technique_at(int i, char *name, int nameLen, char *effect, int effectLen, int *enabled)
{
    try { return g_snapshot.technique_at(i, name, nameLen, effect, effectLen, enabled); } catch (...) { return 0; }
}

// save = 0: temporary (not written to the preset); save = 1: saved to the current preset.
extern "C" __declspec(dllexport) void rsb_request_technique(const char *name, int on, int save)
{
    try
    {
        if (!name || !*name)
            return;
        Request q;
        q.kind = RequestKind::technique;
        q.text = name;
        q.on = on != 0;
        q.save = save != 0;
        g_requests.push(std::move(q));
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
        g_requests.push(std::move(q));
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
        g_requests.push(std::move(q));
    }
    catch (...) {}
}

extern "C" __declspec(dllexport) void rsb_queue_render(void *d3d11Texture, uint32_t w, uint32_t h)
{
    try
    {
        std::lock_guard<std::mutex> lock(g_render_lock);
        g_pending_render = { d3d11Texture, w, h };
        g_last_render = 0;
    }
    catch (...) {}
}

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
        reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
        reshade::register_event<reshade::addon_event::reshade_render_technique>(on_render_technique);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_addon(module);
    }
    return TRUE;
}
