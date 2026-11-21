#pragma once
// ModelHubChat.exe: an MFC chat client for local models (ModelHub's OpenAI-compatible /v1, or any llama-server).
//   ChatApp.cpp     the application object
//   ChatDlg.cpp     the main window: conversations, transcript, composer, the "+" menu
//   Dialogs.cpp     system message, tools, MCP servers, settings, rename
//   LlmClient.cpp   endpoint (ModelHub or custom), model list, streaming chat completions
//   Tools.cpp       the built-in tools: read_file, file_glob_search, grep_search, exec_shell_command,
//                   write_file, edit_file, get_datetime
//   Mcp.cpp         MCP servers (stdio and streamable HTTP): tools and resources
//   Store.cpp       settings and conversations under %LOCALAPPDATA%\ModelHub\chat
//   Http.cpp        WinHTTP (http and https, streaming, cancellation) and server-sent events

#include <afxwin.h>
#include <afxext.h>
#include <afxdialogex.h>
#include <afxcmn.h>
#include <afxdlgs.h>

#include <richedit.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
