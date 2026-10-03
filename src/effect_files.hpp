// Stellar ReShade bridge — "could ReShade have effect files to load?" check.
//
// ReShade 6.8.0 fires reshade_reloaded_effects both when a reload STARTS (after the old effects are destroyed, before the
// new ones are loaded) and when the last effect of a reload has been created. At the start event the technique list is
// empty either way, so the event alone cannot tell "a load is starting" from "there is nothing to load". This check
// answers the second question by looking for *.fx / *.addonfx files in ReShade's effect search paths, resolved the way
// ReShade resolves them (relative paths start at ReShade's base path; a trailing "**" means recursive).
//
// It is deliberately conservative: anything it cannot resolve with certainty (environment macros, errors, very large
// trees) answers "may have files", which keeps the bridge waiting instead of changing ReShade's state during a load.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#include <reshade.hpp>

namespace stellar_rsb
{
    namespace detail
    {
        inline bool is_effect_file(const std::filesystem::path &file)
        {
            std::wstring ext = file.extension().wstring();
            for (wchar_t &c : ext)
                c = static_cast<wchar_t>(std::towlower(c));
            return ext == L".fx" || ext == L".addonfx";
        }

        // Reads a ReShade config array ("a\0b\0"); returns false if the key does not exist in that config.
        inline bool read_config_array(reshade::api::effect_runtime *runtime, const char *key, std::vector<std::string> &out)
        {
            out.clear();
            size_t size = 0;
            if (!reshade::get_config_value(runtime, "GENERAL", key, nullptr, &size))
                return false;
            if (size == 0)
                return true; // exists, but empty
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
    }

    // Upper bound on directory entries examined per check; beyond it the answer is "may have files".
    constexpr std::size_t max_scanned_entries = 200000;

    inline bool may_have_effect_files(reshade::api::effect_runtime *runtime)
    {
        try
        {
            // Same lookup order as ReShade's load_config: the runtime's own config, then the global config, then the
            // built-in default (".\").
            std::vector<std::string> paths;
            if (!detail::read_config_array(runtime, "EffectSearchPaths", paths) &&
                !detail::read_config_array(nullptr, "EffectSearchPaths", paths))
                paths = { ".\\" };

            size_t base_size = 0;
            reshade::get_reshade_base_path(nullptr, &base_size);
            if (base_size <= 1)
                return true;
            std::string base_utf8(base_size, '\0');
            reshade::get_reshade_base_path(&base_utf8[0], &base_size);
            base_utf8.resize(base_size);
            const std::filesystem::path base = std::filesystem::u8path(base_utf8);

            size_t scanned = 0;
            for (const std::string &entry : paths)
            {
                if (entry.find_first_of("%$<>") != std::string::npos)
                    return true; // macro or unusual syntax ReShade expands itself — do not guess

                std::filesystem::path dir = std::filesystem::u8path(entry);
                const bool recursive = dir.filename() == L"**";
                if (recursive)
                    dir.remove_filename();
                if (dir.is_relative())
                    dir = base / dir;

                std::error_code ec;
                if (!std::filesystem::is_directory(dir, ec))
                    continue;

                if (recursive)
                {
                    for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
                         !ec && it != end; it.increment(ec))
                    {
                        if (++scanned > max_scanned_entries) return true;
                        if (detail::is_effect_file(it->path())) return true;
                    }
                }
                else
                {
                    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
                         !ec && it != end; it.increment(ec))
                    {
                        if (++scanned > max_scanned_entries) return true;
                        if (detail::is_effect_file(it->path())) return true;
                    }
                }
                if (ec)
                    return true; // could not finish the listing — do not claim "no files"
            }
            return false;
        }
        catch (...)
        {
            return true;
        }
    }
}
