#pragma once
#include "stdafx.h"

#include "LlmClient.h"

namespace chat
{

// Models that Ollama downloaded: each is a GGUF file in <models>\blobs named after its SHA-256, listed by a
// manifest in <models>\manifests\<registry>\<namespace>\<model>\<tag>.
struct OllamaModel
{
    std::string name;  // "qwen3:8b", "hf.co/owner/repo:Q4_K_M"
    std::vector<std::string> aliases;  // other tags of the same file
    std::wstring blob;
    std::string sha256;
    unsigned long long size = 0;
    std::string url;  // where the registry serves the file (ModelHub can download it again from there)
    bool projector = false;  // a vision model: its image projector is not moved
};

struct OllamaInventory
{
    std::wstring modelsDir;  // %OLLAMA_MODELS%, else %USERPROFILE%\.ollama\models
    std::vector<OllamaModel> models;
    unsigned long long totalBytes = 0;
    bool ollamaInstalled = false;
    bool ollamaRunning = false;
};

OllamaInventory FindOllamaModels();

struct MigrationResult
{
    int moved = 0;        // files moved into ModelHub's models folder
    int duplicates = 0;   // ModelHub had the file already: Ollama's copy was dropped
    std::vector<std::string> failed;
    bool folderDeleted = false;
    std::string error;
};

// Registers every model with ModelHub (a catalog model when the SHA-256 is one, else a custom model "ollama-..."),
// moves its file into ModelHub's models folder, asks ModelHub to verify them, and then deletes Ollama's models
// folder if every model moved. ep must be the ModelHub service.
MigrationResult MigrateOllamaModels(const Endpoint& ep, const OllamaInventory& inv,
    const std::function<void(const std::string&)>& progress);

}  // namespace chat
