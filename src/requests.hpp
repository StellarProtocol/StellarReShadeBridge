// Stellar ReShade bridge — applying queued requests (ReShade's render thread, inside reshade_present).
//
// Rules, each re-checked at the moment it matters (ReShade can start loading in the middle of a batch, see
// loading_state.hpp):
//   * Before EVERY request: if ReShade is loading now, the request and everything after it go back to the front of the
//     queue for a later frame.
//   * A saved request sets g_save_pending; the save itself happens on the first frame (this one or later) where
//     ReShade lists techniques and is not loading. g_save_pending survives frames, so a batch that ends in a load does
//     not lose its save. At most one bridge save per frame.
//   * A save or a preset switch only happens while techniques are listed (count > 0) and not loading. While loading,
//     save_current_preset would write whatever half-loaded state exists and the override revert would enumerate
//     nothing (source/runtime_api.cpp:751-754, 1325-1328), and set_current_preset_path only stores the path without
//     loading the preset (source/runtime_api.cpp:1378-1381).
#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>
#include <reshade.hpp>
#include "bridge_state.hpp"
#include "effect_files.hpp"
#include "loading_state.hpp"
#include "overrides.hpp"

namespace stellar_rsb
{
    inline RequestQueue g_requests;
    inline TemporaryOverrides g_overrides;              // render thread only
    inline bool g_save_pending = false;                 // render thread only: a saved change still has to be written
    inline std::atomic<bool> g_snapshot_dirty{ true };  // the published snapshot may be stale

    // ReShade's log writes one line per call with no shared state but the file handle (source/dll_log.cpp:55-106), so it
    // may be called from any thread.
    inline void log_warning(const char *message) noexcept
    {
        try { reshade::log::message(reshade::log::level::warning, message); } catch (...) {}
    }

    inline void report_push(PushResult result) noexcept
    {
        if (result == PushResult::dropped_first)
            log_warning("Stellar ReShade bridge: request queue is full; dropping new requests until ReShade takes them.");
    }

    namespace detail
    {
        struct TechniqueMatch { const Request *q; TechniqueKey key; size_t matched; };

        // Returns the number of techniques the request matched.
        inline size_t apply_technique(reshade::api::effect_runtime *r, const Request &q)
        {
            TechniqueMatch m{ &q, {}, 0 };
            r->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *rt, reshade::api::effect_technique t, void *u) {
                try
                {
                    auto *m = static_cast<TechniqueMatch *>(u);
                    read_key(rt, t, m->key);
                    if (m->key.second != m->q->text)
                        return;
                    if (!m->q->effect.empty() && m->key.first != m->q->effect)
                        return; // an empty effect name matches any effect
                    g_overrides.apply(rt, t, m->key, m->q->on, m->q->save);
                    m->matched++;
                }
                catch (...) {} // never unwind through ReShade's enumeration loop
            }, &m);
            return m.matched;
        }

        // Saves the current preset without the temporary overrides in it. Caller guarantees count > 0 and not loading.
        inline void save_without_overrides(reshade::api::effect_runtime *r)
        {
            g_overrides.revert(r);
            r->save_current_preset();
            g_overrides.reapply(r);
        }

        // Switches preset. ReShade saves the old preset itself first when the path differs
        // (source/runtime_api.cpp:1388-1396, then load_current_preset at 1398), so the temporary overrides are reverted around it.
        // Caller guarantees count > 0 and not loading.
        inline void switch_preset(reshade::api::effect_runtime *r, const std::string &path)
        {
            g_overrides.revert(r);
            {
                ApplyingScope scope; // the preset load toggles techniques; those are not user changes
                r->set_current_preset_path(path.c_str());
            }
            g_overrides.mark_rebase();
            g_overrides.reapply(r);
        }

        // ';'-separated list -> '\0'-separated array for ReShade's config array setter. Returns false if it has no entries.
        inline bool to_config_array(const std::string &list, std::string &out)
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

        // Only reached while ReShade is not loading, so no compile worker reads _effect_search_paths while
        // set_config_value's load_config rewrites it (source/addon.cpp:105-109, source/runtime.cpp:1015, 1557).
        inline void apply_search_paths(reshade::api::effect_runtime *r, const Request &q)
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
            if (!changed)
                return;
            record_loaded_search_paths(r);          // load_config just ran: this is exactly what ReShade now uses
            r->reload_effect_next_frame(nullptr);   // nullptr = reload all effects
            g_reload_requested = true;              // the old effects stay listed until the reload starts
            g_known_empty = false;
        }

        inline void put_back(std::vector<Request> &requests, size_t from)
        {
            if (g_requests.put_back_front(requests, from) != 0)
                log_warning("Stellar ReShade bridge: request queue is full; dropped the newest requests.");
        }
    }

    // Writes a pending save if ReShade lists techniques and is not loading right now. Returns true if it saved.
    inline bool try_pending_save(reshade::api::effect_runtime *r)
    {
        if (!g_save_pending)
            return false;
        const size_t count = count_techniques(r);
        if (count == 0 || is_loading_now(count))
            return false;
        detail::save_without_overrides(r);
        g_save_pending = false;
        g_snapshot_dirty = true;
        return true;
    }

    // Applies requests in order. `saved` says whether the bridge already saved this frame; returns the updated value.
    inline bool apply_requests(reshade::api::effect_runtime *r, std::vector<Request> &requests, bool saved)
    {
        for (size_t i = 0; i < requests.size(); ++i)
        {
            const size_t count = count_techniques(r);
            if (is_loading_now(count))
            {
                detail::put_back(requests, i);
                return saved;
            }
            const Request &q = requests[i];
            g_snapshot_dirty = true;
            switch (q.kind)
            {
            case RequestKind::effects_enabled:
                r->set_effects_state(q.on);
                break;
            case RequestKind::technique:
                if (detail::apply_technique(r, q) != 0 && q.save)
                    g_save_pending = true;
                break;
            case RequestKind::preset:
                if (count == 0)
                {
                    // "Loaded, empty": switching now would make ReShade save the old preset with an empty technique
                    // list (save_current_preset lists only existing techniques, source/runtime.cpp:1338-1365).
                    log_warning("Stellar ReShade bridge: preset request ignored because ReShade has no techniques loaded.");
                    break;
                }
                if (g_save_pending)
                {
                    // Save before switching: re-selecting the same preset reloads it from disk and would drop the change.
                    if (saved)
                    {
                        detail::put_back(requests, i); // one bridge save per frame: switch next frame
                        return saved;
                    }
                    detail::save_without_overrides(r);
                    g_save_pending = false;
                    saved = true;
                }
                detail::switch_preset(r, q.text);
                break;
            case RequestKind::search_paths:
                detail::apply_search_paths(r, q); // sets g_reload_requested: the next iteration stops the batch
                break;
            }
        }
        return saved;
    }
}
