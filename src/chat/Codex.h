#pragma once
#include "stdafx.h"

#include "LlmClient.h"

// ChatGPT through OpenAI's Codex CLI (it comes with the Codex app): the user's ChatGPT sign-in ("codex login"), so
// a ChatGPT Plus, Pro, Business or Enterprise plan can be used here without an API key. Each answer runs
// "codex exec --json" non-interactively, in a read-only sandbox in the conversation's folder.

namespace chat
{

// The newest codex.exe: CODEX_CLI_PATH, the Codex app's copy (%LOCALAPPDATA%\OpenAI\Codex\bin\<version>), or PATH.
std::wstring CodexExe();

// Signed in: Codex's auth.json (in CODEX_HOME, else %USERPROFILE%\.codex) holds a ChatGPT sign-in or an API key.
bool CodexSignedIn(std::string* how = nullptr);

// The models this sign-in offers, from the list Codex keeps (models_cache.json; refreshed whenever Codex runs).
bool CodexListModels(std::vector<std::string>& models, std::string& error);

StreamResult StreamCodex(const Endpoint& ep, const json& request,
    const std::function<void(DeltaKind, const std::string&)>& onDelta, CancelToken* cancel);

}  // namespace chat
