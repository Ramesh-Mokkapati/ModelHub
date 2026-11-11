// ModelHubManager.exe - ModelHub in the notification area.
//
//   ModelHubManager [--tray] [--start-service]
//     --tray           start in the notification area only (no window); "Start with Windows" uses this
//     --start-service  start the ModelHub service (ModelHub.exe next to this program) if it is not running
//
// Only one copy runs per user: starting it again shows the manager of the running one.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

#include "App.h"
#include "Config.h"
#include "Paths.h"
#include "Text.h"
#include "modelhub/modelhub_client.h"
#include "resource.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace mgr {

namespace {

namespace fs = std::filesystem;
namespace mc = modelhub::client;
using nlohmann::json;

App g_app;
UINT g_taskbarCreated = 0;
std::atomic<bool> g_busy{false};  // a start, stop or restart is running
std::jthread g_poll;
HICON g_icons[3] = {};  // running, stopped, busy

constexpr wchar_t kTrayClass[] = L"ModelHubManagerTray";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ModelHub Manager";
constexpr char kAppName[] = "ModelHub Manager";

ServiceInfo Probe() {
    ServiceInfo s;
    mc::Client c(kAppName);
    if (!c.Connect(false, 0)) {
        s.state = ServiceState::Stopped;
        return s;
    }
    s.state = ServiceState::Running;
    s.port = c.Port();
    json j = json::parse(c.Status().body, nullptr, false);
    if (j.is_object()) {
        s.version = j.value("version", "");
        s.servers = static_cast<int>(j.value("servers", json::array()).size());
    }
    return s;
}

void PostService(const ServiceInfo& s) {
    auto* p = new ServiceInfo(s);
    if (!PostMessageW(g_app.tray, WM_APP_SERVICE, 0, reinterpret_cast<LPARAM>(p))) delete p;  // exiting
}

bool WaitFor(bool running, DWORD ms) {
    mc::Client c(kAppName);
    for (DWORD waited = 0; waited < ms; waited += 250) {
        if (c.Connect(false, 0) == running) return true;
        Sleep(250);
    }
    return false;
}

// Starts ModelHub.exe next to this program (or the one the client finds) and waits until it answers.
bool StartService(std::wstring* error) {
    mc::Client c(kAppName);
    if (c.Connect(false, 0)) return true;
    fs::path exe = modelhub::ExeDir() / L"ModelHub.exe";
    if (!fs::exists(exe)) {
        std::string err;
        if (c.Connect(true, 30000, &err)) return true;
        *error = modelhub::Widen(err);
        return false;
    }
    std::wstring cmd = L"\"" + exe.wstring() + L"\" serve --background";
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                        exe.parent_path().c_str(), &si, &pi)) {
        *error = L"Cannot start " + exe.wstring() + L" (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD exitCode = STILL_ACTIVE;
    for (DWORD waited = 0; waited < 30000; waited += 250) {
        if (c.Connect(false, 0)) {
            CloseHandle(pi.hProcess);
            return true;
        }
        if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exitCode);
            break;
        }
    }
    CloseHandle(pi.hProcess);
    if (exitCode != STILL_ACTIVE)
        *error = L"ModelHub.exe stopped at once (exit code " + std::to_wstring(exitCode) +
                 L"). Another program may be using its port; see config.json in " + modelhub::HubHome().wstring() + L".";
    else
        *error = L"ModelHub.exe did not answer within 30 seconds.";
    return false;
}

bool StopService(std::wstring* error) {
    mc::Client c(kAppName);
    if (!c.Connect(false, 0)) return true;
    mc::Response r = c.Shutdown();
    if (!r.Ok() && r.status != 0) {
        *error = L"ModelHub did not accept the request to stop (HTTP " + std::to_wstring(r.status) + L").";
        return false;
    }
    if (WaitFor(false, 20000)) return true;
    *error = L"ModelHub did not stop within 20 seconds.";
    return false;
}

void Poll(std::stop_token stop) {
    while (!stop.stop_requested()) {
        if (!g_busy) PostService(Probe());
        for (int i = 0; i < 20 && !stop.stop_requested(); ++i) Sleep(100);
    }
}

// ---------------------------------------------------------------- start with Windows

std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}
std::wstring AutostartCommand() { return L"\"" + ExePath() + L"\" --tray --start-service"; }

bool AutostartOn() {
    wchar_t buf[MAX_PATH * 3];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, AutostartCommand().c_str()) == 0;
}

void SetAutostart(bool on) {
    if (on) {
        std::wstring cmd = AutostartCommand();
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, cmd.c_str(),
                        static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
}

// ---------------------------------------------------------------- tray icon

void AddTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_app.tray;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_icons[1];
    wcscpy_s(nid.szTip, L"ModelHub");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    UpdateTray();
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_app.tray;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    const ServiceState st = g_app.service.state;
    const bool busy = g_busy || st == ServiceState::Starting || st == ServiceState::Stopping;
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"&Open ModelHub Manager");
    SetMenuDefaultItem(menu, ID_TRAY_OPEN, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring status = L"Service: " + ServiceText(g_app.service);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_TRAY_STATUS, status.c_str());
    AppendMenuW(menu, MF_STRING | (busy || st == ServiceState::Running ? MF_GRAYED : 0), ID_TRAY_START, L"&Start service");
    AppendMenuW(menu, MF_STRING | (busy || st != ServiceState::Running ? MF_GRAYED : 0), ID_TRAY_STOP, L"S&top service");
    AppendMenuW(menu, MF_STRING | (busy || st != ServiceState::Running ? MF_GRAYED : 0), ID_TRAY_RESTART, L"&Restart service");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (AutostartOn() ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"Start with &Windows (and start the service)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"E&xit");
    SetForegroundWindow(g_app.tray);  // so the menu closes when clicking elsewhere
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN), x, y,
                     g_app.tray, nullptr);
    PostMessageW(g_app.tray, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void Exit() {
    int n = RunningDownloads();
    if (n > 0 &&
        MessageBoxW(nullptr,
                    (std::to_wstring(n) + L" download(s) are still running. They stop now and resume from where they "
                                          L"stopped the next time you download them.\n\nExit anyway?")
                        .c_str(),
                    L"ModelHub Manager", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    CloseManager();
    DestroyWindow(g_app.tray);
}

LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONDBLCLK: ShowManager(); break;
                case WM_CONTEXTMENU: ShowTrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
            }
            return 0;
        case WM_APP_SHOW: ShowManager(); return 0;
        case WM_APP_SERVICE: {
            std::unique_ptr<ServiceInfo> s(reinterpret_cast<ServiceInfo*>(lp));
            if (!g_busy || s->state == ServiceState::Starting) g_app.service = *s;  // polls wait while busy
            UpdateTray();
            ManagerServiceChanged();
            return 0;
        }
        case WM_APP_ACTION_DONE: {
            std::unique_ptr<ActionResult> r(reinterpret_cast<ActionResult*>(lp));
            if (!r->ok) Notify(L"ModelHub service", r->message, true);
            UpdateTray();
            ManagerServiceChanged();
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_TRAY_OPEN: ShowManager(); break;
                case ID_TRAY_START:
                case ID_TRAY_STOP:
                case ID_TRAY_RESTART: ServiceAction(LOWORD(wp)); break;
                case ID_TRAY_AUTOSTART: SetAutostart(!AutostartOn()); break;
                case ID_TRAY_EXIT: Exit(); break;
            }
            return 0;
        case WM_ENDSESSION:
            if (wp) RemoveTrayIcon();
            return 0;
        case WM_DESTROY:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

App& TheApp() { return g_app; }
WorkerGuard::WorkerGuard() { ++g_app.workers; }
WorkerGuard::~WorkerGuard() { --g_app.workers; }

bool ServiceBusy() { return g_busy; }

std::wstring ServiceText(const ServiceInfo& s) {
    switch (s.state) {
        case ServiceState::Stopped: return L"stopped";
        case ServiceState::Starting: return L"starting...";
        case ServiceState::Stopping: return L"stopping...";
        case ServiceState::Running: {
            std::wstring t = L"running on port " + std::to_wstring(s.port);
            if (!s.version.empty()) t += L" (version " + modelhub::Widen(s.version) + L")";
            if (s.servers > 0) t += L", " + std::to_wstring(s.servers) + (s.servers == 1 ? L" model loaded" : L" models loaded");
            return t;
        }
        default: return L"checking...";
    }
}

void ServiceAction(int command) {
    if (command == ID_TRAY_STOP || command == ID_TRAY_RESTART) {
        if (g_app.service.servers > 0 &&
            MessageBoxW(nullptr,
                        (std::to_wstring(g_app.service.servers) +
                         L" model(s) are loaded in the service, and applications may be using them. Their requests fail "
                         L"until the service runs again.\n\n" +
                         (command == ID_TRAY_STOP ? L"Stop the service anyway?" : L"Restart the service anyway?"))
                            .c_str(),
                        L"ModelHub service", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
    }
    if (g_busy.exchange(true)) return;
    g_app.service.state = command == ID_TRAY_START ? ServiceState::Starting : ServiceState::Stopping;
    UpdateTray();
    ManagerServiceChanged();
    std::thread([command] {
        auto* r = new ActionResult;
        std::wstring err;
        if (command == ID_TRAY_STOP || command == ID_TRAY_RESTART) r->ok = StopService(&err);
        if (r->ok && command == ID_TRAY_RESTART) PostService({ServiceState::Starting});
        if (r->ok && (command == ID_TRAY_START || command == ID_TRAY_RESTART)) r->ok = StartService(&err);
        r->message = err;
        ServiceInfo now = Probe();
        g_busy = false;
        PostService(now);
        if (!PostMessageW(g_app.tray, WM_APP_ACTION_DONE, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void UpdateTray() {
    if (!g_app.tray) return;
    const ServiceState st = g_app.service.state;
    const bool busy = g_busy || st == ServiceState::Starting || st == ServiceState::Stopping || !g_app.downloadSummary.empty();
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_app.tray;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = busy ? g_icons[2] : st == ServiceState::Running ? g_icons[0] : g_icons[1];
    std::wstring tip = L"ModelHub service: " + ServiceText(g_app.service);
    if (!g_app.downloadSummary.empty()) tip += L"\n" + g_app.downloadSummary;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Notify(const std::wstring& title, const std::wstring& text, bool error) {
    NOTIFYICONDATAW nid = {sizeof nid};
    nid.hWnd = g_app.tray;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

}  // namespace mgr

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    using namespace mgr;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool trayOnly = false, startService = false;
    for (int i = 1; i < __argc; ++i) {
        if (_wcsicmp(__wargv[i], L"--tray") == 0) trayOnly = true;
        else if (_wcsicmp(__wargv[i], L"--start-service") == 0) startService = true;
    }

    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\ModelHubManager");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kTrayClass, nullptr)) {
            if (!trayOnly) PostMessageW(other, WM_APP_SHOW, 0, 0);
            if (startService) PostMessageW(other, WM_COMMAND, ID_TRAY_START, 0);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    g_app.inst = inst;
    const int iconIds[3] = {IDI_TRAY_RUNNING, IDI_TRAY_STOPPED, IDI_TRAY_BUSY};
    for (int i = 0; i < 3; ++i) LoadIconMetric(inst, MAKEINTRESOURCEW(iconIds[i]), LIM_SMALL, &g_icons[i]);

    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = TrayProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassExW(&wc);
    g_app.tray = CreateWindowExW(0, kTrayClass, L"ModelHub Manager", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // The model work happens here, with the same core as ModelHubSdk.dll.
    modelhub::CoreOptions opt;
    opt.home = modelhub::HubHome();
    opt.config = modelhub::LoadHubConfig(modelhub::ConfigPath());
    // Next to the program (build output), or one folder up (the SDK and release layout: bin/catalog.json).
    opt.catalogFiles = {opt.home / L"catalog.json", modelhub::ExeDir() / L"catalog.json", modelhub::ExeDir().parent_path() / L"catalog.json"};
    opt.app = kAppName;
    g_app.core = std::make_unique<modelhub::HubCore>(opt);
    if (!g_app.core->Init(&g_app.coreError)) g_app.core.reset();

    AddTrayIcon();
    g_poll = std::jthread(Poll);
    if (startService) ServiceAction(ID_TRAY_START);
    if (!trayOnly) ShowManager();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (ManagerDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_poll = {};
    for (int i = 0; i < 300 && g_app.workers > 0; ++i) Sleep(100);  // Hugging Face lookups end within their timeouts
    g_app.core.reset();  // stops downloads (they resume later)
    CoUninitialize();
    ReleaseMutex(single);
    CloseHandle(single);
    return 0;
}
