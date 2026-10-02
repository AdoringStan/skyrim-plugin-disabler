#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
using json = nlohmann::json;

std::filesystem::path GetExecutablePath() {
  std::vector<wchar_t> buffer(260);
  while (true) {
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      throw std::runtime_error(
          "Could not determine the Skyrim executable path");
    }
    if (length < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), length));
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::filesystem::path GetLocalAppDataPath() {
  const auto required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) {
    throw std::runtime_error(
        "The LOCALAPPDATA environment variable is unavailable");
  }

  std::vector<wchar_t> buffer(required);
  const auto length =
      GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), required);
  if (length == 0 || length >= required) {
    throw std::runtime_error(
        "Could not read the LOCALAPPDATA environment variable");
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::string NormalizePluginName(std::string a_name) {
  const auto isWhitespace = [](unsigned char a_character) {
    return std::isspace(a_character) != 0;
  };
  const auto first =
      std::find_if_not(a_name.begin(), a_name.end(), isWhitespace);
  const auto last =
      std::find_if_not(a_name.rbegin(), a_name.rend(), isWhitespace).base();
  if (first >= last) {
    return {};
  }

  a_name = std::string(first, last);
  if (!a_name.empty() && a_name.front() == '*') {
    a_name.erase(a_name.begin());
  }
  const auto trimmedFirst =
      std::find_if_not(a_name.begin(), a_name.end(), isWhitespace);
  const auto trimmedLast =
      std::find_if_not(a_name.rbegin(), a_name.rend(), isWhitespace).base();
  if (trimmedFirst >= trimmedLast) {
    return {};
  }

  a_name = std::string(trimmedFirst, trimmedLast);
  std::transform(a_name.begin(), a_name.end(), a_name.begin(),
                 [](unsigned char a_character) {
                   return static_cast<char>(std::tolower(a_character));
                 });
  return a_name;
}

bool IsPluginFilename(const std::string &a_name) {
  if (a_name.empty() || a_name.find_first_of("/\\") != std::string::npos) {
    return false;
  }
  const auto extension = std::filesystem::path(a_name).extension().string();
  return extension == ".esp" || extension == ".esl" || extension == ".esm";
}

std::set<std::string>
ReadDisabledPlugins(const std::filesystem::path &a_configDirectory) {
  std::set<std::string> disabledPlugins;
  if (!std::filesystem::exists(a_configDirectory)) {
    logs::info("PluginDisabler config directory does not exist: {}",
               a_configDirectory.string());
    return disabledPlugins;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(a_configDirectory)) {
    if (!entry.is_regular_file() ||
        NormalizePluginName(entry.path().extension().string()) != ".json") {
      continue;
    }

    try {
      std::ifstream file(entry.path());
      if (!file) {
        logs::warn("Could not open config file: {}", entry.path().string());
        continue;
      }

      const auto config = json::parse(file);
      if (!config.is_array()) {
        logs::warn("Ignoring config that is not a JSON array: {}",
                   entry.path().string());
        continue;
      }

      for (const auto &value : config) {
        if (!value.is_string()) {
          logs::warn("Ignoring non-string entry in config: {}",
                     entry.path().string());
          continue;
        }

        const auto pluginName = NormalizePluginName(value.get<std::string>());
        if (IsPluginFilename(pluginName)) {
          disabledPlugins.insert(pluginName);
        } else {
          logs::warn("Ignoring invalid plugin filename in {}",
                     entry.path().string());
        }
      }
    } catch (const std::exception &error) {
      logs::error("Could not parse config {}: {}", entry.path().string(),
                  error.what());
    }
  }

  return disabledPlugins;
}

std::filesystem::path GetPluginsListPath() {
  const auto executable = GetExecutablePath().filename();
  const auto gameFolder =
      executable == L"SkyrimVR.exe" ? L"Skyrim VR" : L"Skyrim Special Edition";
  return GetLocalAppDataPath() / gameFolder / L"plugins.txt";
}

std::vector<std::filesystem::path>
GetPluginsListPaths(const std::filesystem::path &a_iniPath) {
  const auto defaultPath = GetPluginsListPath();
  std::vector<std::filesystem::path> paths{defaultPath};
  constexpr unsigned int maxConfiguredPaths = 64;
  std::vector<wchar_t> buffer(32768);
  bool foundConfiguredPath = false;
  for (unsigned int index = 1; index <= maxConfiguredPaths; ++index) {
    const auto key = L"PluginsTxtPath" + std::to_wstring(index);
    const auto length = GetPrivateProfileStringW(
        L"Paths", key.c_str(), L"", buffer.data(),
        static_cast<DWORD>(buffer.size()), a_iniPath.c_str());
    if (length == 0) {
      continue;
    }
    foundConfiguredPath = true;
    if (length >= buffer.size() - 1) {
      logs::error("{} is too long in {}; skipping this path",
                  std::filesystem::path(key).string(), a_iniPath.string());
      continue;
    }

    const std::filesystem::path configuredPath(
        std::wstring(buffer.data(), length));
    if (!configuredPath.is_absolute()) {
      logs::error("{} must be an absolute path in {}; skipping this path",
                  std::filesystem::path(key).string(), a_iniPath.string());
      continue;
    }

    const auto normalizedPath = configuredPath.lexically_normal();
    const auto alreadyIncluded =
        std::any_of(paths.begin(), paths.end(), [&](const auto &existingPath) {
          return existingPath.lexically_normal() == normalizedPath;
        });
    if (!alreadyIncluded) {
      logs::info("Using configured plugins.txt path from {}: {}",
                 std::filesystem::path(key).string(), configuredPath.string());
      paths.push_back(configuredPath);
    }
  }
  if (!foundConfiguredPath) {
    logs::info("No numbered PluginsTxtPath entries set in {}; using only the "
               "default path",
               a_iniPath.string());
  }
  return paths;
}

void DisableListedPlugins(const std::filesystem::path &a_pluginsListPath,
                          const std::set<std::string> &a_disabledPlugins) {
  std::ifstream input(a_pluginsListPath);
  if (!input) {
    logs::warn("Could not open plugins.txt: {}", a_pluginsListPath.string());
    return;
  }

  std::vector<std::string> lines;
  std::string line;
  std::size_t disabledCount = 0;
  while (std::getline(input, line)) {
    auto marker = line.find_first_not_of(" \t");
    if (marker != std::string::npos) {
      if (line.compare(marker, 3, "\xEF\xBB\xBF") == 0) {
        marker += 3;
      }
      if (marker < line.size() && line[marker] == '*') {
        const auto pluginName = NormalizePluginName(line.substr(marker + 1));
        if (a_disabledPlugins.contains(pluginName)) {
          line.erase(marker, 1);
          ++disabledCount;
        }
      }
    }
    lines.push_back(std::move(line));
  }
  if (input.bad()) {
    throw std::runtime_error("Could not read plugins.txt");
  }
  input.close();
  if (disabledCount == 0) {
    logs::info("No configured plugins were enabled in {}",
               a_pluginsListPath.string());
    return;
  }

  const auto temporaryPath =
      a_pluginsListPath.wstring() + L".PluginDisabler.tmp";
  {
    std::ofstream output(temporaryPath, std::ios::trunc);
    if (!output) {
      throw std::runtime_error("Could not create temporary plugins.txt");
    }
    for (const auto &outputLine : lines) {
      output << outputLine << '\n';
    }
    if (!output) {
      throw std::runtime_error("Could not write temporary plugins.txt");
    }
  }

  if (!MoveFileExW(temporaryPath.c_str(), a_pluginsListPath.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const auto errorCode = GetLastError();
    std::error_code cleanupError;
    std::filesystem::remove(temporaryPath, cleanupError);
    throw std::system_error(static_cast<int>(errorCode), std::system_category(),
                            "Could not replace plugins.txt at " +
                                a_pluginsListPath.string() +
                                " using temporary file " +
                                std::filesystem::path(temporaryPath).string());
  }
  logs::info("Disabled {} plugin(s) in {}", disabledCount,
             a_pluginsListPath.string());
}

void RunPluginDisabler(std::string_view a_phase) {
  try {
    const auto executablePath = GetExecutablePath();
    const auto configDirectory = executablePath.parent_path() / L"Data" /
                                 L"SKSE" / L"plugins" / L"PluginDisabler";
    const auto iniPath = executablePath.parent_path() / L"Data" / L"SKSE" /
                         L"plugins" / L"PluginDisabler.ini";
    const auto disabledPlugins = ReadDisabledPlugins(configDirectory);
    if (!disabledPlugins.empty()) {
      for (const auto &pluginsListPath : GetPluginsListPaths(iniPath)) {
        try {
          DisableListedPlugins(pluginsListPath, disabledPlugins);
        } catch (const std::exception &error) {
          logs::error("Failed processing plugins.txt at {}: {}",
                      pluginsListPath.string(), error.what());
        }
      }
    }
    logs::info("Loaded {} plugin name(s) from PluginDisabler config during {}",
               disabledPlugins.size(), a_phase);
  } catch (const std::exception &error) {
    logs::error("PluginDisabler failed during {}: {}", a_phase, error.what());
  }
}
} // namespace

SKSE_PLUGIN_PRELOAD(const SKSE::PreLoadInterface *a_skse) {
  SKSE::Init(a_skse);
  logs::info("SKSE_PLUGIN_PRELOAD");
  RunPluginDisabler("preload");
  return true;
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface *a_skse) {
  SKSE::Init(a_skse);
  logs::info("SKSE_PLUGIN_LOAD");

  const auto skseVersion = REL::Version::unpack(SKSE::GetSKSEVersion());
  logs::info("Detected SKSE version {}", skseVersion.string());
  if (skseVersion < REL::Version(2, 2, 7)) {
    logs::warn("SKSE {} does not support preload; using the normal load phase. "
               "If Skyrim has already read plugins.txt, changes apply next "
               "launch",
               skseVersion.string());
    RunPluginDisabler("normal load fallback");
  }
  return true;
}