// Stellar ReShade bridge — "could ReShade have effect files to load?" check.
//
// ReShade 6.8.0 fires reshade_reloaded_effects when a reload STARTS: after the old effects are destroyed and before the
// new ones are looked up (full reload: source/runtime.cpp:3473-3490; single effect: 3449-3471). The technique list is
// empty at that point either way, so the event alone cannot tell "a load is starting" from "there is nothing to load".
// This check answers the second question the way ReShade's load_effects does (find_files over _effect_search_paths,
// source/runtime.cpp:242-290 and 3408-3414). It MUST NEVER under-report: a wrong "no files" would let the bridge change
// ReShade's config while compile workers read _effect_search_paths (source/runtime.cpp:1557). So every uncertainty
// answers "may have files" (the bridge then keeps waiting):
//
//   * Which paths. ReShade's load_config reads GENERAL/EffectSearchPaths from the runtime's config, falling back to the
//     global config (source/runtime.cpp:988-1002, 1015); otherwise the member keeps its previous value, initially ".\"
//     (source/runtime.cpp:300). get_config_value reads the SAME cached ini objects with the same fallback order here
//     (source/addon.cpp:43-48: runtime config = ini_file::load_cache(get_config_path()), else global_config()).
//     But the cache re-reads the file from disk when it changed (source/ini_file.cpp:28-31, 283-285), and ReShade only
//     re-runs load_config at construction (source/runtime.cpp:353), after set_config_value (source/addon.cpp:105-109)
//     and from its overlay's Reload button (source/runtime_gui.cpp:1992). So the config value can differ from what
//     ReShade loaded. The bridge therefore records the value at the moments ReShade provably just loaded it (see
//     record_* below) and answers "may have files" whenever the current value differs from that record.
//   * A key that exists with an empty value makes load_config use an empty list, while get_config_value reports it as
//     missing (source/addon.cpp:61-63, 77) and the bridge falls back further. ReShade then has no files at all, so any
//     answer is safe.
//   * Matching. ReShade compares the extension exactly with ".fx"/".addonfx" and skips directories
//     (source/runtime.cpp:272-276); the bridge matches case-insensitively and does not skip directories: a superset.
//   * Macros ('%...%', expanded by ReShade's resolve_path, source/runtime.cpp:38-108, 154), listing errors, more than
//     max_scanned_entries entries or more than max_scan_time: "may have files".
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <chrono>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include <reshade.hpp>

namespace stellar_rsb
{
    // Budget per check; beyond either the answer is "may have files". The check runs on the render thread inside
    // ReShade's reload start, so it is kept short.
    constexpr std::size_t max_scanned_entries = 20000;
    constexpr std::chrono::milliseconds max_scan_time{ 50 };

    namespace detail
    {
        inline bool is_effect_file(const std::filesystem::path &file)
        {
            std::wstring ext = file.extension().wstring();
            for (wchar_t &c : ext)
                c = static_cast<wchar_t>(std::towlower(c));
            return ext == L".fx" || ext == L".addonfx";
        }

        // Reads a ReShade config array ("a\0b\0"); returns false if the key is missing (or empty) in that config.
        inline bool read_config_array(reshade::api::effect_runtime *runtime, const char *key, std::vector<std::string> &out)
        {
            out.clear();
            size_t size = 0;
            if (!reshade::get_config_value(runtime, "GENERAL", key, nullptr, &size))
                return false;
            if (size == 0)
                return true;
            std::string buf(size, '\0');
            reshade::get_config_value(runtime, "GENERAL", key, &buf[0], &size);
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

        // EffectSearchPaths as load_config would read it now: runtime config, then global config, then the default.
        inline void read_effect_search_paths(reshade::api::effect_runtime *runtime, std::vector<std::string> &out)
        {
            if (!read_config_array(runtime, "EffectSearchPaths", out) && !read_config_array(nullptr, "EffectSearchPaths", out))
                out = { ".\\" };
        }

        struct LoadedSearchPaths
        {
            std::mutex lock;
            reshade::api::effect_runtime *owner = nullptr; // runtime the record belongs to; nullptr = no record
            std::vector<std::string> paths;
        };
        inline LoadedSearchPaths g_loaded_search_paths;

        // Returns true if the scan finished and found no effect file.
        inline bool scan_finds_nothing(const std::vector<std::string> &paths, const std::filesystem::path &base)
        {
            const auto deadline = std::chrono::steady_clock::now() + max_scan_time;
            size_t scanned = 0;
            const auto over_budget = [&]() {
                return ++scanned > max_scanned_entries || ((scanned & 63) == 0 && std::chrono::steady_clock::now() > deadline);
            };
            for (const std::string &entry : paths)
            {
                if (entry.find_first_of("%$<>") != std::string::npos)
                    return false; // macro or unusual syntax ReShade expands itself — do not guess

                std::filesystem::path dir = std::filesystem::u8path(entry);
                const bool recursive = dir.filename() == L"**";
                if (recursive)
                    dir.remove_filename();
                if (dir.is_relative())
                    dir = base / dir; // ReShade starts relative paths at its base path (source/runtime.cpp:158-159)

                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec))
                    continue; // ReShade skips paths that do not resolve (source/runtime.cpp:255-264)

                const auto options = std::filesystem::directory_options::skip_permission_denied;
                if (recursive)
                {
                    for (std::filesystem::recursive_directory_iterator it(dir, options, ec), end; !ec && it != end; it.increment(ec))
                        if (over_budget() || is_effect_file(it->path())) return false;
                }
                else
                {
                    for (std::filesystem::directory_iterator it(dir, options, ec), end; !ec && it != end; it.increment(ec))
                        if (over_budget() || is_effect_file(it->path())) return false;
                }
                if (ec)
                    return false; // could not finish the listing — do not claim "no files"
            }
            return true;
        }
    }

    // Call ONLY right after ReShade provably ran load_config for `runtime` on this thread: after the bridge's own
    // set_config_value (source/addon.cpp:105-109 calls load_config synchronously).
    inline void record_loaded_search_paths(reshade::api::effect_runtime *runtime) noexcept
    {
        try
        {
            std::vector<std::string> paths;
            detail::read_effect_search_paths(runtime, paths);
            std::lock_guard<std::mutex> lock(detail::g_loaded_search_paths.lock);
            detail::g_loaded_search_paths.owner = runtime;
            detail::g_loaded_search_paths.paths = std::move(paths);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(detail::g_loaded_search_paths.lock);
            detail::g_loaded_search_paths.owner = nullptr; // no record: every check answers "may have files"
        }
    }

    // At init_effect_runtime. A NEW runtime ran load_config in its constructor (source/runtime.cpp:353) and is
    // initialised right after it is created (source/dxgi/dxgi_swapchain.cpp:75-76 -> runtime_manager.cpp:64-67).
    // A re-init of the SAME runtime after a resize (on_init(true), source/dxgi/dxgi_swapchain.cpp:441 and others) does
    // not reload the config, so an existing record for that runtime is kept. If a new runtime reuses an old address the
    // old record is kept too; it then either equals the current config (exact) or differs ("may have files"): safe.
    inline void record_search_paths_at_init(reshade::api::effect_runtime *runtime) noexcept
    {
        {
            std::lock_guard<std::mutex> lock(detail::g_loaded_search_paths.lock);
            if (detail::g_loaded_search_paths.owner == runtime)
                return;
        }
        record_loaded_search_paths(runtime);
    }

    inline bool may_have_effect_files(reshade::api::effect_runtime *runtime)
    {
        try
        {
            std::vector<std::string> paths;
            detail::read_effect_search_paths(runtime, paths);
            {
                std::lock_guard<std::mutex> lock(detail::g_loaded_search_paths.lock);
                if (detail::g_loaded_search_paths.owner != runtime || detail::g_loaded_search_paths.paths != paths)
                    return true; // the config may not be what ReShade loaded
            }

            size_t base_size = 0;
            reshade::get_reshade_base_path(nullptr, &base_size);
            if (base_size <= 1)
                return true;
            std::string base_utf8(base_size, '\0');
            reshade::get_reshade_base_path(&base_utf8[0], &base_size); // = g_reshade_base_path (source/addon.cpp:25-41)
            base_utf8.resize(base_size);

            return !detail::scan_finds_nothing(paths, std::filesystem::u8path(base_utf8));
        }
        catch (...)
        {
            return true;
        }
    }
}
