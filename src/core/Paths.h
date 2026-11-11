#pragma once
#include <cstdint>
#include <filesystem>

namespace modelhub {

// %MODELHUB_HOME%, else %LOCALAPPDATA%\ModelHub. Holds models\, manifest.json, config.json, hub.json.
std::filesystem::path HubHome();
inline std::filesystem::path ModelsDir() { return HubHome() / L"models"; }
inline std::filesystem::path ManifestPath() { return HubHome() / L"manifest.json"; }
inline std::filesystem::path ConfigPath() { return HubHome() / L"config.json"; }
inline std::filesystem::path HubInfoPath() { return HubHome() / L"hub.json"; }  // port + token of the running hub
std::filesystem::path ExeDir();
std::filesystem::path KnownFolderLocalAppData();
uint64_t FreeDiskBytes(const std::filesystem::path& anyPathOnTheDrive);  // walks up to an existing folder

}  // namespace modelhub
