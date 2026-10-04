// Stellar ReShade bridge — the config and preset files of the isolated effect runtime.
//
// ReShade's runtime reads its settings from its own config file and, for every key that file lacks, from the global
// ReShade.ini (load_config's config_get, source/runtime.cpp:983-1050 in ReShade 6.8.0). The isolated runtime therefore
// gets a config file that sets every key that must DIFFER from the game's runtime and copies the keys that decide which
// effects compile and how, so its effects match the game's:
//   * copied from the game runtime's config (falling back to the global config, the way ReShade reads them):
//     EffectSearchPaths, TextureSearchPaths, PreprocessorDefinitions, PerformanceMode, IntermediateCachePath,
//     NoEffectCache, NoDebugInfo;
//   * fixed: SkipLoadingDisabledEffects=1 (only effects with an enabled technique compile, runtime.cpp:1619-1636),
//     NoReloadOnInit=0 (the first present loads everything, runtime.cpp:3662-3664), PresetPath = the preset copy, and
//     StartupPresetPath empty (a startup preset would replace PresetPath, runtime.cpp:1037-1038);
//   * neutralised: no preset shortcuts, no shortcut keys, no gamepad navigation.
//
// The preset is a COPY of the game's current preset (so the isolated runtime can never write the user's preset), whose
// global-section Techniques / TechniqueSorting lines are replaced by the list the bridge computed from the game
// runtime's LIVE technique states. Uniform values and per-effect preprocessor definitions come from the copied file.
//
// ReShade's ini format: "key=value", arrays separated by ',' with ",," as an escaped comma (source/ini_file.cpp:72-110).
#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>
#include <reshade.hpp>
#include "effect_files.hpp"

namespace stellar_rsb
{
    namespace isolated_detail
    {
        inline std::string ini_escape(const std::string &value)
        {
            std::string out;
            out.reserve(value.size());
            for (char c : value)
            {
                out.push_back(c);
                if (c == ',')
                    out.push_back(',');
            }
            return out;
        }

        inline std::string ini_join(const std::vector<std::string> &values)
        {
            std::string out;
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i != 0)
                    out.push_back(',');
                out += ini_escape(values[i]);
            }
            return out;
        }

        inline bool read_file(const std::filesystem::path &path, std::string &out)
        {
            out.clear();
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return false;
            out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return !file.bad();
        }

        inline bool write_file(const std::filesystem::path &path, const std::string &text)
        {
            std::error_code ec;
            if (path.has_parent_path())
                std::filesystem::create_directories(path.parent_path(), ec); // an existing directory is not an error
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.flush();
            return file.good();
        }

        inline std::string trim(const std::string &s)
        {
            const size_t begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            const size_t end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }
    }

    // "<dir>/<stem>.preset.ini" next to the config file. ReShade accepts a preset path only with a .ini or .txt extension
    // and only if the file has a Techniques key (resolve_preset_path, source/runtime.cpp:169-179).
    inline std::filesystem::path isolated_preset_path(const std::filesystem::path &config_path)
    {
        std::filesystem::path name = config_path.stem();
        name += ".preset.ini";
        return config_path.parent_path() / name;
    }

    // Writes the isolated runtime's config file. `main` is the game's runtime (its config is the source of the copied keys).
    inline bool write_isolated_config(reshade::api::effect_runtime *main, const std::filesystem::path &config_path,
        const std::filesystem::path &preset_path)
    {
        using namespace isolated_detail;
        static const char *const copied_keys[] = {
            "EffectSearchPaths", "TextureSearchPaths", "PreprocessorDefinitions", "PerformanceMode",
            "IntermediateCachePath", "NoEffectCache", "NoDebugInfo",
        };

        std::string text = "[GENERAL]\n";
        std::vector<std::string> values;
        for (const char *key : copied_keys)
        {
            if (detail::read_config_array(main, key, values) || detail::read_config_array(nullptr, key, values))
                text += std::string(key) + '=' + ini_join(values) + '\n';
            // otherwise the key is absent here too and ReShade falls back to the global config, as for the game's runtime
        }
        text += "SkipLoadingDisabledEffects=1\n";
        text += "NoReloadOnInit=0\n";
        text += "PresetPath=" + ini_escape(preset_path.u8string()) + '\n';
        text += "StartupPresetPath=\n";
        text += "PresetShortcutKeys=\n";
        text += "PresetShortcutPaths=\n";
        text += "PresetTransitionDuration=0\n";
        text += "[INPUT]\n";
        text += "GamepadNavigation=0\n";
        text += "KeyEffects=0,0,0,0\n";
        text += "KeyReload=0,0,0,0\n";
        text += "KeyNextPreset=0,0,0,0\n";
        text += "KeyPreviousPreset=0,0,0,0\n";
        text += "KeyScreenshot=0,0,0,0\n";
        return write_file(config_path, text);
    }

    // Copies the preset at `source` to `dest` with the global-section Techniques and TechniqueSorting lines replaced.
    // `techniques` / `sorting` hold "Technique@Effect.fx" entries. A missing source preset gives a file with only those
    // two lines (ReShade then uses each effect's default values, as it would for a new preset).
    inline bool write_isolated_preset(const std::filesystem::path &source, const std::filesystem::path &dest,
        const std::vector<std::string> &techniques, const std::vector<std::string> &sorting)
    {
        using namespace isolated_detail;
        std::string original;
        read_file(source, original); // a preset that does not exist yet is not an error

        if (original.size() >= 3 && static_cast<unsigned char>(original[0]) == 0xEF &&
            static_cast<unsigned char>(original[1]) == 0xBB && static_cast<unsigned char>(original[2]) == 0xBF)
            original.erase(0, 3);

        std::string text = "Techniques=" + ini_join(techniques) + '\n';
        text += "TechniqueSorting=" + ini_join(sorting) + '\n';

        bool in_global_section = true;
        size_t start = 0;
        while (start < original.size())
        {
            size_t end = original.find('\n', start);
            const size_t next = end == std::string::npos ? original.size() : end + 1;
            const std::string line = original.substr(start, next - start);
            start = next;

            const std::string trimmed = trim(line);
            if (!trimmed.empty() && trimmed[0] == '[')
                in_global_section = false;
            if (in_global_section)
            {
                const size_t eq = trimmed.find('=');
                const std::string key = trim(eq == std::string::npos ? trimmed : trimmed.substr(0, eq));
                if (key == "Techniques" || key == "TechniqueSorting")
                    continue; // replaced above; ReShade would append duplicate keys to one list
            }
            text += line;
            if (line.empty() || line.back() != '\n')
                text.push_back('\n');
        }
        return write_file(dest, text);
    }
}
