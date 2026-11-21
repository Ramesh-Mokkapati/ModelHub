#pragma once
#include "stdafx.h"

namespace chat
{

constexpr UINT WM_APP_EVENT = WM_APP + 10;        // lParam: UiEvent* from the chat worker (the receiver deletes it)
constexpr UINT WM_APP_MODELS = WM_APP + 11;       // lParam: ModelsResult* (the receiver deletes it)
constexpr UINT WM_APP_MCP_CHANGED = WM_APP + 12;  // an MCP server connected, failed or stopped
constexpr UINT WM_APP_CONFIRM = WM_APP + 13;      // lParam: std::shared_ptr<ConfirmRequest>* (the receiver deletes it)
constexpr UINT WM_APP_TREE_CHECK = WM_APP + 14;   // tools dialog: lParam: HTREEITEM whose check box changed
constexpr UINT WM_APP_DOWNLOAD = WM_APP + 15;     // lParam: DownloadEvent* (the receiver deletes it)
constexpr UINT WM_APP_IMPORT = WM_APP + 16;       // lParam: Detection* found at startup (the receiver deletes it)
constexpr UINT WM_APP_CLAUDE_PROGRESS = WM_APP + 17;  // Claude setup: wParam 1 = finished, lParam: std::string*
constexpr UINT WM_APP_MIGRATED = WM_APP + 18;     // lParam: MigrationResult* of the Ollama move (the receiver deletes it)

}  // namespace chat
