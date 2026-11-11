// The model manager window: what runs well on this PC, suggestions per task, download, delete, and adding models
// (Hugging Face) or folders of GGUF files. Everything it changes is shared with every application through the
// ModelHub store (manifest.json, custom_models.json, config.json).
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "Advisor.h"
#include "App.h"
#include "Text.h"
#include "modelhub/modelhub_client.h"
#include "resource.h"

namespace mgr {

namespace {

using nlohmann::json;
using modelhub::Narrow;
using modelhub::Widen;

constexpr UINT_PTR kTimerDownloads = 1;  // every second: download progress
constexpr UINT_PTR kTimerStatus = 2;     // every 2 seconds: file check state, free space

struct Row {
    std::string id, name, fit, speed, suggestion, path;
    uint64_t size = 0, partial = 0;
    bool installed = false, inHubFolder = true, fitsDisk = true;
    std::vector<std::string> users, copies;
    json model;  // the catalog entry
};

struct WorkResult {
    enum Kind { Delete } kind;
    std::string model;
    json result;
};

HWND g_dlg = nullptr;
std::vector<std::string> g_taskIds;  // combo index 0 = all models
std::string g_task;
json g_status, g_catalog;
std::vector<Row> g_rows;
std::map<std::string, json> g_downloads;  // model -> latest progress
std::set<std::string> g_watching;         // downloads started here: report when they end
bool g_lastScanning = false;
std::wstring g_message;                   // last action's result, shown in the store line

json Call(const std::string& method, const json& args = json::object()) {
    if (!TheApp().core)
        return {{"error", {{"message", TheApp().coreError}}}, {"status", 500}};
    return TheApp().core->Call(method, args);
}

bool IsError(const json& j) { return j.is_object() && j.contains("error"); }
std::wstring ErrorText(const json& j) {
    if (j.is_object() && j.contains("error") && j["error"].is_object()) return Widen(j["error"].value("message", "unknown error"));
    return L"unknown error";
}

double Scale() { return g_dlg ? GetDpiForWindow(g_dlg) / 96.0 : 1.0; }
int S(double v) { return static_cast<int>(v * Scale() + 0.5); }
HWND Item(int id) { return GetDlgItem(g_dlg, id); }
void SetText(int id, const std::wstring& s) { SetDlgItemTextW(g_dlg, id, s.c_str()); }
std::wstring Size(uint64_t bytes) { return Widen(modelhub::FormatSize(bytes)); }

std::string FitText(const std::string& fit) {
    if (fit == "gpu") return "Graphics card";
    if (fit == "gpu+cpu") return "Graphics card + CPU";
    if (fit == "cpu") return "CPU";
    if (fit == "cpu-tight") return "CPU (tight on memory)";
    if (fit == "too-large") return "Too large for this PC";
    return fit;
}

std::string Join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}

const Row* Selected() {
    HWND list = Item(IDC_MODELS);
    int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (i < 0) return nullptr;
    LVITEMW it = {LVIF_PARAM};
    it.iItem = i;
    ListView_GetItem(list, &it);
    return it.lParam >= 0 && static_cast<size_t>(it.lParam) < g_rows.size() ? &g_rows[it.lParam] : nullptr;
}

bool Downloading(const std::string& id) {
    auto it = g_downloads.find(id);
    return it != g_downloads.end() && it->second.value("running", false);
}

std::wstring DownloadText(const json& d) {
    const std::string phase = d.value("phase", "");
    const double done = d.value("done", 0.0), total = d.value("total", 0.0), bps = d.value("bytes_per_second", 0.0);
    if (phase == "waiting") return L"Waiting for another application's download";
    if (phase == "checking") return L"Checking the files on this PC";
    if (phase == "verifying") return L"Verifying (SHA-256)";
    std::wstring t = L"Downloading";
    if (total > 0) t += L" " + std::to_wstring(static_cast<int>(100.0 * done / total)) + L"%";
    if (bps > 0) {
        wchar_t buf[64];
        swprintf_s(buf, L", %.1f MB/s", bps / 1048576.0);
        t += buf;
        if (total > done) {
            int secs = static_cast<int>((total - done) / bps);
            t += secs >= 120 ? L", about " + std::to_wstring(secs / 60) + L" min left" : L", about " + std::to_wstring(secs) + L" s left";
        }
    }
    return t;
}

std::wstring StatusText(const Row& r) {
    if (Downloading(r.id)) return DownloadText(g_downloads[r.id]);
    if (r.installed) return r.inHubFolder ? L"Installed" : L"Installed (app folder)";
    if (r.partial > 0 && r.size > 0)
        return L"Partly downloaded (" + std::to_wstring(static_cast<int>(100.0 * r.partial / r.size)) + L"%)";
    return g_lastScanning ? L"Checking..." : L"Not downloaded";
}

// ---------------------------------------------------------------- data

void BuildRows() {
    std::map<std::string, json> byId;
    std::vector<std::string> order;
    for (const auto& m : g_catalog.value("models", json::array())) {
        byId[m.value("id", "")] = m;
        order.push_back(m.value("id", ""));
    }
    std::map<std::string, json> fit;           // id -> {fit, speed, fits_disk}
    std::map<std::string, std::vector<std::string>> suggested;  // id -> labels
    std::vector<std::string> ranked;           // advice order

    if (g_task.empty()) {
        json f = Call("fits", {{"context_tokens", 8192}, {"parallel", 1}, {"engine", "hub"}});
        for (const auto& m : f.value("models", json::array())) fit[m.value("id", "")] = m;
        for (const auto& t : g_catalog.value("tasks", json::array())) {  // which model ModelHub suggests for each task
            json a = Call("advise", {{"task", t.value("id", "")}, {"engine", "hub"}});
            if (!IsError(a) && !a.value("recommended", "").empty())
                suggested[a.value("recommended", "")].push_back(t.value("name", t.value("id", "")));
        }
        for (auto& [id, v] : suggested) v.insert(v.begin(), "Suggested for:");
    } else {
        json a = Call("advise", {{"task", g_task}, {"engine", "hub"}});
        const std::string rec = a.value("recommended", ""), first = a.value("first_model", ""), ideal = a.value("ideal", "");
        for (const auto& o : a.value("options", json::array())) {
            fit[o.value("id", "")] = o;
            ranked.push_back(o.value("id", ""));
        }
        if (!rec.empty()) suggested[rec].push_back("★ Recommended");
        if (!first.empty() && first != rec) suggested[first].push_back("Start with this");
        if (!ideal.empty() && ideal != rec) suggested[ideal].push_back("Best for large work");
        // Only the task's models, in the order the advice ranks them (all models if it ranks none).
        if (!ranked.empty()) order = ranked;
    }

    g_rows.clear();
    for (const auto& id : order) {
        const json& m = byId[id];
        if (m.is_null()) continue;
        Row r;
        r.id = id;
        r.model = m;
        r.name = m.value("name", id);
        r.size = m.value("size", uint64_t{0});
        r.installed = m.value("installed", false);
        r.path = m.value("path", "");
        r.inHubFolder = m.value("in_hub_folder", true);
        r.partial = m.value("partial_bytes", uint64_t{0});
        r.users = m.value("users", std::vector<std::string>{});
        r.copies = m.value("copies", std::vector<std::string>{});
        if (auto f = fit.find(id); f != fit.end()) {
            r.fit = FitText(f->second.value("fit", ""));
            r.speed = f->second.value("speed", "");
            r.fitsDisk = f->second.value("fits_disk", true);
        }
        if (auto s = suggested.find(id); s != suggested.end())
            r.suggestion = s->second.front() == "Suggested for:" ? "Suggested for " + Join({s->second.begin() + 1, s->second.end()})
                                                                  : Join(s->second);
        g_rows.push_back(std::move(r));
    }
}

void SetCell(HWND list, int item, int col, const std::wstring& text) {
    ListView_SetItemText(list, item, col, const_cast<wchar_t*>(text.c_str()));
}

void FillList() {
    HWND list = Item(IDC_MODELS);
    const Row* sel = Selected();
    std::string keep = sel ? sel->id : "";
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    int selectIndex = -1;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const Row& r = g_rows[i];
        std::wstring name = Widen(r.name);
        LVITEMW it = {LVIF_TEXT | LVIF_PARAM};
        it.iItem = static_cast<int>(i);
        it.pszText = name.data();
        it.lParam = static_cast<LPARAM>(i);
        int at = ListView_InsertItem(list, &it);
        SetCell(list, at, 1, Size(r.size));
        SetCell(list, at, 2, Widen(r.fit));
        SetCell(list, at, 3, Widen(r.speed));
        SetCell(list, at, 4, StatusText(r));
        SetCell(list, at, 5, Widen(Join(r.users)));
        SetCell(list, at, 6, Widen(r.suggestion));
        if (r.id == keep) selectIndex = at;
    }
    if (selectIndex < 0 && !g_rows.empty() && !g_task.empty()) selectIndex = 0;  // the recommendation
    if (selectIndex >= 0) {
        ListView_SetItemState(list, selectIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, selectIndex, FALSE);
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void UpdateSelection() {
    const Row* r = Selected();
    const bool dl = r && Downloading(r->id);
    EnableWindow(Item(IDC_DOWNLOAD), r && !r->installed && !dl);
    EnableWindow(Item(IDC_CANCEL_DOWNLOAD), dl);
    EnableWindow(Item(IDC_DELETE), r && !dl && (r->installed || r->partial > 0));
    EnableWindow(Item(IDC_SHOW), r && r->installed);
    SetDlgItemTextW(g_dlg, IDC_DOWNLOAD, r && r->partial > 0 && !r->installed ? L"Resume &download" : L"&Download");

    std::wstring details;
    if (r) {
        const json& m = r->model;
        wchar_t params[64] = L"";
        if (double p = m.value("params_b", 0.0); p > 0) {
            double active = m.value("active_params_b", 0.0);
            if (active > 0 && active < p) swprintf_s(params, L"%.1fB parameters (%.1fB active)", p, active);
            else swprintf_s(params, L"%.1fB parameters", p);
        }
        details = Widen(r->name) + L"  ·  " + params + L"  ·  licence: " + Widen(m.value("license", "?"));
        if (!m.value("summary", "").empty()) details += L"\n" + Widen(m.value("summary", ""));
        if (r->installed) details += L"\n" + Widen(r->path);
        if (r->copies.size() > 1) details += L"  (+" + std::to_wstring(r->copies.size() - 1) + L" more copies)";
        if (!r->installed && !r->fitsDisk) details += L"\nNot enough free disk space for this model.";
    }
    SetText(IDC_DETAILS, details);

    if (dl) {
        const json& d = g_downloads[r->id];
        double done = d.value("done", 0.0), total = d.value("total", 0.0);
        ShowWindow(Item(IDC_PROGRESS), SW_SHOW);
        SendMessageW(Item(IDC_PROGRESS), PBM_SETRANGE32, 0, 1000);
        SendMessageW(Item(IDC_PROGRESS), PBM_SETPOS, total > 0 ? static_cast<WPARAM>(1000.0 * done / total) : 0, 0);
        SetText(IDC_PROGRESS_TEXT, DownloadText(d) + L"  (" + Size(static_cast<uint64_t>(done)) + L" of " +
                                       Size(static_cast<uint64_t>(total)) + L")");
    } else {
        ShowWindow(Item(IDC_PROGRESS), SW_HIDE);
        SetText(IDC_PROGRESS_TEXT, L"");
    }
}

void UpdateStoreLine() {
    std::wstring s;
    if (!TheApp().core) {
        s = L"ModelHub could not start: " + Widen(TheApp().coreError);
    } else {
        s = L"Models folder: " + Widen(g_status.value("models_dir", "")) + L"  ·  " +
            Size(g_status.value("free_disk_bytes", uint64_t{0})) + L" free";
        if (g_status.value("scanning", false)) s += L"  ·  checking files: " + Widen(g_status.value("scan", ""));
    }
    if (!g_message.empty()) s = g_message + L"   |   " + s;
    SetText(IDC_STORE, s);
}

void UpdateHeader() {
    std::string hw = g_status.contains("hardware") ? g_status["hardware"].value("summary", "") : "";
    SetText(IDC_HARDWARE, hw.empty() ? L"" : L"This PC: " + Widen(hw));
    std::wstring notes;
    if (!TheApp().core) {
        notes = L"The catalog could not be loaded, so no models can be shown.";
    } else if (g_task.empty()) {
        notes = L"Every model in the catalog, and how it would run on this PC (8K context). Pick a task to see the "
                L"model ModelHub suggests for it. Models you download here are shared with every application.";
    } else {
        json a = Call("advise", {{"task", g_task}, {"engine", "hub"}});
        std::vector<std::string> n = a.value("notes", std::vector<std::string>{});
        notes = Widen(Join(n, " "));
    }
    SetText(IDC_NOTES, notes);
    UpdateStoreLine();
}

void Refresh() {
    if (!g_dlg) return;
    g_status = Call("status");
    g_catalog = Call("catalog");
    g_lastScanning = g_status.value("scanning", false);
    BuildRows();
    FillList();
    UpdateHeader();
    UpdateSelection();
}

void FillTasks() {
    HWND combo = Item(IDC_TASK);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    g_taskIds = {""};
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All models (what runs on this PC)"));
    json cat = Call("catalog");
    for (const auto& t : cat.value("tasks", json::array())) {
        std::wstring name = Widen(t.value("name", t.value("id", "")));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        g_taskIds.push_back(t.value("id", ""));
    }
    auto at = std::find(g_taskIds.begin(), g_taskIds.end(), g_task);
    SendMessageW(combo, CB_SETCURSEL, at == g_taskIds.end() ? 0 : at - g_taskIds.begin(), 0);
}

void SelectModel(const std::string& id) {
    HWND list = Item(IDC_MODELS);
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_rows[i].id == id) {
            ListView_SetItemState(list, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list, static_cast<int>(i), FALSE);
        }
}

// ---------------------------------------------------------------- downloads

void PollDownloads() {
    json d = Call("downloads");
    int running = 0;
    std::wstring summary;
    bool ended = false;
    for (const auto& s : d.value("downloads", json::array())) {
        const std::string id = s.value("model", "");
        g_downloads[id] = s;
        if (s.value("running", false)) {
            ++running;
            if (summary.empty()) summary = L"Downloading " + Widen(id) + L": " + DownloadText(s);
            continue;
        }
        if (!g_watching.erase(id)) continue;
        ended = true;
        std::wstring name = Widen(id);
        if (s.value("phase", "") == "done") {
            g_message = name + L" downloaded and verified.";
            Notify(L"ModelHub", name + L" is downloaded, verified and ready for every application.");
        } else if (!s.value("error", "").empty() && s.value("error", "").find("cancel") == std::string::npos) {
            g_message = name + L": " + Widen(s.value("error", ""));
            Notify(L"Download failed", g_message, true);
        } else {
            g_message = L"Download of " + name + L" cancelled; it resumes from where it stopped.";
        }
    }
    if (running > 1) summary += L" (+" + std::to_wstring(running - 1) + L" more)";
    if (summary != TheApp().downloadSummary) {
        TheApp().downloadSummary = summary;
        UpdateTray();
    }
    if (ended) {
        Refresh();
        return;
    }
    HWND list = Item(IDC_MODELS);
    for (size_t i = 0; i < g_rows.size(); ++i)
        if (g_downloads.count(g_rows[i].id)) SetCell(list, static_cast<int>(i), 4, StatusText(g_rows[i]));
    UpdateSelection();
}

void StartDownload(const Row& r) {
    if (r.fit == FitText("too-large") &&
        MessageBoxW(g_dlg, (Widen(r.name) + L" is too large to run well on this PC. Download it anyway?").c_str(),
                    L"Download", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!r.fitsDisk &&
        MessageBoxW(g_dlg, (L"There may not be enough free disk space for " + Widen(r.name) + L" (" + Size(r.size) +
                            L"). Download it anyway?")
                               .c_str(),
                    L"Download", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    json res = Call("start_download", {{"model", r.id}});
    if (IsError(res)) {
        MessageBoxW(g_dlg, ErrorText(res).c_str(), L"Download", MB_ICONERROR);
        return;
    }
    if (res.value("state", "") == "installed") {
        Refresh();
        return;
    }
    g_watching.insert(r.id);
    g_message = L"Downloading " + Widen(r.name) + L" into the shared store...";
    PollDownloads();
}

// ---------------------------------------------------------------- delete

void PostWork(WorkResult* w) {
    if (!g_dlg || !PostMessageW(g_dlg, WM_APP_WORK_DONE, 0, reinterpret_cast<LPARAM>(w))) delete w;
}

void DeleteSelected() {
    const Row* r = Selected();
    if (!r) return;
    std::wstring text = L"Delete " + Widen(r->name) + L"?\n\n";
    if (r->installed) {
        text += L"Files (" + Size(r->size) + L" each):\n";
        std::vector<std::string> copies = r->copies.empty() ? std::vector<std::string>{r->path} : r->copies;
        for (const auto& c : copies) text += L"    " + Widen(c) + L"\n";
    }
    if (r->partial > 0) text += L"The partial download (" + Size(r->partial) + L") is deleted too.\n";
    if (!r->users.empty())
        text += L"\nUsed by: " + Widen(Join(r->users)) + L". These applications will have to download it again.\n";
    if (r->installed && !r->inHubFolder) text += L"\nA copy in an application's own folder is deleted too.\n";
    if (r->installed) text += L"\nThe files go to the Recycle Bin.";
    if (MessageBoxW(g_dlg, text.c_str(), L"Delete model", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;

    const std::string id = r->id;
    const bool viaService = TheApp().service.state == ServiceState::Running;
    g_message = L"Deleting " + Widen(r->name) + L"...";
    UpdateStoreLine();
    EnableWindow(Item(IDC_DELETE), FALSE);
    std::thread([id, viaService] {
        WorkerGuard guard;
        auto* w = new WorkResult{WorkResult::Delete, id, {}};
        if (viaService) {  // the service stops its llama-server of this model first
            modelhub::client::Client c("ModelHub Manager");
            if (c.Connect(false, 0)) {
                modelhub::client::Response resp = c.DeleteModel(id);
                w->result = json::parse(resp.body, nullptr, false);
                if (w->result.is_discarded())
                    w->result = {{"error", {{"message", "the service answered HTTP " + std::to_string(resp.status)}}}};
            }
        }
        if (w->result.is_null()) w->result = Call("delete_model", {{"model", id}});
        PostWork(w);
    }).detach();
}

// ---------------------------------------------------------------- add a model / a folder

void AddFolder() {
    IFileOpenDialog* fd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) return;
    DWORD opts = 0;
    fd->GetOptions(&opts);
    fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    fd->SetTitle(L"A folder with GGUF model files to share with every application");
    std::wstring folder;
    if (SUCCEEDED(fd->Show(g_dlg))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(fd->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                folder = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    fd->Release();
    if (folder.empty()) return;
    json r = Call("add_folder", {{"path", Narrow(folder)}, {"persist", true}, {"app", "ModelHub Manager"}});
    if (IsError(r)) {
        MessageBoxW(g_dlg, ErrorText(r).c_str(), L"Add folder", MB_ICONERROR);
        return;
    }
    g_message = L"Checking the model files in " + folder + L"; models found there are shared with every application.";
    Refresh();
}

void ShowInFolder(const Row& r) {
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(Widen(r.path).c_str())) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

// ---------------------------------------------------------------- layout

void Place(int id, int x, int y, int w, int h) {
    SetWindowPos(Item(id), nullptr, x, y, std::max(0, w), std::max(0, h), SWP_NOZORDER | SWP_NOACTIVATE);
}

void Layout() {
    RECT rc;
    GetClientRect(g_dlg, &rc);
    const int W = rc.right, H = rc.bottom, m = S(10), gap = S(6), bh = S(26), lh = S(18);
    int y = m;
    Place(IDC_HARDWARE, m, y, W - 2 * m, lh);
    y += lh + gap;
    const int sbw = S(76);
    Place(IDC_SERVICE_RESTART, W - m - sbw, y - S(3), sbw, bh);
    Place(IDC_SERVICE_STOP, W - m - 2 * sbw - gap, y - S(3), sbw, bh);
    Place(IDC_SERVICE_START, W - m - 3 * sbw - 2 * gap, y - S(3), sbw, bh);
    Place(IDC_SERVICE, m, y + S(2), W - 2 * m - 3 * sbw - 3 * gap, lh);
    y += bh + gap;
    Place(IDC_TASK_LABEL, m, y + S(4), S(120), lh);
    Place(IDC_TASK, m + S(124), y, S(400), S(300));
    y += bh + gap;
    Place(IDC_NOTES, m, y, W - 2 * m, S(50));  // three lines
    y += S(50) + gap;

    int bottom = H - m;
    Place(IDC_STORE, m, bottom - lh, W - 2 * m, lh);
    bottom -= lh + gap;
    // Buttons: model actions on the left, adding on the right.
    struct B { int id; int w; };
    const B left[] = {{IDC_DOWNLOAD, 112}, {IDC_CANCEL_DOWNLOAD, 112}, {IDC_DELETE, 80}, {IDC_SHOW, 104}};
    const B right[] = {{IDC_ADD_HF, 160}, {IDC_ADD_FOLDER, 96}, {IDC_RESCAN, 72}};
    int x = m;
    for (const B& b : left) {
        Place(b.id, x, bottom - bh, S(b.w), bh);
        x += S(b.w) + gap;
    }
    x = W - m;
    for (int i = static_cast<int>(std::size(right)) - 1; i >= 0; --i) {
        x -= S(right[i].w);
        Place(right[i].id, x, bottom - bh, S(right[i].w), bh);
        x -= gap;
    }
    bottom -= bh + gap;
    Place(IDC_PROGRESS, m, bottom - S(16), S(220), S(16));
    Place(IDC_PROGRESS_TEXT, m + S(228), bottom - S(17), W - 2 * m - S(228), lh);
    bottom -= S(16) + gap;
    Place(IDC_DETAILS, m, bottom - S(48), W - 2 * m, S(48));
    bottom -= S(48) + gap;
    Place(IDC_MODELS, m, y, W - 2 * m, bottom - y);
    InvalidateRect(g_dlg, nullptr, TRUE);
}

void SetupList() {
    HWND list = Item(IDC_MODELS);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const struct { const wchar_t* name; int width; int fmt; } cols[] = {
        {L"Model", 240, LVCFMT_LEFT},     {L"Size", 64, LVCFMT_RIGHT},    {L"Runs on", 136, LVCFMT_LEFT},
        {L"Speed", 64, LVCFMT_LEFT},      {L"Status", 190, LVCFMT_LEFT},  {L"Used by", 110, LVCFMT_LEFT},
        {L"Suggestion", 300, LVCFMT_LEFT}};
    for (int i = 0; i < static_cast<int>(std::size(cols)); ++i) {
        LVCOLUMNW c = {LVCF_TEXT | LVCF_WIDTH | LVCF_FMT};
        c.pszText = const_cast<wchar_t*>(cols[i].name);
        c.cx = S(cols[i].width);
        c.fmt = cols[i].fmt;
        ListView_InsertColumn(list, i, &c);
    }
}

void OnWorkDone(std::unique_ptr<WorkResult> w) {
    if (w->kind == WorkResult::Delete) {
        const json& r = w->result;
        std::wstring name = Widen(w->model);
        if (IsError(r) && r.value("deleted", json::array()).empty()) {
            g_message = L"";
            MessageBoxW(g_dlg, (L"Could not delete " + name + L":\n\n" + ErrorText(r)).c_str(), L"Delete model", MB_ICONERROR);
        } else {
            size_t n = r.value("deleted", json::array()).size();
            g_message = L"Deleted " + name + L" (" + std::to_wstring(n) + L" file" + (n == 1 ? L"" : L"s") +
                        (r.value("recycled", true) ? L", in the Recycle Bin)." : L").");
            std::wstring failed;
            for (const auto& f : r.value("failed", json::array()))
                failed += L"\n" + Widen(f.value("path", "")) + L": " + Widen(f.value("error", ""));
            if (!failed.empty())
                MessageBoxW(g_dlg, (L"Some files of " + name + L" could not be deleted:" + failed).c_str(), L"Delete model",
                            MB_ICONWARNING);
        }
        Refresh();
    }
}

INT_PTR CALLBACK ManagerProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_dlg = dlg;
            HICON big = nullptr, little = nullptr;
            LoadIconMetric(TheApp().inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
            LoadIconMetric(TheApp().inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &little);
            SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little));
            SetupList();
            FillTasks();
            RECT work;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            int w = std::min<int>(S(1120), work.right - work.left), h = std::min<int>(S(680), work.bottom - work.top);
            SetWindowPos(dlg, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                         w, h, SWP_NOZORDER);
            Layout();
            ManagerServiceChanged();
            Refresh();
            SetTimer(dlg, kTimerDownloads, 1000, nullptr);
            SetTimer(dlg, kTimerStatus, 2000, nullptr);
            return TRUE;
        }
        case WM_SIZE: Layout(); return TRUE;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {S(860), S(520)};
            return TRUE;
        }
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(dlg, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return TRUE;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && IsWindowVisible(dlg)) Refresh();  // another application may have changed the store
            return FALSE;
        case WM_TIMER:
            if (wp == kTimerDownloads) {
                PollDownloads();
            } else if (wp == kTimerStatus && IsWindowVisible(dlg)) {
                g_status = Call("status");
                bool scanning = g_status.value("scanning", false);
                if (scanning != g_lastScanning) Refresh();  // the file check finished: installed states changed
                else UpdateStoreLine();
            }
            return TRUE;
        case WM_APP_WORK_DONE: OnWorkDone(std::unique_ptr<WorkResult>(reinterpret_cast<WorkResult*>(lp))); return TRUE;
        case WM_NOTIFY: {
            auto* h = reinterpret_cast<NMHDR*>(lp);
            if (h->idFrom == IDC_MODELS) {
                if (h->code == LVN_ITEMCHANGED) UpdateSelection();
                else if (h->code == NM_DBLCLK) {
                    if (const Row* r = Selected(); r && !r->installed && !Downloading(r->id)) StartDownload(*r);
                }
            }
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_TASK:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        int i = static_cast<int>(SendMessageW(Item(IDC_TASK), CB_GETCURSEL, 0, 0));
                        g_task = i > 0 && i < static_cast<int>(g_taskIds.size()) ? g_taskIds[i] : "";
                        g_message.clear();
                        Refresh();
                    }
                    break;
                case IDC_DOWNLOAD:
                    if (const Row* r = Selected(); r && IsWindowEnabled(Item(IDC_DOWNLOAD))) StartDownload(*r);
                    break;
                case IDC_CANCEL_DOWNLOAD:
                    if (const Row* r = Selected()) Call("cancel_download", {{"model", r->id}});
                    break;
                case IDC_DELETE: DeleteSelected(); break;
                case IDC_SHOW:
                    if (const Row* r = Selected(); r && r->installed) ShowInFolder(*r);
                    break;
                case IDC_ADD_HF: ShowHfBrowser(dlg); break;
                case IDC_ADD_FOLDER: AddFolder(); break;
                case IDC_RESCAN:
                    Call("rescan");
                    g_message = L"Checking the model files again...";
                    Refresh();
                    break;
                case IDC_SERVICE_START: ServiceAction(ID_TRAY_START); break;
                case IDC_SERVICE_STOP: ServiceAction(ID_TRAY_STOP); break;
                case IDC_SERVICE_RESTART: ServiceAction(ID_TRAY_RESTART); break;
                case IDCANCEL: ShowWindow(dlg, SW_HIDE); break;  // Esc: back to the tray
            }
            return TRUE;
        case WM_CLOSE: ShowWindow(dlg, SW_HIDE); return TRUE;  // keeps running in the tray (downloads continue)
        case WM_DESTROY:
            KillTimer(dlg, kTimerDownloads);
            KillTimer(dlg, kTimerStatus);
            g_dlg = nullptr;
            return TRUE;
    }
    return FALSE;
}

}  // namespace

void ShowManager() {
    if (!g_dlg) CreateDialogParamW(TheApp().inst, MAKEINTRESOURCEW(IDD_MANAGER), nullptr, ManagerProc, 0);
    if (!g_dlg) return;
    if (IsIconic(g_dlg)) ShowWindow(g_dlg, SW_RESTORE);
    ShowWindow(g_dlg, SW_SHOW);
    SetForegroundWindow(g_dlg);
}

void CloseManager() {
    if (g_dlg) DestroyWindow(g_dlg);
}

void ManagerServiceChanged() {
    if (!g_dlg) return;
    const ServiceInfo& s = TheApp().service;
    const bool busy = ServiceBusy() || s.state == ServiceState::Starting || s.state == ServiceState::Stopping;
    SetText(IDC_SERVICE, L"ModelHub service (shares loaded models between applications): " + ServiceText(s));
    EnableWindow(Item(IDC_SERVICE_START), !busy && s.state == ServiceState::Stopped);
    EnableWindow(Item(IDC_SERVICE_STOP), !busy && s.state == ServiceState::Running);
    EnableWindow(Item(IDC_SERVICE_RESTART), !busy && s.state == ServiceState::Running);
}

bool ManagerDialogMessage(MSG* msg) {
    if (HWND hf = HfBrowserWindow(); hf && IsDialogMessageW(hf, msg)) return true;
    return g_dlg && IsDialogMessageW(g_dlg, msg);
}

void RefreshManager() {
    if (g_dlg && IsWindowVisible(g_dlg)) Refresh();
}

void DownloadModelById(const std::string& id) {
    if (!g_dlg) return;
    if (!g_task.empty()) {  // models added from Hugging Face belong to no task: show them all
        g_task.clear();
        SendMessageW(Item(IDC_TASK), CB_SETCURSEL, 0, 0);
    }
    Refresh();
    for (const Row& r : g_rows)
        if (r.id == id) {
            SelectModel(id);
            if (r.installed) g_message = Widen(r.name) + L" is already installed.";
            else if (!Downloading(id)) StartDownload(r);
            UpdateStoreLine();
            return;
        }
}

int RunningDownloads() {
    int n = 0;
    for (const auto& [id, d] : g_downloads) n += d.value("running", false) ? 1 : 0;
    return n;
}

}  // namespace mgr
