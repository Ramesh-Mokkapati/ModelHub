#pragma once
#include "stdafx.h"

namespace chat
{

struct McpServerConfig
{
    std::string name;
    std::string transport = "stdio";  // "stdio" (a command line) or "http" (streamable HTTP)
    std::string command;              // stdio: run through cmd.exe, e.g. npx -y @modelcontextprotocol/server-memory
    std::string url;                  // http: e.g. https://huggingface.co/mcp
    std::string token;                // http: sent as "Authorization: Bearer <token>" when not empty
    bool enabled = true;

    bool SameConnection(const McpServerConfig& o) const
    {
        return name == o.name && transport == o.transport && command == o.command && url == o.url && token == o.token &&
               enabled == o.enabled;
    }
};

// A source of models besides ModelHub: a cloud API (Anthropic's Claude, OpenAI's ChatGPT models, ...) or another
// program on this PC that serves models (Ollama, LM Studio, Jan, a llama-server).
struct ProviderConfig
{
    std::string id;             // short and unique: model ids are "<id>|<model>"
    std::string name;           // "Claude (Anthropic)"
    std::string kind = "openai";  // "anthropic" (Messages API) or "openai" (OpenAI-compatible /v1)
    std::string baseUrl;        // https://api.anthropic.com, https://api.openai.com/v1, http://127.0.0.1:11434/v1
    std::string apiKey;         // kept encrypted for this Windows user in settings.json
    std::string apiKeyEnv;      // or read from this environment variable when apiKey is empty
    bool enabled = true;
    bool inAuto = false;        // Auto mode may pick its models (cloud: messages leave this PC)
    std::vector<std::string> models;  // last list the provider gave (used when it cannot be reached)
    bool Cloud() const;         // not on this PC
    std::string Key() const;    // apiKey, else the environment variable
};

// %LOCALAPPDATA%\ModelHub\chat\settings.json
struct Settings
{
    std::string backend = "modelhub";  // "modelhub" (the ModelHub service) or "custom"
    std::string customUrl = "http://127.0.0.1:8080/v1";
    std::string customKey;
    std::string model;  // a model id, or "auto": Auto mode picks one per message
    std::string reasoning = "default";  // default, off, low, medium, high
    std::string defaultSystemMessage;
    std::string defaultWorkspace;
    std::string shell = "cmd";  // cmd or powershell
    std::string temperature;    // empty: the model's default
    bool confirmShell = true;
    bool confirmWrites = true;
    bool showReasoning = true;
    bool autoStartHub = true;  // start the ModelHub service when it is not running (at startup and when sending)
    int maxToolRounds = 25;
    // Auto mode
    bool autoRouterModel = false;       // a small installed model also classifies each request
    bool autoSticky = true;             // keep the conversation's model when it is nearly as good
    bool autoSuggestDownloads = true;   // offer better models that are not downloaded
    std::map<std::string, std::string> autoOverrides;  // category id -> model id
    std::set<std::string> disabledTools;  // built-in names, and "<server>__<tool>" for MCP tools
    std::vector<McpServerConfig> mcpServers;
    std::vector<ProviderConfig> providers;
    const ProviderConfig* FindProvider(const std::string& id) const;
    // Providers found in installed applications are added at startup, once: one the user removes stays removed.
    bool autoImportProviders = true;
    // The window's panels (widths in pixels at 96 DPI).
    int sidebarWidth = 280;
    int infoWidth = 300;
    bool sidebarVisible = true;
    bool infoVisible = true;
    bool ollamaMoveDeclined = false;   // "Don't ask again" for moving Ollama's models into ModelHub
    bool claudeSetupDeclined = false;  // "Don't ask again" for setting up Claude when the Claude app is installed
    std::set<std::string> importedProviders;  // ProviderSignature() of everything imported so far

    void Load();
    bool Save() const;
};

// One conversation: %LOCALAPPDATA%\ModelHub\chat\conversations\<id>.json. messages are OpenAI chat messages;
// assistant messages may carry "reasoning_content", and fields starting with "x_" are for display only.
struct Conversation
{
    std::string id;
    std::string title;
    std::string workspace;
    std::string systemMessage;
    long long created = 0;
    long long updated = 0;
    json messages = json::array();

    bool Save() const;
    void Delete() const;
};

std::vector<std::shared_ptr<Conversation>> LoadConversations();  // newest first

}  // namespace chat
