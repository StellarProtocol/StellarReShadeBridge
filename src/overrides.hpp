// Stellar ReShade bridge — temporary technique overrides.
//
// A temporary request (save = 0) must never reach the preset file, but ReShade's save_current_preset() writes every
// technique's CURRENT state, and switching presets saves the old one first. So the bridge remembers each temporary
// override as (effect file, technique) -> { original state, wanted state } and, around every save and preset switch it
// performs, reverts the overrides, lets ReShade save, then re-applies them — all inside one reshade_present.
//
// All members are used on the render thread only (inside ReShade events); there is no lock. Every ReShade call that
// changes a technique is wrapped in ApplyingScope so the bridge's own reshade_set_technique_state handler can tell its
// own changes from a user's.
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <reshade.hpp>

namespace stellar_rsb
{
    // True while the bridge itself is changing technique states.
    inline std::atomic<bool> g_applying{ false };

    struct ApplyingScope
    {
        bool previous;
        ApplyingScope() : previous(g_applying.exchange(true)) {}
        ~ApplyingScope() { g_applying = previous; }
        ApplyingScope(const ApplyingScope &) = delete;
        ApplyingScope &operator=(const ApplyingScope &) = delete;
    };

    template <typename Getter>
    inline void read_reshade_string(std::string &out, Getter get)
    {
        size_t size = 0;
        get(nullptr, &size); // size includes the terminating NUL
        if (size <= 1) { out.clear(); return; }
        out.resize(size);
        get(&out[0], &size); // size now = characters copied
        out.resize(size);
    }

    using TechniqueKey = std::pair<std::string, std::string>; // (effect file name, technique name)

    inline void read_key(reshade::api::effect_runtime *rt, reshade::api::effect_technique t, TechniqueKey &key)
    {
        read_reshade_string(key.first, [&](char *b, size_t *n) { rt->get_technique_effect_name(t, b, n); });
        read_reshade_string(key.second, [&](char *b, size_t *n) { rt->get_technique_name(t, b, n); });
    }

    class TemporaryOverrides
    {
    public:
        bool empty() const { return _map.empty(); }
        void clear() { _map.clear(); _rebase_pending = false; }

        // Applies one matched technique of a request. A saved request makes its state the new baseline (no override);
        // a temporary request records the state it replaces, and a temporary request back to that state ends the override.
        void apply(reshade::api::effect_runtime *rt, reshade::api::effect_technique t, const TechniqueKey &key, bool on, bool save)
        {
            auto it = _map.find(key);
            if (save)
            {
                if (it != _map.end()) _map.erase(it);
            }
            else
            {
                const bool original = it != _map.end() ? it->second.original : rt->get_technique_state(t);
                if (on == original)
                {
                    if (it != _map.end()) _map.erase(it);
                }
                else
                {
                    _map[key] = Entry{ original, on };
                }
            }
            ApplyingScope scope;
            rt->set_technique_state(t, on);
        }

        // Someone other than the bridge changed this technique: that state is the new baseline.
        void forget(const TechniqueKey &key) { _map.erase(key); }

        // Puts every overridden technique back to its original state (before a save or a preset switch).
        void revert(reshade::api::effect_runtime *rt) { visit(rt, false); }

        // Re-applies the overrides (after a save, a preset switch or a reload that restored the preset's states).
        void reapply(reshade::api::effect_runtime *rt) { visit(rt, true); }

        // After a preset switch the originals belong to the old preset; the next reapply that finds techniques re-reads
        // them from the new preset (and drops overrides that the new preset already matches).
        void mark_rebase() { _rebase_pending = true; }

    private:
        struct Entry { bool original; bool wanted; };
        struct Visit { TemporaryOverrides *self; bool wanted; bool rebase; bool any; TechniqueKey key; };

        void visit(reshade::api::effect_runtime *rt, bool wanted)
        {
            if (_map.empty())
                return;
            Visit v{ this, wanted, wanted && _rebase_pending, false, {} };
            ApplyingScope scope;
            rt->enumerate_techniques(nullptr, [](reshade::api::effect_runtime *r, reshade::api::effect_technique t, void *u) {
                auto *v = static_cast<Visit *>(u);
                try
                {
                    v->any = true;
                    read_key(r, t, v->key);
                    auto it = v->self->_map.find(v->key);
                    if (it == v->self->_map.end())
                        return;
                    if (v->rebase)
                    {
                        const bool current = r->get_technique_state(t);
                        if (current == it->second.wanted) { v->self->_map.erase(it); return; }
                        it->second.original = current;
                    }
                    r->set_technique_state(t, v->wanted ? it->second.wanted : it->second.original);
                }
                catch (...) {} // never unwind through ReShade's enumeration loop
            }, &v);
            if (v.rebase && v.any)
                _rebase_pending = false;
        }

        std::map<TechniqueKey, Entry> _map;
        bool _rebase_pending = false;
    };
}
