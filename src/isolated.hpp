// Stellar ReShade bridge — rendering a photo in a separate ("isolated") effect runtime.
//
// Why: ReShade 6.8.0 compiles one permutation per render-target size, but an effect's named textures live in ONE list per
// runtime, and a texture the screen-size permutation already created is reused at the wrong size by a larger permutation
// (source/runtime.cpp:2035-2084). Multi-pass effects then read the top-left part of the photo and magnify it. A runtime
// whose OWN back buffer has the photo's size has no other permutation: render_effects on that back buffer uses the
// default permutation (runtime.cpp:3962-3964 "add-on passed in the back buffer"), so every texture has the right size.
//
// How (all on the render thread, inside the host's render event):
//   begin   : the game runtime's LIVE technique states (plus the host's isolated technique requests) are written as the
//             Techniques list of a copy of the current preset; a config file points the new runtime at that copy (see
//             isolated_files.hpp). A width x height texture backs an IsolatedSwapChain (isolated_swapchain.hpp), and
//             reshade::create_effect_runtime builds the runtime on the game's own D3D11 device and immediate context.
//   step    : while the runtime has no techniques listed (loading), each event presents it once
//             (update_and_present_effect_runtime): ReShade loads effects only inside its present (update_effects,
//             runtime.cpp:3660+, called from on_present at runtime.cpp:731) and creates ONE effect per present
//             (runtime.cpp:3755-3760). Techniques can be listed only when it is not loading (runtime_api.cpp:751-754).
//   render  : copy the host texture into the back buffer, render_effects on it, copy the result back. render_effects
//             draws at most once between two presents of this runtime (_effects_rendered_this_frame, runtime.cpp:3804-3807,
//             reset at the end of on_present, runtime.cpp:939), so a present runs before every render but the first.
//   end     : destroy the runtime (it frees every effect resource), then release the swap chain, texture and device refs.
//
// The runtime has no input window (no hotkeys), no depth buffer (the built-in depth add-on keeps its data on the GAME's
// device wrapper; the isolated runtime gets a new wrapper whose private data is empty, so that add-on skips it —
// generic_depth_addon.cpp on_begin_render_effects returns when its device data is null), and it never writes the
// user's preset (it uses a copy).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include <reshade.hpp>
#include "effect_files.hpp"
#include "isolated_files.hpp"
#include "isolated_swapchain.hpp"
#include "loading_state.hpp"
#include "overrides.hpp"

namespace stellar_rsb
{
    // rsb_isolated_state() values.
    enum IsolatedState : int
    {
        iso_idle = 0,          // nothing begun (or ended and torn down)
        iso_starting = 1,      // begin queued; waits for a render event (and for the game's runtime to finish loading)
        iso_loading = 2,       // runtime exists and is loading/compiling effects: keep issuing render events
        iso_ready = 3,         // techniques are listed and not loading: a queued render will draw
        iso_empty = 4,         // nothing to draw (effects off globally, or no technique enabled): no runtime was created
        iso_ending = 5,        // end queued; the next render event frees everything
        iso_err_no_runtime = -1,      // ReShade has given the bridge no runtime yet, or the game is not Direct3D 11
        iso_err_bad_argument = -2,    // size out of range, missing/relative config path, or it would overwrite the preset
        iso_err_too_large = -3,       // larger than the device's maximum texture size
        iso_err_runtime_sync = -4,    // ReShade's "Effect Runtime Sync" would mirror the isolated runtime onto the game's
        iso_err_resource = -5,        // a texture or view could not be created (for example out of video memory)
        iso_err_file = -6,            // the config or preset copy could not be written
        iso_err_runtime_refused = -7, // ReShade refused to create the runtime (or lacks the export)
        iso_err_lost = -8,            // the game's runtime or device went away (resize, device loss, shutdown)
        iso_err_internal = -9,        // unexpected failure
        iso_err_busy = -10,           // rsb_isolated_begin only: a session is starting or active; end it first
    };

    // rsb_isolated_last_render() values (>= 0: number of techniques drawn).
    enum IsolatedRender : int
    {
        iso_render_none = 0,
        iso_render_no_runtime = -1,  // no isolated runtime (not begun, starting, failed or ended)
        iso_render_nothing_queued = -2,
        iso_render_failed = -3,      // render-target view could not be used
        iso_render_not_ready = -4,   // the runtime is (again) loading; queue the render again on a later frame
        iso_render_mismatch = -5,    // the texture is not width x height, multisampled, or not an RGBA8 format
        iso_render_internal = -9,
    };

    // Set while reshade::create_effect_runtime runs on this thread: its init_effect_runtime event belongs to the
    // isolated runtime, not to the game's.
    inline thread_local bool t_creating_isolated = false;

    namespace isolated_detail
    {
        // ReShade config array from `section` (runtime config, or the global config for nullptr).
        inline bool read_config_section_array(reshade::api::effect_runtime *runtime, const char *section, const char *key,
            std::vector<std::string> &out)
        {
            out.clear();
            size_t size = 0;
            if (!reshade::get_config_value(runtime, section, key, nullptr, &size))
                return false;
            if (size == 0)
                return true;
            std::string buf(size, '\0');
            reshade::get_config_value(runtime, section, key, &buf[0], &size);
            buf.resize(size);
            size_t start = 0;
            while (start < buf.size())
            {
                size_t end = buf.find('\0', start);
                if (end == std::string::npos) end = buf.size();
                if (end > start) out.emplace_back(buf, start, end - start);
                start = end + 1;
            }
            return true;
        }

        // Mirrors the built-in "Effect Runtime Sync" add-on's switch: global [ADDON] SyncEffectRuntimes when it parses as an
        // integer, otherwise on only when a VR runtime is loaded (ReShade 6.8.0 examples/15-effect_runtime_sync on_init).
        // When sync is on, that add-on copies preset switches, technique states and the effects toggle between ALL runtimes,
        // which would push the isolated runtime's preset copy onto the game's runtime.
        inline bool runtime_sync_may_be_active()
        {
            std::vector<std::string> values;
            if (read_config_section_array(nullptr, "ADDON", "DisabledAddons", values))
                for (const std::string &name : values)
                    if (name == "Effect Runtime Sync")
                        return false;
            if (read_config_section_array(nullptr, "ADDON", "SyncEffectRuntimes", values) && !values.empty())
            {
                int value = 0;
                const std::string &s = values[0];
                if (std::from_chars(s.data(), s.data() + s.size(), value).ec == std::errc{})
                    return value != 0;
            }
            return GetModuleHandleW(L"openvr_api.dll") != nullptr || GetModuleHandleW(L"openxr_loader.dll") != nullptr;
        }

        inline bool is_rgba8(DXGI_FORMAT format)
        {
            return format >= DXGI_FORMAT_R8G8B8A8_TYPELESS && format <= DXGI_FORMAT_R8G8B8A8_SINT;
        }

        template <typename T>
        inline void release(T *&object) noexcept
        {
            if (object != nullptr)
            {
                try { object->Release(); } catch (...) {}
                object = nullptr;
            }
        }

        using RequestMap = std::map<TechniqueKey, bool>; // (effect file, technique) -> enabled; effect "" = any effect

        inline const bool *find_request(const RequestMap &requests, const TechniqueKey &key)
        {
            auto it = requests.find(key);
            if (it == requests.end())
                it = requests.find(TechniqueKey{ std::string(), key.second });
            return it != requests.end() ? &it->second : nullptr;
        }
    }

    // Technique requests for the isolated runtime (any thread -> render thread).
    class IsolatedRequests
    {
    public:
        void set(const std::string &effect, const std::string &name, bool on)
        {
            std::lock_guard<std::mutex> lock(_lock);
            _map[TechniqueKey{ effect, name }] = on;
            _version++;
        }
        void clear()
        {
            std::lock_guard<std::mutex> lock(_lock);
            _map.clear();
            _version++;
        }
        // Copies the map into `out` if it changed since `version`; returns true if it copied.
        bool copy_if_changed(isolated_detail::RequestMap &out, uint64_t &version)
        {
            std::lock_guard<std::mutex> lock(_lock);
            if (version == _version)
                return false;
            out = _map;
            version = _version;
            return true;
        }

    private:
        std::mutex _lock;
        isolated_detail::RequestMap _map;
        uint64_t _version = 1;
    };

    // Host -> render thread hand-off. Every host call changes it under the lock; the render event takes it at its start
    // and publishes the resulting state only if no newer host call arrived meanwhile (sequence number).
    struct IsolatedPending
    {
        std::mutex lock;
        uint64_t sequence = 0;     // bumped by every begin / end
        bool begin = false;
        bool end = false;
        uint32_t width = 0, height = 0;
        std::string config_path;
        void *texture = nullptr;   // holds one COM reference
    };

    // The live session. Render thread only, under g_iso_lock.
    struct IsolatedSession
    {
        reshade::api::effect_runtime *runtime = nullptr;
        IsolatedSwapChain *swapchain = nullptr;
        ID3D11Device *device = nullptr;
        ID3D11DeviceContext *context = nullptr;
        ID3D11Texture2D *buffer = nullptr;
        uint64_t game_device = 0;  // native device of the game's runtime, to match destroy_device
        reshade::api::resource_view rtv{}, rtv_srgb{};
        uint32_t width = 0, height = 0;
        bool needs_present = false; // a render happened since the last present
        isolated_detail::RequestMap requests;
        uint64_t requests_version = 0;
    };

    inline IsolatedRequests g_iso_requests;
    inline IsolatedPending g_iso_pending;
    inline IsolatedSession g_iso;
    inline std::recursive_mutex g_iso_lock;  // destroying the runtime re-enters the bridge's destroy_effect_runtime handler
    inline std::atomic<int> g_iso_state{ iso_idle };
    inline std::atomic<int> g_iso_last_render{ iso_render_no_runtime };

    inline bool isolated_owns(reshade::api::effect_runtime *runtime)
    {
        return runtime != nullptr && runtime == g_iso.runtime;
    }

    // Frees everything the session holds. Safe on a partly built session. Caller holds g_iso_lock.
    inline void isolated_teardown() noexcept
    {
        IsolatedSession &s = g_iso;
        try
        {
            if (s.runtime != nullptr)
            {
                reshade::api::device *const d = s.runtime->get_device();
                if (s.rtv_srgb.handle != 0 && s.rtv_srgb.handle != s.rtv.handle)
                    d->destroy_resource_view(s.rtv_srgb);
                if (s.rtv.handle != 0)
                    d->destroy_resource_view(s.rtv);
                reshade::api::effect_runtime *const runtime = s.runtime;
                // Keep s.runtime set while ReShade destroys it: its destroy_effect_runtime event comes back to the bridge,
                // which must recognise it as the isolated runtime (and not as the game's).
                reshade::destroy_effect_runtime(runtime); // waits for its compile threads and the GPU, frees effect resources
            }
        }
        catch (...) {}
        s.runtime = nullptr;
        s.rtv = {};
        s.rtv_srgb = {};
        isolated_detail::release(s.swapchain);
        isolated_detail::release(s.buffer);
        if (s.context != nullptr)
        {
            try { s.context->Flush(); } catch (...) {} // lets the driver release the freed memory promptly
        }
        isolated_detail::release(s.context);
        isolated_detail::release(s.device);
        s.game_device = 0;
        s.width = s.height = 0;
        s.needs_present = false;
        s.requests.clear();
        s.requests_version = 0;
    }

    namespace isolated_detail
    {
        constexpr int begin_deferred = 100; // internal: the game's runtime is loading; try again next event

        struct Collect
        {
            const RequestMap *requests;
            std::vector<std::string> *enabled;
            std::vector<std::string> *sorting;
            TechniqueKey key;
        };

        // Lists the game runtime's techniques in its order with their LIVE states (temporary overrides included), with the
        // isolated technique requests applied on top.
        inline void collect_techniques(reshade::api::effect_runtime *game, const RequestMap &requests,
            std::vector<std::string> &enabled, std::vector<std::string> &sorting)
        {
            Collect c{ &requests, &enabled, &sorting, {} };
            game->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *rt, reshade::api::effect_technique t, void *u) {
                try
                {
                    auto *c = static_cast<Collect *>(u);
                    read_key(rt, t, c->key);
                    const std::string entry = c->key.first.empty() ? c->key.second : c->key.second + '@' + c->key.first;
                    c->sorting->push_back(entry);
                    bool on = rt->get_technique_state(t);
                    if (const bool *requested = find_request(*c->requests, c->key))
                        on = *requested;
                    if (on)
                        c->enabled->push_back(entry);
                }
                catch (...) {} // never unwind through ReShade's enumeration loop
            }, &c);
        }

        inline std::filesystem::path current_preset_path(reshade::api::effect_runtime *game)
        {
            std::string preset;
            read_reshade_string(preset, [&](char *b, size_t *n) { game->get_current_preset_path(b, n); });
            std::filesystem::path path = std::filesystem::u8path(preset);
            if (!path.empty() && path.is_relative())
            {
                size_t size = 0;
                reshade::get_reshade_base_path(nullptr, &size);
                if (size > 1)
                {
                    std::string base(size, '\0');
                    reshade::get_reshade_base_path(&base[0], &size);
                    base.resize(size);
                    path = std::filesystem::u8path(base) / path;
                }
            }
            return path;
        }

        inline bool same_file(const std::filesystem::path &a, const std::filesystem::path &b)
        {
            std::error_code ec;
            if (std::filesystem::equivalent(a, b, ec) && !ec)
                return true;
            return a.lexically_normal() == b.lexically_normal();
        }

        // Enforces the isolated technique requests on the isolated runtime (only while it lists techniques).
        struct Enforce { const RequestMap *requests; TechniqueKey key; };
        inline void enforce_requests(reshade::api::effect_runtime *iso, const RequestMap &requests)
        {
            if (requests.empty())
                return;
            Enforce e{ &requests, {} };
            iso->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *rt, reshade::api::effect_technique t, void *u) {
                try
                {
                    auto *e = static_cast<Enforce *>(u);
                    read_key(rt, t, e->key);
                    if (const bool *requested = find_request(*e->requests, e->key))
                        if (rt->get_technique_state(t) != *requested)
                            rt->set_technique_state(t, *requested);
                }
                catch (...) {}
            }, &e);
        }

        // Builds the session. Returns iso_loading on success, iso_empty, begin_deferred or an error code. On failure
        // everything built so far is torn down by the caller.
        inline int begin_session(uint32_t width, uint32_t height, const std::string &config_utf8,
            reshade::api::effect_runtime *game)
        {
            if (game == nullptr)
                return iso_err_no_runtime;
            reshade::api::device *const game_device = game->get_device();
            if (game_device == nullptr || game_device->get_api() != reshade::api::device_api::d3d11)
                return iso_err_no_runtime;

            if (is_loading_now(count_techniques(game)))
                return begin_deferred;
            if (!game->get_effects_state())
                return iso_empty;
            if (runtime_sync_may_be_active())
                return iso_err_runtime_sync;

            IsolatedSession &s = g_iso;
            g_iso_requests.copy_if_changed(s.requests, s.requests_version);

            std::vector<std::string> enabled, sorting;
            collect_techniques(game, s.requests, enabled, sorting);
            if (enabled.empty())
                return iso_empty; // with no enabled technique ReShade would compile EVERY effect (runtime.cpp:1619-1636)

            const std::filesystem::path config_path = std::filesystem::u8path(config_utf8);
            if (config_path.empty() || !config_path.is_absolute())
                return iso_err_bad_argument;
            const std::filesystem::path preset_copy = isolated_preset_path(config_path);
            const std::filesystem::path preset = current_preset_path(game);
            if (!preset.empty() && (same_file(preset, preset_copy) || same_file(preset, config_path)))
                return iso_err_bad_argument; // never overwrite the user's preset

            ID3D11Device *const native = reinterpret_cast<ID3D11Device *>(game_device->get_native());
            if (native == nullptr || FAILED(native->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void **>(&s.device))))
                return iso_err_no_runtime;
            s.game_device = game_device->get_native();

            const UINT max_size = s.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : 8192u;
            if (width > max_size || height > max_size)
                return iso_err_too_large;

            if (!write_isolated_config(game, config_path, preset_copy) || !write_isolated_preset(preset, preset_copy, enabled, sorting))
                return iso_err_file;

            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; // unorm + sRGB render-target views, like the host's texture
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(s.device->CreateTexture2D(&desc, nullptr, &s.buffer)) || s.buffer == nullptr)
                return iso_err_resource;

            s.device->GetImmediateContext(&s.context);
            if (s.context == nullptr)
                return iso_err_internal;

            s.swapchain = new IsolatedSwapChain(s.device, s.buffer, width, height);
            s.width = width;
            s.height = height;

            // ReShadeCreateEffectRuntime requires GetDevice on the swap chain to return exactly the device pointer passed in.
            ID3D11Device *device_arg = nullptr;
            const HRESULT device_hr = s.swapchain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&device_arg));
            if (device_arg != nullptr)
                device_arg->Release(); // only the pointer is needed; s.device keeps the device alive
            if (FAILED(device_hr) || device_arg == nullptr || device_arg != s.device)
                return iso_err_internal;

            reshade::api::effect_runtime *runtime = nullptr;
            bool created = false;
            t_creating_isolated = true;
            try
            {
                // ReShade casts each void* straight to IUnknown*, so pass interface pointers, not the C++ class pointer.
                created = reshade::create_effect_runtime(reshade::api::device_api::d3d11,
                    static_cast<IUnknown *>(device_arg), static_cast<IUnknown *>(s.context),
                    static_cast<IUnknown *>(static_cast<IDXGISwapChain *>(s.swapchain)), config_path.u8string().c_str(), &runtime);
            }
            catch (...) { created = false; }
            t_creating_isolated = false;
            if (!created || runtime == nullptr)
                return iso_err_runtime_refused;
            s.runtime = runtime;

            reshade::api::device *const d = runtime->get_device();
            const reshade::api::resource res{ reinterpret_cast<uint64_t>(s.buffer) };
            if (!d->create_resource_view(res, reshade::api::resource_usage::render_target,
                    reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm, 0, 1, 0, 1), &s.rtv))
                return iso_err_resource;
            if (!d->create_resource_view(res, reshade::api::resource_usage::render_target,
                    reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm_srgb, 0, 1, 0, 1), &s.rtv_srgb))
                s.rtv_srgb = s.rtv;

            s.needs_present = false; // the first step presents anyway (nothing is listed before the first load)
            return iso_loading;
        }

        inline void present_once(IsolatedSession &s)
        {
            reshade::update_and_present_effect_runtime(s.runtime); // on_present + flush (addon.cpp:265-273)
            s.needs_present = false;
        }

        // One step of a live session: drive loading and enforce requests. Returns the state.
        inline int step_session(IsolatedSession &s)
        {
            g_iso_requests.copy_if_changed(s.requests, s.requests_version);
            size_t count = count_techniques(s.runtime);
            if (count == 0)
            {
                present_once(s);
                count = count_techniques(s.runtime);
            }
            if (count != 0)
            {
                enforce_requests(s.runtime, s.requests);
                count = count_techniques(s.runtime); // enabling a technique whose effect was never created starts a load
            }
            return count != 0 ? iso_ready : iso_loading;
        }

        inline int validate_texture(const IsolatedSession &s, void *texture)
        {
            ID3D11Texture2D *tex = nullptr;
            if (FAILED(static_cast<IUnknown *>(texture)->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
                return iso_render_mismatch;
            D3D11_TEXTURE2D_DESC desc = {};
            tex->GetDesc(&desc);
            tex->Release();
            if (desc.Width != s.width || desc.Height != s.height || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
                !is_rgba8(desc.Format) || desc.Usage != D3D11_USAGE_DEFAULT)
                return iso_render_mismatch;
            return 0;
        }

        // Draws the isolated runtime's effects into `texture`. Returns the techniques drawn or an iso_render_* code.
        inline int render_session(IsolatedSession &s, void *texture, std::atomic<bool> &counting, std::atomic<int> &drawn)
        {
            if (const int bad = validate_texture(s, texture); bad != 0)
                return bad;
            if (s.needs_present)
            {
                present_once(s);
                enforce_requests(s.runtime, s.requests);
            }
            if (count_techniques(s.runtime) == 0)
                return iso_render_not_ready;

            ID3D11Resource *src = nullptr;
            if (FAILED(static_cast<IUnknown *>(texture)->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void **>(&src))) || src == nullptr)
                return iso_render_mismatch;

            s.context->CopyResource(s.buffer, src);
            drawn = 0;
            counting = true;
            try
            {
                s.runtime->render_effects(s.runtime->get_command_queue()->get_immediate_command_list(), s.rtv, s.rtv_srgb);
            }
            catch (...)
            {
                counting = false;
                src->Release();
                return iso_render_internal;
            }
            counting = false;
            s.context->CopyResource(src, s.buffer);
            src->Release();
            s.needs_present = true;
            return drawn.load();
        }
    }

    // The isolated runtime's game runtime or device went away: tear down and report it. Render thread (ReShade events).
    inline void isolated_lost() noexcept
    {
        try
        {
            std::lock_guard<std::recursive_mutex> lock(g_iso_lock);
            if (g_iso.runtime == nullptr && g_iso.device == nullptr)
                return;
            isolated_teardown();
            std::lock_guard<std::mutex> pending(g_iso_pending.lock);
            const int state = g_iso_state.load();
            if (state == iso_loading || state == iso_ready)
                g_iso_state = iso_err_lost;
        }
        catch (...) {}
    }

    // The isolated render event body: end, begin, step, render — in that order, at most ONE present of the isolated
    // runtime per event. `game` is the game's runtime. Render thread.
    inline void isolated_event(reshade::api::effect_runtime *game, std::atomic<bool> &counting, std::atomic<int> &drawn) noexcept
    {
        using namespace isolated_detail;
        void *texture = nullptr;
        try
        {
            std::lock_guard<std::recursive_mutex> lock(g_iso_lock);
            IsolatedPending &p = g_iso_pending;
            bool do_end = false, do_begin = false;
            uint32_t width = 0, height = 0;
            std::string config;
            uint64_t sequence = 0;
            {
                std::lock_guard<std::mutex> pending(p.lock);
                do_end = p.end;
                do_begin = p.begin;
                width = p.width;
                height = p.height;
                config = p.config_path;
                texture = p.texture;
                p.texture = nullptr;
                p.end = false;
                p.begin = false;
                sequence = p.sequence;
            }

            bool publish = false;
            int state = iso_idle;
            if (do_end)
            {
                isolated_teardown();
                state = iso_idle;
                publish = true;
            }
            if (do_begin)
            {
                isolated_teardown();
                int result = iso_err_internal;
                try { result = begin_session(width, height, config, game); } catch (...) { result = iso_err_internal; }
                if (result == begin_deferred)
                {
                    isolated_teardown();
                    std::lock_guard<std::mutex> pending(p.lock);
                    if (p.sequence == sequence && !p.begin && !p.end)
                        p.begin = true; // parameters are still in place; try again on the next event
                    state = iso_starting;
                }
                else if (result != iso_loading)
                {
                    isolated_teardown();
                    state = result;
                }
                else
                {
                    state = iso_loading;
                }
                publish = true;
            }
            if (g_iso.runtime != nullptr)
            {
                try { state = step_session(g_iso); }
                catch (...) { isolated_teardown(); state = iso_err_internal; }
                publish = true;
            }

            if (texture != nullptr)
            {
                int code = iso_render_no_runtime;
                if (g_iso.runtime != nullptr)
                {
                    if (state != iso_ready)
                        code = iso_render_not_ready;
                    else
                    {
                        try { code = render_session(g_iso, texture, counting, drawn); }
                        catch (...) { counting = false; code = iso_render_internal; }
                    }
                }
                g_iso_last_render = code;
            }

            if (publish)
            {
                std::lock_guard<std::mutex> pending(p.lock);
                if (p.sequence == sequence)
                    g_iso_state = state; // a newer begin/end already set its own state; the next event handles it
            }
        }
        catch (...)
        {
            counting = false;
        }
        if (texture != nullptr)
        {
            try { static_cast<IUnknown *>(texture)->Release(); } catch (...) {}
        }
    }
}
