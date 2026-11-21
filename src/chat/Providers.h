#pragma once
#include "stdafx.h"

#include "LlmClient.h"
#include "OllamaMigrate.h"
#include "Store.h"

namespace chat
{

// Model ids of a provider's models are "<provider id>|<model>"; ModelHub's (and the custom server's) have no "|".
constexpr char kProviderSep = '|';
std::string QualifiedModel(const std::string& providerId, const std::string& model);
bool SplitQualified(const std::string& qualified, std::string& providerId, std::string& model);
// "claude-opus-5-5 (Claude)" for a provider's model, the id itself for a local one.
std::string DisplayModel(const Settings& s, const std::string& qualified);

struct ProviderPreset
{
    const char* label;     // shown in the edit dialog
    const char* id;
    const char* name;
    const char* kind;      // "anthropic" or "openai"
    const char* baseUrl;
    const char* keyEnv;    // environment variable that usually holds the key ("" when none is needed)
    const char* keyPage;   // where to get a key ("" when none is needed)
};
const std::vector<ProviderPreset>& ProviderPresets();

// What is on this PC: API keys in environment variables, model servers that answer, and installed apps.
struct DetectedProvider
{
    ProviderConfig config;
    std::string note;  // "Ollama is running with 3 models", "key in ANTHROPIC_API_KEY"
};
struct Detection
{
    std::vector<DetectedProvider> found;
    std::vector<std::string> notes;  // installed apps that cannot be used directly (Claude, ChatGPT desktop)
    bool claudeApp = false;          // the Claude app or Claude Code is installed
    OllamaInventory ollama;          // models Ollama downloaded
};
Detection DetectProviders();

// "kind|url|key source": the same provider found again (or set up by hand) has the same signature.
std::string ProviderSignature(const ProviderConfig& p);
std::string UniqueProviderId(const std::vector<ProviderConfig>& list, std::string base);
// Adds what Detect found and was never imported before (a provider the user removed is not added again).
// Returns the names added.
std::vector<std::string> ImportDetected(Settings& s, const Detection& d);

bool ProviderEndpoint(const ProviderConfig& p, Endpoint& ep, std::string& error);
// Live list from the provider (GET /v1/models); false with an error when it cannot be reached.
bool ListProviderModels(const ProviderConfig& p, std::vector<std::string>& models, std::string& error);

}  // namespace chat
