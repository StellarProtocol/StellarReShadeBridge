// Stellar ReShade bridge — "is ReShade loading effects?" state.
//
// ReShade 6.8.0 has no public is_loading(). Internally it is
//   _reload_remaining_effects != max || !_reload_create_queue.empty()            (source/runtime.hpp:62)
// and every technique enumeration returns nothing while it is true                (source/runtime_api.cpp:751-754).
// So:
//   * a technique list with at least one entry proves ReShade is NOT loading right now;
//   * an empty list is ambiguous: loading, or loaded with no techniques. It counts as loading unless the bridge
//     established "loaded, empty" at the start of a reload (see on_reloaded_effects in bridge.cpp and effect_files.hpp).
// is_loading() can turn true in the middle of a reshade_present: enabling a technique whose effect was not created yet
// pushes that effect onto _reload_create_queue (source/runtime.cpp:3345-3349). The bridge therefore re-checks before
// every request it applies and before every save.
//
// All flags are written on ReShade's render thread (its events and reshade_present); atomics only because the exported
// getters may read them from other threads.
#pragma once

#include <atomic>
#include <cstddef>
#include <reshade.hpp>

namespace stellar_rsb
{
    // A load is (or may be) in progress. Set at init_effect_runtime and at a reload start; cleared when techniques can be
    // listed or when a reload start found nothing to load.
    inline std::atomic<bool> g_loading{ true };

    // A reload START found no effect files, so ReShade is done and simply has no techniques: an empty list is not
    // "loading". Only set by a reload-start event (no compile workers exist then: ReShade's load_effects returns before
    // spawning any when it finds no files, source/runtime.cpp:3410-3414) and cleared by the next one.
    inline std::atomic<bool> g_known_empty{ false };

    // The bridge asked for a full reload (search-path change) that has not STARTED yet. reload_effect_next_frame only
    // records the request (source/runtime_api.cpp:1447-1453); update_effects starts it on a later present, and only
    // while ReShade is not in a preset transition (source/runtime.cpp:3666). Until then the OLD effects are still
    // listed, so requests must keep waiting even though techniques can be enumerated.
    inline std::atomic<bool> g_reload_requested{ false };

    inline size_t count_techniques(reshade::api::effect_runtime *runtime)
    {
        size_t n = 0;
        runtime->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *, reshade::api::effect_technique, void *u) {
            ++*static_cast<size_t *>(u);
        }, &n);
        return n;
    }

    inline bool is_loading_now(size_t technique_count)
    {
        return g_loading.load() || g_reload_requested.load() || (technique_count == 0 && !g_known_empty.load());
    }
}
