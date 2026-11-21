#pragma once
#include "stdafx.h"

#include "LlmClient.h"

namespace chat
{

// GitHub Copilot's models through the local Copilot runtime, the engine the GitHub Copilot app and the Copilot SDK
// use (GitHub Models, the old HTTP API, was retired on 30 July 2026). The runtime runs as a child process speaking
// JSON-RPC 2.0 over stdio with "Content-Length:" framing; one runtime per GitHub host, started when first needed.
// Sign-in is the Copilot CLI's own (copilot login [--host https://<company>.ghe.com]), so a Copilot subscription
// on that account is needed; every prompt counts toward its usage.

// copilot-runtime.exe: %COPILOT_CLI_PATH%, else the newest one the GitHub Copilot app keeps in
// %LOCALAPPDATA%\github-copilot-sdk\cli; "" when neither is there.
std::wstring CopilotRuntimeExe();
// copilot.exe next to the runtime (for "copilot login"), "" when missing.
std::wstring CopilotCliExe();

struct CopilotAuth
{
    bool runtimeFound = false;
    bool signedIn = false;
    std::string login;  // the account, when the runtime says
    std::string error;
};
// host: "https://github.com" or "https://<company>.ghe.com".
CopilotAuth CopilotAuthStatus(const std::string& host);
bool CopilotListModels(const std::string& host, std::vector<std::string>& models, std::string& error);

// One request as a fresh Copilot session: the conversation so far goes in as context, the app's tools (request
// "tools", OpenAI shape) are the only tools Copilot may use, and it calls them through runTool. The answer streams
// through onDelta. Copilot runs its own tool loop, so the result never carries tool calls.
StreamResult StreamCopilot(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel, const ToolRunner& runTool);

}  // namespace chat
