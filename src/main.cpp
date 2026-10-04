// Windows APIs are used for process paths, environment variables, INI reads,
// file replacement, and SKSE's Windows message callback environment.
#include <Windows.h>

// nlohmann::json turns each config file's JSON text into C++ values we can
// validate and loop over.
#include <nlohmann/json.hpp>

// Standard-library helpers for searching, character handling, paths, files,
// collections, errors, and strings.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
// Give the JSON library's full type name a short local alias.
using json = nlohmann::json;

// Ask Windows for the running game's executable path. The buffer starts at a
// common path size and grows if the full path does not fit.
std::filesystem::path GetExecutablePath() {
  std::vector<wchar_t> buffer(260);
  while (true) {
    const auto length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      // Throwing lets the caller's surrounding try/catch log the failure.
      throw std::runtime_error(
          "Could not determine the Skyrim executable path");
    }
    if (length < buffer.size()) {
      // A wide string is used because Windows paths may contain Unicode.
      return std::filesystem::path(std::wstring(buffer.data(), length));
    }
    // The returned length filled the buffer, so retry with more space.
    buffer.resize(buffer.size() * 2);
  }
}

// Read the user's Windows Local AppData path. The first API call asks how many
// wchar_t slots are required; the second call copies the actual value.
std::filesystem::path GetLocalAppDataPath() {
  const auto requiredBufferSize =
      GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (requiredBufferSize == 0) {
    throw std::runtime_error(
        "The LOCALAPPDATA environment variable is unavailable");
  }

  std::vector<wchar_t> buffer(requiredBufferSize);
  const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                              requiredBufferSize);
  if (length == 0 || length >= requiredBufferSize) {
    throw std::runtime_error(
        "Could not read the LOCALAPPDATA environment variable");
  }
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

// Remove only surrounding whitespace while preserving filename case. This is
// important on Proton/Linux, where the real filesystem can be case-sensitive:
// comparisons use normalized names, but opening a file needs its actual name.
std::string TrimPluginName(std::string a_name) {
  const auto isWhitespace = [](unsigned char a_character) {
    return std::isspace(a_character) != 0;
  };
  const auto first =
      std::find_if_not(a_name.begin(), a_name.end(), isWhitespace);
  const auto last =
      std::find_if_not(a_name.rbegin(), a_name.rend(), isWhitespace).base();
  return first < last ? std::string(first, last) : std::string{};
}

// Convert plugin names into a consistent comparison key. This trims outer
// whitespace, tolerates a leading plugins.txt enabled marker '*', trims again,
// and lowercases the result. Returning a copy allows the caller to normalize
// its string without changing the original spelling used for display/logging.
std::string NormalizePluginName(std::string a_name) {
  a_name = TrimPluginName(std::move(a_name));
  if (!a_name.empty() && a_name.front() == '*') {
    a_name.erase(a_name.begin());
  }
  a_name = TrimPluginName(std::move(a_name));
  if (a_name.empty()) {
    return {};
  }

  std::transform(a_name.begin(), a_name.end(), a_name.begin(),
                 [](unsigned char a_character) {
                   return static_cast<char>(std::tolower(a_character));
                 });
  return a_name;
}

// Config entries must be a filename, not a path, and must use a supported
// Skyrim plugin extension. NormalizePluginName has already lowercased it, so
// these extension comparisons are case-insensitive in effect.
bool IsPluginFilename(const std::string &a_name) {
  if (a_name.empty() || a_name.find_first_of("/\\") != std::string::npos) {
    return false;
  }
  const auto extension = std::filesystem::path(a_name).extension().string();
  return extension == ".esp" || extension == ".esl" || extension == ".esm";
}

// The next few functions are necesarry because the plugins are not loaded into
// game-data yet, and CLib functions for reading plugin data only work on
// plugins loaded into game-data.

// TES4 records store numeric fields in
// little-endian byte order. These helpers combine the individual bytes into
// ordinary C++ integer values.
std::uint16_t ReadUInt16LE(const unsigned char *a_data) {
  return static_cast<std::uint16_t>(a_data[0]) |
         static_cast<std::uint16_t>(a_data[1] << 8);
}

std::uint32_t ReadUInt32LE(const unsigned char *a_data) {
  return static_cast<std::uint32_t>(a_data[0]) |
         (static_cast<std::uint32_t>(a_data[1]) << 8) |
         (static_cast<std::uint32_t>(a_data[2]) << 16) |
         (static_cast<std::uint32_t>(a_data[3]) << 24);
}

// Read the plugin's TES4 file header and return the filenames in its MAST
// subrecords. A value is returned even for a plugin with no masters (an empty
// set); nullopt means the file/header was unavailable or unsupported, so the
// caller cannot safely assume it has no dependencies.
std::optional<std::set<std::string>>
ReadPluginMasters(const std::filesystem::path &a_pluginPath) {
  std::ifstream file(a_pluginPath, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  std::array<unsigned char, 24> recordHeader{};
  file.read(reinterpret_cast<char *>(recordHeader.data()), recordHeader.size());
  if (file.gcount() != static_cast<std::streamsize>(recordHeader.size()) ||
      std::string_view(reinterpret_cast<const char *>(recordHeader.data()),
                       4) != "TES4") {
    return std::nullopt;
  }

  // The TES4 record header is 24 bytes. Its flags indicate whether the record
  // data is compressed; compressed data is intentionally rejected because
  // this small reader does not implement Bethesda's compression format.
  constexpr std::uint32_t compressedFlag = 0x00040000;
  const auto recordFlags = ReadUInt32LE(recordHeader.data() + 8);
  const auto recordSize = ReadUInt32LE(recordHeader.data() + 4);
  constexpr std::uint32_t maxHeaderSize = 64 * 1024 * 1024;
  if ((recordFlags & compressedFlag) != 0 || recordSize > maxHeaderSize) {
    return std::nullopt;
  }

  // Read exactly the TES4 record payload. The size cap also avoids allocating
  // an unreasonable amount of memory if a file is malformed.
  std::vector<unsigned char> recordData(recordSize);
  if (recordSize > 0) {
    file.read(reinterpret_cast<char *>(recordData.data()), recordSize);
    if (file.gcount() != static_cast<std::streamsize>(recordSize)) {
      return std::nullopt;
    }
  }

  std::set<std::string> masters;
  // XXXX is Bethesda's extended-size marker: its 4-byte value supplies the
  // size of the following subrecord when the normal 16-bit size is too small.
  std::optional<std::uint32_t> extendedSize;
  std::size_t offset = 0;
  while (offset < recordData.size()) {
    // Every normal subrecord needs a 4-byte type and a 2-byte size.
    if (recordData.size() - offset < 6) {
      return std::nullopt;
    }

    const std::string_view subrecordType(
        reinterpret_cast<const char *>(recordData.data() + offset), 4);
    const auto subrecordSize = ReadUInt16LE(recordData.data() + offset + 4);
    offset += 6;

    if (subrecordType == "XXXX") {
      // XXXX itself must contain exactly one 32-bit size value.
      if (subrecordSize != 4 || recordData.size() - offset < 4) {
        return std::nullopt;
      }
      extendedSize = ReadUInt32LE(recordData.data() + offset);
      offset += 4;
      continue;
    }

    const auto payloadSize = extendedSize.value_or(subrecordSize);
    extendedSize.reset();
    if (payloadSize > recordData.size() - offset) {
      return std::nullopt;
    }

    if (subrecordType == "MAST") {
      // MAST payloads contain a NUL-terminated master filename. The paired
      // DATA subrecord that follows is not needed for dependency checking.
      const auto *nameData = recordData.data() + offset;
      const auto *terminator = std::find(nameData, nameData + payloadSize, 0);
      if (terminator == nameData + payloadSize) {
        return std::nullopt;
      }
      const std::string masterName(
          reinterpret_cast<const char *>(nameData),
          static_cast<std::size_t>(terminator - nameData));
      const auto normalizedMaster = NormalizePluginName(masterName);
      if (IsPluginFilename(normalizedMaster)) {
        masters.insert(normalizedMaster);
      }
    }
    offset += payloadSize;
  }

  if (extendedSize) {
    return std::nullopt;
  }
  return masters;
}

// Summarize a run so later stages can both present a useful AppData count and
// verify exactly which entries were successfully changed after Skyrim loads.
struct DisableSummary {
  std::size_t appDataDisabledEntries{};
  std::set<std::string> pluginsToVerify;
};

DisableSummary preloadSummary;
DisableSummary pendingSummary;
bool forceTerminateOnVerificationFailure{};

// Read all JSON arrays in the config directory and merge their plugin names.
// A set naturally removes duplicates. Names are normalized before insertion
// into the set.
std::set<std::string>
ReadDisabledPlugins(const std::filesystem::path &a_configDirectory) {
  std::set<std::string> disabledPlugins;
  if (!std::filesystem::exists(a_configDirectory)) {
    // A missing folder is a valid "nothing configured" state, not a fatal
    // error; the caller will receive an empty set and skip file edits.
    logs::info("PluginDisabler config directory does not exist: {}",
               a_configDirectory.string());
    return disabledPlugins;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(a_configDirectory)) {

    // skip if not a json file
    if (!entry.is_regular_file() ||
        NormalizePluginName(entry.path().extension().string()) != ".json") {
      continue;
    }

    try {
      // ifstream opens this individual JSON file for reading. Its destructor
      // closes it automatically when this loop iteration leaves scope.
      std::ifstream file(entry.path());
      if (!file) {
        logs::warn("Could not open config file: {}", entry.path().string());
        continue;
      }

      const auto config = json::parse(file);
      // The config format is deliberately a top-level JSON array of strings.
      if (!config.is_array()) {
        logs::warn("Ignoring config that is not a JSON array: {}",
                   entry.path().string());
        continue;
      }

      for (const auto &value : config) {
        // Ignore malformed individual array items instead of rejecting every
        // otherwise-valid plugin name in the same file.
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
      // A broken config file should be visible in the log but should not stop
      // other JSON files from being processed.
      logs::error("Could not parse config {}: {}", entry.path().string(),
                  error.what());
    }
  }

  return disabledPlugins;
}

// Build the game's standard load-order path under Local AppData. Skyrim VR
// uses a different folder name from flat Skyrim SE/AE.
std::filesystem::path GetPluginsListPath() {
  const auto executable = GetExecutablePath().filename();
  const auto gameFolder =
      executable == L"SkyrimVR.exe" ? L"Skyrim VR" : L"Skyrim Special Edition";
  logs::info("{} detected", gamefolder.string());
  return GetLocalAppDataPath() / gameFolder / L"plugins.txt";
}

// PluginDisabler.ini lives next to the SKSE plugin config directory in Data.
std::filesystem::path GetPluginDisablerIniPath() {
  return GetExecutablePath().parent_path() / L"Data" / L"SKSE" / L"plugins" /
         L"PluginDisabler.ini";
}

// The Windows INI API returns the supplied default when the file or key is
// missing. The bundled config may override this default explicitly.
bool ForceTerminateIfPluginsStillPresent() {
  const auto iniPath = GetPluginDisablerIniPath();
  return GetPrivateProfileIntW(L"General",
                               L"ForceTerminateIfPluginsStillPresent", 1,
                               iniPath.c_str()) == 1;
}

// Return every plugins.txt file this run should update. The standard AppData
// file is always first; numbered INI entries add mod manager files. Keeping
// the default first lets RunPluginDisabler use index zero for its user-facing
// AppData count. Lexical normalization prevents obvious duplicate paths.
std::vector<std::filesystem::path>
GetPluginsListPaths(const std::filesystem::path &a_iniPath) {
  const auto defaultPath = GetPluginsListPath();
  std::vector<std::filesystem::path> paths{defaultPath};
  constexpr unsigned int maxConfiguredPaths = 64;
  std::vector<wchar_t> buffer(32768);
  bool foundConfiguredPath = false;
  for (unsigned int index = 1; index <= maxConfiguredPaths; ++index) {
    // INI files do not have arrays, so numbered keys represent a list.
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
    // Require a complete path so its meaning does not depend on Skyrim's
    // current working directory.
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

// Process one plugins.txt and return the number of enabled entries actually
// changed. a_changedPlugins receives names only after the replacement succeeds
// so the later in-game verification checks successful edits, not intentions.
std::size_t DisableListedPlugins(const std::filesystem::path &a_pluginsListPath,
                                 const std::set<std::string> &a_disabledPlugins,
                                 std::set<std::string> &a_changedPlugins) {
  std::ifstream input(a_pluginsListPath);
  if (!input) {
    logs::info(
        "----------------------------------------------------------------");
    logs::warn("Could not open plugins.txt: {}", a_pluginsListPath.string());
    return 0;
  }

  std::vector<std::string> lines;
  // Keep the original lines so non-target entries and their ordering remain
  // unchanged when the rewritten file is produced.
  std::map<std::string, std::string> enabledPluginNames;
  std::string line;
  while (std::getline(input, line)) {
    auto marker = line.find_first_not_of(" \t");
    if (marker != std::string::npos) {
      if (line.compare(marker, 3, "\xEF\xBB\xBF") == 0) {
        marker += 3;
      }
      if (marker < line.size() && line[marker] == '*') {
        const auto displayName = line.substr(marker + 1);
        enabledPluginNames.try_emplace(NormalizePluginName(displayName),
                                       displayName);
      }
    }
    lines.push_back(std::move(line));
  }
  if (input.bad()) {
    throw std::runtime_error("Could not read plugins.txt");
  }
  input.close();

  // Only entries marked enabled ('*') and named in the JSON configs are
  // candidates. Already-disabled or absent entries do not need editing.
  std::set<std::string> enabledTargets;
  for (const auto &[normalizedName, displayName] : enabledPluginNames) {
    if (a_disabledPlugins.contains(normalizedName)) {
      enabledTargets.insert(normalizedName);
    }
  }
  if (enabledTargets.empty()) {
    logs::info("No configured plugins were enabled in {}",
               a_pluginsListPath.string());
    return 0;
  }

  std::map<std::string, std::set<std::string>> pluginMasters;
  std::vector<std::string> unreadablePluginHeaders;
  // Skyrim plugin files are stored beside the game executable under Data.
  const auto dataDirectory = GetExecutablePath().parent_path() / L"Data";
  for (const auto &[normalizedName, displayName] : enabledPluginNames) {
    const auto pluginHeaderPath =
        dataDirectory / std::filesystem::path(TrimPluginName(displayName));
    const auto masters = ReadPluginMasters(pluginHeaderPath);
    if (!masters) {
      unreadablePluginHeaders.push_back(displayName);
      logs::error("Cannot read dependency header {} for enabled plugin {} "
                  "while processing {}",
                  pluginHeaderPath.string(), displayName,
                  a_pluginsListPath.string());
      continue;
    }
    pluginMasters.emplace(normalizedName, *masters);
  }

  if (!unreadablePluginHeaders.empty()) {
    // Fail closed for this load-order file: an unreadable enabled plugin might
    // depend on any requested target, so editing would risk breaking it.
    logs::warn("No changes made to {}: {} enabled plugin header(s) could not "
               "be inspected; see preceding log entries",
               a_pluginsListPath.string(), unreadablePluginHeaders.size());
    return 0;
  }

  std::set<std::string> blockedTargets;
  std::map<std::string, std::set<std::string>> blockingDependents;
  // Find the dependency closure of targets that must remain enabled. A
  // targeted plugin is initially expected to be disabled. Any non-target
  // plugin remains enabled, so it protects each targeted master it requires.
  // If a target becomes protected, it too remains enabled and may protect its
  // own masters; repeat until no new target is blocked.
  bool foundNewlyBlockedTarget = true;
  while (foundNewlyBlockedTarget) {
    foundNewlyBlockedTarget = false;
    // check if a plugin will be disabled - if so, we don't need to check its
    // masters so continue the loop.
    for (const auto &[dependentName, masters] : pluginMasters) {
      const bool dependentWillRemainEnabled =
          !enabledTargets.contains(dependentName) ||
          blockedTargets.contains(dependentName);
      if (!dependentWillRemainEnabled) {
        continue;
      }

      for (const auto &masterName : masters) {
        // check if plugins to be disable contains the current master file. If
        // so, block that plugin from being disabled. If the blocked target
        // being added hasn't been added before (eg, from another plugin
        // requiring it), add it to the blocking dependents list. If it has been
        // added before, add the blocking dependent to the existing list.
        if (enabledTargets.contains(masterName) &&
            blockedTargets.insert(masterName).second) {
          blockingDependents[masterName].insert(dependentName);
          foundNewlyBlockedTarget = true;
        } else if (blockedTargets.contains(masterName)) {
          blockingDependents[masterName].insert(dependentName);
        }
      }
    }
  }

  logs::info(
      "----------------------------------------------------------------");
  for (const auto &[masterName, dependentNames] : blockingDependents) {
    for (const auto &dependentName : dependentNames) {
      logs::warn("Not disabling {} in {} because enabled plugin {} depends "
                 "on it",
                 enabledPluginNames.at(masterName), a_pluginsListPath.string(),
                 enabledPluginNames.at(dependentName));
    }
  }

  auto safeTargets = enabledTargets;
  for (const auto &blockedTarget : blockedTargets) {
    safeTargets.erase(blockedTarget);
  }
  if (safeTargets.empty()) {
    logs::warn("No configured plugins were disabled in {} because each is a "
               "master of an enabled plugin",
               a_pluginsListPath.string());
    return 0;
  }

  // Remove activation markers only for targets not protected by a dependent.
  // We still rewrite from the saved lines rather than editing the source while
  // reading, so a read error cannot leave a partially rewritten load order.
  std::vector<std::string> disabledPluginNames;
  std::size_t disabledCount = 0;
  for (auto &pluginLine : lines) {
    auto marker = pluginLine.find_first_not_of(" \t");
    if (marker == std::string::npos) {
      continue;
    }
    if (pluginLine.compare(marker, 3, "\xEF\xBB\xBF") == 0) {
      marker += 3;
    }
    if (marker >= pluginLine.size() || pluginLine[marker] != '*') {
      continue;
    }

    const auto pluginName = NormalizePluginName(pluginLine.substr(marker + 1));
    if (safeTargets.contains(pluginName)) {
      disabledPluginNames.push_back(pluginLine.substr(marker + 1));
      pluginLine.erase(marker, 1);
      ++disabledCount;
    }
  }

  const auto temporaryPath =
      a_pluginsListPath.wstring() + L".PluginDisabler.tmp";
  // Write to a sibling temporary first. MoveFileExW below replaces the target
  // only after the complete output has been written successfully.
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
  logs::info("Disabled {} plugin(s) in {}:", disabledCount,
             a_pluginsListPath.string());
  for (const auto &pluginName : disabledPluginNames) {
    logs::info("  {}", pluginName);
    a_changedPlugins.insert(NormalizePluginName(pluginName));
  }
  return disabledCount;
}

// Run the common config and file-edit work for either SKSE phase. Each path is
// handled independently so one bad manager profile does not block the others.
DisableSummary RunPluginDisabler(std::string_view a_phase) {
  DisableSummary summary;
  try {
    const auto executablePath = GetExecutablePath();
    const auto configDirectory = executablePath.parent_path() / L"Data" /
                                 L"SKSE" / L"plugins" / L"PluginDisabler";
    const auto iniPath = GetPluginDisablerIniPath();
    const auto disabledPlugins = ReadDisabledPlugins(configDirectory);
    if (!disabledPlugins.empty()) {
      const auto pluginsListPaths = GetPluginsListPaths(iniPath);
      for (std::size_t index = 0; index < pluginsListPaths.size(); ++index) {
        const auto &pluginsListPath = pluginsListPaths[index];
        try {
          std::set<std::string> changedPlugins;
          const auto disabledEntries = DisableListedPlugins(
              pluginsListPath, disabledPlugins, changedPlugins);
          if (index == 0) {
            summary.appDataDisabledEntries = disabledEntries;
          }
          summary.pluginsToVerify.insert(changedPlugins.begin(),
                                         changedPlugins.end());
        } catch (const std::exception &error) {
          logs::error("Failed processing plugins.txt at {}: {}",
                      pluginsListPath.string(), error.what());
        }
      }
      // seprator for log
      logs::info(
          "----------------------------------------------------------------");
    }
    logs::info("Loaded {} plugin name(s) from PluginDisabler config during {}",
               disabledPlugins.size(), a_phase);
  } catch (const std::exception &error) {
    logs::error("PluginDisabler failed during {}: {}", a_phase, error.what());
  }
  return summary;
}

bool TerminateGameProcess() {
  // This is an explicit user choice from the mismatch prompt. Flush first so
  // the reason for the forced exit is persisted before the process ends.
  logs::warn("Force-terminating Skyrim at the user's request");
  spdlog::default_logger()->flush();
  if (!TerminateProcess(GetCurrentProcess(), 0)) {
    const auto errorCode = GetLastError();
    logs::error("Could not terminate the Skyrim process: {}", errorCode);
    return false;
  }
  return true;
}

void ShowDisableSummary(const DisableSummary &a_summary,
                        const std::set<std::string> &a_stillLoadedPlugins,
                        bool a_forceTerminateOnAcknowledge) {
  if (a_summary.appDataDisabledEntries == 0 &&
      a_summary.pluginsToVerify.empty()) {
    return;
  }

  std::wstring message;
  if (a_summary.appDataDisabledEntries > 0) {
    message = std::to_wstring(a_summary.appDataDisabledEntries) +
              L" plugin entr" +
              (a_summary.appDataDisabledEntries == 1 ? L"y" : L"ies") +
              L" disabled in the game's AppData plugins.txt.\n\n";
  } else {
    message = L"PluginDisabler updated configured plugins.txt file(s).\n\n";
  }
  if (const auto logDirectory = SKSE::log::log_directory()) {
    message += L"Log: " + (*logDirectory / L"PluginDisabler.log").wstring();
  } else {
    message += L"See the SKSE log directory for PluginDisabler.log.";
  }

  // A mismatch means at least one successfully edited plugin was still found
  // in the loaded game data. Only that case offers the configured quit action.
  UINT flags = MB_ICONINFORMATION | MB_SETFOREGROUND;
  if (!a_stillLoadedPlugins.empty()) {
    message += L"\n\nSome targeted plugins are still loaded in this game "
               L"session. See the log for their names. The running session "
               L"does not reflect the disable list. ";
    if (a_forceTerminateOnAcknowledge) {
      message += L"Click OK to quit Skyrim now.";
      flags |= MB_OK;
    } else {
      message += L"Would you like to quit Skyrim now?";
      flags |= MB_YESNO | MB_DEFBUTTON1;
    }
  } else {
    message += L"\n\nVerification complete: none of the plugins changed by "
               L"PluginDisabler are loaded in this game session. The game can "
               L"safely continue loading";
    flags |= MB_OK;
  }

  const auto result =
      MessageBoxW(nullptr, message.c_str(), L"Plugin Disabler", flags);
  const bool shouldTerminate =
      !a_stillLoadedPlugins.empty() &&
      (a_forceTerminateOnAcknowledge ? result == IDOK : result == IDYES);
  if (shouldTerminate && !TerminateGameProcess()) {
    MessageBoxW(nullptr,
                L"Plugin Disabler could not terminate Skyrim. "
                L"Please close the game manually and relaunch it through your "
                L"usual launcher or mod manager.",
                L"Plugin Disabler", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
  }
}

void OnSKSEMessage(SKSE::MessagingInterface::Message *a_message) {
  // kDataLoaded is sent after Skyrim's data handler has loaded plugin forms;
  // checking here observes the current session, not just the edited text file.
  if (!a_message || a_message->type != SKSE::MessagingInterface::kDataLoaded) {
    return;
  }

  const auto dataHandler = RE::TESDataHandler::GetSingleton();
  if (!dataHandler) {
    logs::error("Could not verify disabled plugins: TESDataHandler is missing");
    return;
  }

  std::set<std::string> stillLoadedPlugins;
  for (const auto &pluginName : pendingSummary.pluginsToVerify) {
    // ESL/light plugins are stored in a separate loaded list, so check both.
    if (dataHandler->LookupLoadedModByName(pluginName) ||
        dataHandler->LookupLoadedLightModByName(pluginName)) {
      stillLoadedPlugins.insert(pluginName);
    }
  }

  logs::info(
      "----------------------------------------------------------------");
  if (stillLoadedPlugins.empty()) {
    logs::info("Post-load verification passed: none of the changed plugins "
               "are loaded in this session");
  } else {
    logs::warn(
        "Post-load verification found {} changed plugin(s) still loaded:",
        stillLoadedPlugins.size());
    for (const auto &pluginName : stillLoadedPlugins) {
      logs::warn("  {}", pluginName);
    }
  }
  ShowDisableSummary(pendingSummary, stillLoadedPlugins,
                     forceTerminateOnVerificationFailure);
}
} // namespace

SKSE_PLUGIN_PRELOAD(const SKSE::PreLoadInterface *a_skse) {
  // SKSE 2.2.7+ invokes this before Skyrim loads its ESP/ESM/ESL data. Keep
  // this callback for early file edits; its interface offers fewer services.
  SKSE::Init(a_skse);
  logs::info("SKSE_PLUGIN_PRELOAD");
  preloadSummary = RunPluginDisabler("preload");
  return true;
}

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface *a_skse) {
  // The ordinary load callback is still required by SKSE. It also handles the
  // file-edit fallback on older SKSE versions that have no preload phase.
  SKSE::Init(a_skse);
  logs::info("SKSE_PLUGIN_LOAD");

  const auto skseVersion = REL::Version::unpack(SKSE::GetSKSEVersion());
  logs::info("Detected SKSE version {}", skseVersion.string());
  pendingSummary = preloadSummary;
  forceTerminateOnVerificationFailure = false;
  try {
    forceTerminateOnVerificationFailure = ForceTerminateIfPluginsStillPresent();
  } catch (const std::exception &error) {
    logs::warn("Could not read ForceTerminateIfPluginsStillPresent setting: {}",
               error.what());
  }

  if (skseVersion < REL::Version(2, 2, 7)) {
    logs::warn("SKSE {} does not support preload; using the normal load phase. "
               "If Skyrim has already read plugins.txt, changes apply next "
               "launch",
               skseVersion.string());
    pendingSummary = RunPluginDisabler("normal load fallback");
  }
  // Register only when there is something to verify. The SKSE message handler
  // performs the check later, after Skyrim signals kDataLoaded.
  if (pendingSummary.pluginsToVerify.empty()) {
    logs::info("No successfully changed plugin entries to verify after load");
    return true;
  }

  const auto messaging = SKSE::GetMessagingInterface();
  if (!messaging || !messaging->RegisterListener(OnSKSEMessage)) {
    logs::error("Could not register for SKSE DataLoaded verification");
  }
  return true;
}