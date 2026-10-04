// Stellar ReShade bridge — uniform overrides: keep chosen effect values fixed whatever preset is loaded.
//
// ReShade resets every uniform to its shader default whenever an effect is (re)loaded (load_effect, source/runtime.cpp:
// 1841-1842 in ReShade 6.8.0) and again before applying a preset (load_current_preset, runtime.cpp:1270-1272); a key the
// preset file lacks keeps that default (runtime.cpp:1280-1292). So a value the host needs (for example a "mask UI" switch
// that must be off in this game) cannot be guaranteed through preset files alone.
//
// The bridge stores (effect file, variable) -> value and applies it on EVERY effect runtime (the game's and the isolated
// one) inside reshade_begin_effects, i.e. right before techniques are drawn, so a value that a load just reset never
// reaches a drawn frame. Applying is skipped unless something may have changed it: the override list changed, an effect
// (re)load finished (reshade_reloaded_effects), a preset was applied (reshade_set_current_preset_path), or a uniform
// with an overridden name was set (reshade_set_uniform_value — ReShade raises it for preset loads and overlay edits while
// not loading, runtime.cpp:4676); plus a re-check every `recheck_frames` frames as a safety net. A value is written only
// when it differs, so a steady state costs one comparison per overridden variable on a re-check.
//
// The value is a list of numbers separated by ',' ("0", "1", "0.5,0.25,1"); "true" / "false" are accepted. It is
// converted to the variable's type (bool: non-zero; int/uint: rounded; float). Fewer numbers than components set only
// the first ones; arrays get element 0. ReShade itself saves the overridden value into a preset whenever it saves one.
#pragma once

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <reshade.hpp>
#include "overrides.hpp"

namespace stellar_rsb
{
    constexpr int uniform_recheck_frames = 120;
    constexpr size_t uniform_max_components = 16;

    struct UniformOverrideValue
    {
        std::vector<double> numbers;
    };

    using UniformKey = std::pair<std::string, std::string>; // (effect file name, "" = any; variable name)
    using UniformOverrideMap = std::map<UniformKey, UniformOverrideValue>;

    // Bumped by anything that may make an applied value stale. Read on the render thread.
    inline std::atomic<uint64_t> g_uniform_epoch{ 1 };

    class UniformOverrideStore
    {
    public:
        // Returns false if `text` is not a list of 1..16 numbers.
        static bool parse(const char *text, UniformOverrideValue &out)
        {
            out.numbers.clear();
            if (text == nullptr)
                return false;
            const std::string s(text);
            size_t start = 0;
            while (start <= s.size())
            {
                size_t end = s.find(',', start);
                if (end == std::string::npos) end = s.size();
                std::string token = s.substr(start, end - start);
                const size_t b = token.find_first_not_of(" \t");
                const size_t e = token.find_last_not_of(" \t");
                token = b == std::string::npos ? std::string() : token.substr(b, e - b + 1);
                if (token.empty())
                    return false;
                std::string lower = token;
                for (char &c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                double value = 0;
                if (lower == "true")
                    value = 1;
                else if (lower == "false")
                    value = 0;
                else
                {
                    char *stop = nullptr;
                    value = std::strtod(token.c_str(), &stop);
                    if (stop == token.c_str() || *stop != '\0' || !std::isfinite(value))
                        return false;
                }
                out.numbers.push_back(value);
                if (out.numbers.size() > uniform_max_components)
                    return false;
                start = end + 1;
            }
            return !out.numbers.empty();
        }

        bool set(const std::string &effect, const std::string &variable, const char *value)
        {
            if (variable.empty())
                return false;
            std::lock_guard<std::mutex> lock(_lock);
            if (value == nullptr || *value == '\0')
            {
                _map.erase(UniformKey{ effect, variable });
                rebuild_names();
                _version++;
                g_uniform_epoch++;
                return true;
            }
            UniformOverrideValue parsed;
            if (!parse(value, parsed))
                return false;
            _map[UniformKey{ effect, variable }] = std::move(parsed);
            rebuild_names();
            _version++;
            g_uniform_epoch++;
            return true;
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(_lock);
            _map.clear();
            _names.clear();
            _version++;
            g_uniform_epoch++;
        }

        bool empty()
        {
            std::lock_guard<std::mutex> lock(_lock);
            return _map.empty();
        }

        bool has_name(const std::string &variable)
        {
            std::lock_guard<std::mutex> lock(_lock);
            return _names.count(variable) != 0;
        }

        // Copies the map into `out` if it changed since `version`.
        void copy_if_changed(UniformOverrideMap &out, uint64_t &version)
        {
            std::lock_guard<std::mutex> lock(_lock);
            if (version == _version)
                return;
            out = _map;
            version = _version;
        }

    private:
        void rebuild_names()
        {
            _names.clear();
            for (const auto &entry : _map)
                _names.insert(entry.first.second);
        }

        std::mutex _lock;
        UniformOverrideMap _map;
        std::set<std::string> _names;
        uint64_t _version = 1;
    };

    inline UniformOverrideStore g_uniform_overrides;

    namespace uniform_detail
    {
        struct RuntimeState
        {
            uint64_t applied_epoch = 0;
            int frames_since_check = 0;
        };

        // Render thread only (ReShade events).
        inline std::map<reshade::api::effect_runtime *, RuntimeState> g_runtime_states;
        inline UniformOverrideMap g_cached;
        inline uint64_t g_cached_version = 0;

        struct Apply
        {
            const UniformOverrideMap *map;
            std::string name, effect;
        };

        template <typename T, typename Get, typename Set>
        inline void write_if_different(const std::vector<double> &numbers, size_t count, Get get, Set set, T (*convert)(double))
        {
            T current[uniform_max_components] = {};
            T wanted[uniform_max_components] = {};
            get(current, count);
            bool differs = false;
            for (size_t i = 0; i < count; ++i)
            {
                wanted[i] = convert(numbers[i]);
                differs |= !(current[i] == wanted[i]);
            }
            if (differs)
                set(wanted, count);
        }

        inline bool to_bool(double v) { return v != 0; }
        inline float to_float(double v) { return static_cast<float>(v); }
        inline int32_t to_int(double v) { return static_cast<int32_t>(std::llround(v)); }
        inline uint32_t to_uint(double v) { return v <= 0 ? 0u : static_cast<uint32_t>(std::llround(v)); }

        inline void apply_one(reshade::api::effect_runtime *rt, reshade::api::effect_uniform_variable v, const UniformOverrideValue &value)
        {
            reshade::api::format base = reshade::api::format::unknown;
            uint32_t rows = 0, columns = 0, array_length = 0;
            rt->get_uniform_variable_type(v, &base, &rows, &columns, &array_length);
            size_t components = static_cast<size_t>(rows == 0 ? 1 : rows) * static_cast<size_t>(columns == 0 ? 1 : columns);
            if (components > uniform_max_components)
                components = uniform_max_components;
            const size_t count = value.numbers.size() < components ? value.numbers.size() : components;
            if (count == 0)
                return;

            switch (base)
            {
            case reshade::api::format::r32_typeless: // bool
                write_if_different<bool>(value.numbers, count,
                    [&](bool *out, size_t n) { rt->get_uniform_value_bool(v, out, n); },
                    [&](const bool *in, size_t n) { rt->set_uniform_value_bool(v, in, n); }, &to_bool);
                break;
            case reshade::api::format::r32_sint:
            case reshade::api::format::r16_sint:
                write_if_different<int32_t>(value.numbers, count,
                    [&](int32_t *out, size_t n) { rt->get_uniform_value_int(v, out, n); },
                    [&](const int32_t *in, size_t n) { rt->set_uniform_value_int(v, in, n); }, &to_int);
                break;
            case reshade::api::format::r32_uint:
            case reshade::api::format::r16_uint:
                write_if_different<uint32_t>(value.numbers, count,
                    [&](uint32_t *out, size_t n) { rt->get_uniform_value_uint(v, out, n); },
                    [&](const uint32_t *in, size_t n) { rt->set_uniform_value_uint(v, in, n); }, &to_uint);
                break;
            case reshade::api::format::r32_float:
            case reshade::api::format::r16_float:
                write_if_different<float>(value.numbers, count,
                    [&](float *out, size_t n) { rt->get_uniform_value_float(v, out, n); },
                    [&](const float *in, size_t n) { rt->set_uniform_value_float(v, in, n); }, &to_float);
                break;
            default:
                break; // unknown type: leave it alone
            }
        }

        // Applies every override to `rt`. Must run while `rt` is not loading (uniforms are listed only then).
        inline void apply_all(reshade::api::effect_runtime *rt, const UniformOverrideMap &map)
        {
            if (map.empty())
                return;
            Apply a{ &map, {}, {} };
            rt->enumerate_uniform_variables(nullptr, [](reshade::api::effect_runtime *r, reshade::api::effect_uniform_variable v, void *u) {
                try
                {
                    auto *a = static_cast<Apply *>(u);
                    read_reshade_string(a->name, [&](char *b, size_t *n) { r->get_uniform_variable_name(v, b, n); });
                    read_reshade_string(a->effect, [&](char *b, size_t *n) { r->get_uniform_variable_effect_name(v, b, n); });
                    auto it = a->map->find(UniformKey{ a->effect, a->name });
                    if (it == a->map->end())
                        it = a->map->find(UniformKey{ std::string(), a->name });
                    if (it != a->map->end())
                        apply_one(r, v, it->second);
                }
                catch (...) {} // never unwind through ReShade's enumeration loop
            }, &a);
        }
    }

    // reshade_begin_effects (any runtime, render thread): apply the overrides if anything may have changed them.
    inline void uniform_overrides_before_effects(reshade::api::effect_runtime *rt) noexcept
    {
        try
        {
            using namespace uniform_detail;
            g_uniform_overrides.copy_if_changed(g_cached, g_cached_version);
            RuntimeState &state = g_runtime_states[rt];
            const uint64_t epoch = g_uniform_epoch.load();
            const bool recheck = ++state.frames_since_check >= uniform_recheck_frames;
            if (epoch == state.applied_epoch && !recheck)
                return;
            state.applied_epoch = epoch; // read BEFORE applying: our own writes bump the epoch once more, which re-checks
            state.frames_since_check = 0;
            apply_all(rt, g_cached);
        }
        catch (...) {}
    }

    inline void uniform_overrides_forget(reshade::api::effect_runtime *rt) noexcept
    {
        try { uniform_detail::g_runtime_states.erase(rt); } catch (...) {}
    }

    // reshade_set_uniform_value: a load or an overlay edit may have replaced an overridden value.
    inline void uniform_overrides_on_set(reshade::api::effect_runtime *rt, reshade::api::effect_uniform_variable v) noexcept
    {
        try
        {
            if (g_uniform_overrides.empty())
                return;
            std::string name;
            read_reshade_string(name, [&](char *b, size_t *n) { rt->get_uniform_variable_name(v, b, n); });
            if (g_uniform_overrides.has_name(name))
                g_uniform_epoch++;
        }
        catch (...) {}
    }
}
