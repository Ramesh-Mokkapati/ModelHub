#pragma once
// ModelHubManager.exe: ModelHub in the notification area.
//   main.cpp           tray icon, ModelHub service control (start, stop, restart), start with Windows
//   ManagerWindow.cpp  the model manager: suggestions for this PC, download, delete, add models and folders
//   HfBrowser.cpp      browse GGUF models on Hugging Face: best use, which files run here, download
// Model work runs in this process through HubCore (the same core as ModelHubSdk.dll), so the manager works
// whether or not the service is running; deleting goes through the service when it runs, so that it can first
// stop its llama-server of that model.
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

#include "HubCore.h"

namespace mgr {

constexpr UINT WM_APP_TRAY = WM_APP + 1;         // tray icon events
constexpr UINT WM_APP_SERVICE = WM_APP + 2;      // lParam: ServiceInfo* (the receiver deletes it)
constexpr UINT WM_APP_ACTION_DONE = WM_APP + 3;  // lParam: ActionResult* (the receiver deletes it)
constexpr UINT WM_APP_SHOW = WM_APP + 4;         // a second start of the program: show the manager
constexpr UINT WM_APP_WORK_DONE = WM_APP + 5;    // manager window: lParam: WorkResult* (the receiver deletes it)

enum class ServiceState { Unknown, Stopped, Starting, Running, Stopping };

struct ServiceInfo {
    ServiceState state = ServiceState::Unknown;
    int port = 0;
    std::string version;
    int servers = 0;  // llama-servers it has running
};

struct ActionResult {
    bool ok = true;
    std::wstring message;
};

struct App {
    HINSTANCE inst = nullptr;
    HWND tray = nullptr;  // hidden window that owns the tray icon
    std::unique_ptr<modelhub::HubCore> core;
    std::string coreError;  // why the core could not start (no catalog, ...)
    ServiceInfo service;
    std::wstring downloadSummary;  // for the tray tooltip, "" when nothing downloads
    std::atomic<int> workers{0};   // worker threads using core: exit waits for them
};
App& TheApp();

// Held by a worker thread while it uses TheApp().core.
struct WorkerGuard {
    WorkerGuard();
    ~WorkerGuard();
    WorkerGuard(const WorkerGuard&) = delete;
    WorkerGuard& operator=(const WorkerGuard&) = delete;
};

// main.cpp
void ServiceAction(int command);  // ID_TRAY_START / ID_TRAY_STOP / ID_TRAY_RESTART, on a worker thread
bool ServiceBusy();
std::wstring ServiceText(const ServiceInfo& s);
void Notify(const std::wstring& title, const std::wstring& text, bool error = false);  // tray balloon
void UpdateTray();

// ManagerWindow.cpp
void ShowManager();
void DownloadModelById(const std::string& id);  // after adding a model: refresh the list and download it
void RefreshManager();
void CloseManager();
void ManagerServiceChanged();
bool ManagerDialogMessage(MSG* msg);

// HfBrowser.cpp
void ShowHfBrowser(HWND owner);
HWND HfBrowserWindow();
int RunningDownloads();

}  // namespace mgr
