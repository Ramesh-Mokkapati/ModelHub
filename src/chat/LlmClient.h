#pragma once
#include "stdafx.h"

#include "Http.h"
#include "Store.h"

namespace chat
{

// Where chat requests go: the ModelHub service (started when needed; it loads the model's llama-server on the
// first request) or a custom OpenAI-compatible server such as llama-server -m model.gguf --port 8080.
struct Endpoint
{
    std::string baseUrl;  // ".../v1"
    HttpHeaders headers;  // Authorization, X-ModelHub-App
    std::string label;    // for the status line
    bool modelhub = false;
    std::string kind = "openai";  // "openai" (/v1/chat/completions), "anthropic" (/v1/messages), "copilot", "codex"
};

// ModelHub: connects to the running service, or (Settings: "Start ModelHub when needed", on by default) starts it
// and waits for it; onStarting hears about that, and *started says whether it was started here.
bool ResolveEndpoint(const Settings& settings, Endpoint& ep, std::string& error,
    const std::function<void(const std::string&)>& onStarting = {}, bool* started = nullptr);
bool ListModels(const Endpoint& ep, std::vector<std::string>& ids, std::string& error);

enum class DeltaKind
{
    Reasoning,
    Content
};

struct StreamResult
{
    std::string content;
    std::string reasoning;
    json toolCalls = json::array();  // [{id, type:"function", function:{name, arguments}}]
    std::string finishReason;
    std::string error;
    bool cancelled = false;
    double tokensPerSecond = 0;
    int completionTokens = 0;
    json providerContent;  // Claude: the answer's content blocks, replayed as they are on the next request
    std::string servedModel;
};

// Claude's Messages API (AnthropicClient.cpp): the same request (OpenAI shape) and result.
StreamResult StreamAnthropic(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel);

// POST <base>/chat/completions with "stream": true; onDelta runs on the calling thread for every piece of text.
// Runs one of the app's tools for an agent that loops on its own (GitHub Copilot): name, arguments -> result text.
using ToolRunner = std::function<std::string(const std::string& name, const json& args)>;

// GitHub Copilot (ep.kind "copilot") runs its tool loop itself and calls runTool; the other APIs return tool calls.
StreamResult StreamChat(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel, const ToolRunner& runTool = {});

}  // namespace chat
