// Browse Hugging Face: search GGUF models, see what each is best used for and how each of its files would run on
// this PC, and download one for every application (it becomes a custom model in ModelHub's catalog).
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Advisor.h"
#include "App.h"
#include "HuggingFace.h"
#include "Text.h"
#include "resource.h"

namespace mgr {

namespace {

using nlohmann::json;
using modelhub::Narrow;
using modelhub::Widen;

constexpr UINT WM_HF_DONE = WM_APP + 20;  // lParam: Work* (the receiver deletes it)

struct Work {
    enum Kind { Search, Repo, Add } kind;
    int generation = 0;  // results of an older request are dropped
    std::string repo, file;
    json result;
};

HWND g_hf = nullptr;
std::vector<json> g_results;   // what the list shows
json g_allResults = json::array();
json g_repo;                   // the selected repository's evaluation
std::string g_wantFile;        // a file named in the search box: select it when its repository arrives
std::atomic<int> g_searchGen{0}, g_repoGen{0};

const char* kKinds[][2] = {{"", "All GGUF models"}, {"text", "Chat and writing"}, {"code", "Coding"}, {"embeddings", "Embeddings"}};
const char* kSorts[][2] = {{"downloads", "Most downloaded"}, {"trending", "Trending"}, {"likes", "Most liked"}, {"updated", "Recently updated"}};

double Scale() { return g_hf ? GetDpiForWindow(g_hf) / 96.0 : 1.0; }
int S(double v) { return static_cast<int>(v * Scale() + 0.5); }
HWND Item(int id) { return GetDlgItem(g_hf, id); }
void SetText(int id, const std::wstring& s) { SetDlgItemTextW(g_hf, id, s.c_str()); }
void Status(const std::wstring& s) { SetText(IDC_HF_STATUS, s); }

json Call(const std::string& method, const json& args) {
    if (!TheApp().core) return {{"error", {{"message", TheApp().coreError}}}, {"status", 500}};
    return TheApp().core->Call(method, args);
}
bool IsError(const json& j) { return j.is_object() && j.contains("error"); }
std::wstring ErrorText(const json& j) {
    return j.is_object() && j.contains("error") && j["error"].is_object() ? Widen(j["error"].value("message", "error")) : L"error";
}

std::wstring FitText(const std::string& fit) {
    if (fit == "gpu") return L"Graphics card";
    if (fit == "gpu+cpu") return L"Graphics card + CPU";
    if (fit == "cpu") return L"CPU";
    if (fit == "cpu-tight") return L"CPU (tight on memory)";
    if (fit == "too-large") return L"Too large";
    return L"?";
}

std::wstring Count(int64_t n) {
    wchar_t b[32];
    if (n >= 1000000) swprintf_s(b, L"%.1fM", n / 1e6);
    else if (n >= 1000) swprintf_s(b, L"%.1fk", n / 1e3);
    else swprintf_s(b, L"%lld", static_cast<long long>(n));
    return b;
}

std::wstring Params(const json& r) {
    double p = r.value("params_b", 0.0), a = r.value("active_params_b", 0.0);
    if (p <= 0) return L"";
    wchar_t b[48];
    if (a > 0 && a < p * 0.8) swprintf_s(b, L"%.3gB (%.3gB active)", p, a);  // mixture of experts
    else swprintf_s(b, L"%.3gB", p);
    return b;
}

void SetCell(HWND list, int item, int col, const std::wstring& text) {
    ListView_SetItemText(list, item, col, const_cast<wchar_t*>(text.c_str()));
}

int SelectedIndex(int listId) { return ListView_GetNextItem(Item(listId), -1, LVNI_SELECTED); }

void AddColumns(int listId, std::initializer_list<std::pair<const wchar_t*, int>> cols) {
    HWND list = Item(listId);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    int i = 0;
    for (const auto& [name, width] : cols) {
        LVCOLUMNW c = {LVCF_TEXT | LVCF_WIDTH};
        c.pszText = const_cast<wchar_t*>(name);
        c.cx = S(width);
        ListView_InsertColumn(list, i++, &c);
    }
}

// ---------------------------------------------------------------- results

void FillResults() {
    HWND list = Item(IDC_HF_RESULTS);
    const bool fitOnly = IsDlgButtonChecked(g_hf, IDC_HF_FITONLY) == BST_CHECKED;
    g_results.clear();
    for (const auto& r : g_allResults)
        if (!fitOnly || (!r.value("gated", false) && r.value("fit", "") != "too-large" && r.value("language_model", true)))
            g_results.push_back(r);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    for (size_t i = 0; i < g_results.size(); ++i) {
        const json& r = g_results[i];
        std::wstring repo = Widen(r.value("repo", ""));
        LVITEMW it = {LVIF_TEXT};
        it.iItem = static_cast<int>(i);
        it.pszText = repo.data();
        int at = ListView_InsertItem(list, &it);
        SetCell(list, at, 1, Widen(r.value("category", "")));
        SetCell(list, at, 2, Params(r));
        std::wstring fit = r.value("gated", false) ? L"Gated" : r.contains("fit") ? FitText(r.value("fit", "")) : L"Unknown size";
        SetCell(list, at, 3, fit);
        SetCell(list, at, 4, Count(r.value("downloads", int64_t{0})));
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
}

void StartSearch() {
    wchar_t buf[512];
    GetDlgItemTextW(g_hf, IDC_HF_QUERY, buf, 512);
    std::string q = modelhub::Trim(Narrow(buf));
    int k = static_cast<int>(SendMessageW(Item(IDC_HF_KIND), CB_GETCURSEL, 0, 0));
    int s = static_cast<int>(SendMessageW(Item(IDC_HF_SORT), CB_GETCURSEL, 0, 0));
    const std::string kind = k >= 0 && k < 4 ? kKinds[k][0] : "", sort = s >= 0 && s < 4 ? kSorts[s][0] : "downloads";

    // "owner/repo", "owner/repo/file.gguf" or a huggingface.co address: open that repository directly.
    g_wantFile.clear();
    std::string direct;
    if (auto f = modelhub::ParseHfSpec(q)) {
        direct = f->repo;
        g_wantFile = f->file;
    } else {
        std::string t = q;
        for (const char* prefix : {"https://huggingface.co/", "http://huggingface.co/", "huggingface.co/", "hf.co/"})
            if (t.rfind(prefix, 0) == 0) t.erase(0, strlen(prefix));
        size_t a = t.find('/');
        if (a != std::string::npos && a > 0 && t.find(' ') == std::string::npos) {
            size_t b = t.find('/', a + 1);
            direct = t.substr(0, b);
        }
    }
    const int gen = ++g_searchGen;
    if (!direct.empty()) {
        g_allResults = json::array({{{"repo", direct}, {"category", ""}}});
        FillResults();
        ListView_SetItemState(Item(IDC_HF_RESULTS), 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        return;
    }
    Status(L"Searching Hugging Face...");
    std::thread([q, kind, sort, gen] {
        WorkerGuard guard;
        auto* w = new Work{Work::Search, gen};
        w->result = Call("hf_search", {{"query", q}, {"kind", kind}, {"sort", sort}, {"limit", 50}});
        if (!g_hf || !PostMessageW(g_hf, WM_HF_DONE, 0, reinterpret_cast<LPARAM>(w))) delete w;
    }).detach();
}

// ---------------------------------------------------------------- one repository

const json* SelectedFile() {
    int i = SelectedIndex(IDC_HF_FILES);
    if (i < 0 || !g_repo.is_object() || !g_repo.contains("files") || i >= static_cast<int>(g_repo["files"].size())) return nullptr;
    return &g_repo["files"][i];
}

void UpdateButtons() {
    const json* f = SelectedFile();
    const bool installed = f && f->value("installed", false);
    EnableWindow(Item(IDC_HF_DOWNLOAD), f && f->value("compatible", false) && !installed);
    SetText(IDC_HF_DOWNLOAD, installed ? L"Installed" : L"&Download");
    EnableWindow(Item(IDC_HF_OPEN), g_repo.is_object() && g_repo.contains("url"));
}

void ShowRepo() {
    std::wstring info;
    const json& r = g_repo;
    info = Widen(r.value("repo", "")) + L"\r\n";
    std::wstring facts = Widen(r.value("category", ""));
    auto add = [&](const std::wstring& s) {
        if (!s.empty()) facts += (facts.empty() ? L"" : L"  ·  ") + s;
    };
    add(Params(r).empty() ? L"" : Params(r) + L" parameters");
    add(Widen(r.value("architecture", "")));
    if (int c = r.value("context_k", 0); c > 0) add(std::to_wstring(c) + L"K context");
    add(r.value("license", "").empty() ? L"" : L"licence " + Widen(r.value("license", "")));
    add(Count(r.value("downloads", int64_t{0})) + L" downloads");
    info += facts + L"\r\n\r\nBest for: " + Widen(r.value("best_for", ""));
    if (!r.value("description", "").empty()) info += L"\r\n\r\n" + Widen(r.value("description", ""));
    if (!r.value("base_model", "").empty()) info += L"\r\n\r\nBased on " + Widen(r.value("base_model", "")) + L".";
    for (const auto& n : r.value("notes", json::array())) info += L"\r\n\r\n⚠ " + Widen(n.get<std::string>());
    SetText(IDC_HF_INFO, info);

    HWND list = Item(IDC_HF_FILES);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    int select = -1;
    const json files = r.value("files", json::array());
    for (size_t i = 0; i < files.size(); ++i) {
        const json& f = files[i];
        std::wstring q = Widen(f.value("quant", ""));
        if (q.empty()) q = Widen(f.value("file", ""));
        if (f.value("recommended", false)) q = L"★ " + q;
        LVITEMW it = {LVIF_TEXT};
        it.iItem = static_cast<int>(i);
        it.pszText = q.data();
        int at = ListView_InsertItem(list, &it);
        SetCell(list, at, 1, Widen(modelhub::FormatSize(f.value("size", uint64_t{0}))));
        SetCell(list, at, 2, Widen(f.value("quality", "")));
        SetCell(list, at, 3, FitText(f.value("fit", "")));
        SetCell(list, at, 4, Widen(f.value("speed", "")));
        std::wstring status = f.value("installed", false)        ? L"Installed"
                              : !f.value("compatible", false)    ? Widen(f.value("reason", ""))
                              : f.value("recommended", false)    ? L"Best for this PC"
                                                                 : L"Runs here";
        SetCell(list, at, 5, status);
        if (!g_wantFile.empty() ? f.value("file", "") == g_wantFile : f.value("recommended", false)) select = at;
    }
    if (select >= 0) {
        ListView_SetItemState(list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, select, FALSE);
    }
    g_wantFile.clear();
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    int compatible = r.value("compatible_files", 0);
    Status(std::to_wstring(files.size()) + L" GGUF files, " + std::to_wstring(compatible) + L" of them run on this PC." +
           (r.value("recommended", "").empty() ? L"" : L"  ★ marks the best one for this PC."));
    UpdateButtons();
}

void LoadRepo(const std::string& repo) {
    const int gen = ++g_repoGen;
    g_repo = json();
    ListView_DeleteAllItems(Item(IDC_HF_FILES));
    SetText(IDC_HF_INFO, L"Loading " + Widen(repo) + L"...");
    Status(L"Reading " + Widen(repo) + L" from Hugging Face...");
    UpdateButtons();
    std::thread([repo, gen] {
        WorkerGuard guard;
        auto* w = new Work{Work::Repo, gen, repo};
        w->result = Call("hf_repo", {{"repo", repo}});
        if (!g_hf || !PostMessageW(g_hf, WM_HF_DONE, 0, reinterpret_cast<LPARAM>(w))) delete w;
    }).detach();
}

void DownloadSelected() {
    const json* f = SelectedFile();
    if (!f || !f->value("compatible", false) || f->value("installed", false)) return;
    const std::string repo = g_repo.value("repo", ""), file = f->value("file", "");
    if (f->value("fit", "") == "gpu+cpu" || f->value("fit", "") == "cpu-tight") {
        std::wstring q = L"This file runs " + std::wstring(f->value("fit", "") == "gpu+cpu" ? L"partly on the CPU" : L"close to this PC's memory limit") +
                         L", so it will be slow. Download it anyway?";
        if (MessageBoxW(g_hf, q.c_str(), L"Download", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }
    Status(L"Adding " + Widen(file) + L" to ModelHub...");
    EnableWindow(Item(IDC_HF_DOWNLOAD), FALSE);
    std::thread([repo, file] {
        WorkerGuard guard;
        auto* w = new Work{Work::Add, 0, repo, file};
        w->result = Call("hf_add", {{"repo", repo}, {"file", file}});
        if (!g_hf || !PostMessageW(g_hf, WM_HF_DONE, 0, reinterpret_cast<LPARAM>(w))) delete w;
    }).detach();
}

void OnDone(std::unique_ptr<Work> w) {
    if (w->kind == Work::Search) {
        if (w->generation != g_searchGen) return;
        if (IsError(w->result)) {
            Status(ErrorText(w->result));
            return;
        }
        g_allResults = w->result.value("results", json::array());
        FillResults();
        Status(std::to_wstring(g_results.size()) + L" models. Select one to see what it is best for and which of its files run on this PC.");
    } else if (w->kind == Work::Repo) {
        if (w->generation != g_repoGen) return;
        if (IsError(w->result)) {
            SetText(IDC_HF_INFO, Widen(w->repo) + L"\r\n\r\n" + ErrorText(w->result));
            Status(ErrorText(w->result));
            return;
        }
        g_repo = w->result;
        ShowRepo();
        // A repository opened by name has no search details yet: take them from the repository.
        for (size_t i = 0; i < g_results.size(); ++i)
            if (g_results[i].value("repo", "") == w->repo && g_results[i].value("category", "").empty()) {
                g_results[i] = g_repo;
                HWND list = Item(IDC_HF_RESULTS);
                SetCell(list, static_cast<int>(i), 1, Widen(g_repo.value("category", "")));
                SetCell(list, static_cast<int>(i), 2, Params(g_repo));
                SetCell(list, static_cast<int>(i), 3, g_repo.value("gated", false) ? L"Gated" : FitText(g_repo.value("fit", "")));
                SetCell(list, static_cast<int>(i), 4, Count(g_repo.value("downloads", int64_t{0})));
            }
    } else {
        UpdateButtons();
        if (IsError(w->result)) {
            Status(ErrorText(w->result));
            MessageBoxW(g_hf, ErrorText(w->result).c_str(), L"Download", MB_ICONERROR);
            return;
        }
        const std::string id = w->result.contains("model") ? w->result["model"].value("id", "") : "";
        Status(L"Downloading " + Widen(w->file) + L" as " + Widen(id) +
               L". The ModelHub window shows the progress; the model is shared with every application.");
        DownloadModelById(id);
    }
}

void Layout() {
    RECT rc;
    GetClientRect(g_hf, &rc);
    const int W = rc.right, H = rc.bottom, m = S(10), gap = S(6), bh = S(26), lh = S(18);
    auto place = [](int id, int x, int y, int w, int h) {
        SetWindowPos(Item(id), nullptr, x, y, std::max(0, w), std::max(0, h), SWP_NOZORDER | SWP_NOACTIVATE);
    };
    int y = m;
    const int kindW = S(150), sortW = S(140), searchW = S(84), labelW = S(48);
    int x = W - m - searchW;
    place(IDC_HF_SEARCH, x, y - S(1), searchW, bh);
    x -= gap + sortW;
    place(IDC_HF_SORT, x, y, sortW, S(300));
    x -= gap + kindW;
    place(IDC_HF_KIND, x, y, kindW, S(300));
    place(IDC_HF_LABEL, m, y + S(4), labelW, lh);
    place(IDC_HF_QUERY, m + labelW, y + S(1), x - gap - m - labelW, S(22));
    y += bh + gap;
    place(IDC_HF_FITONLY, m, y, S(320), lh);
    y += lh + gap;

    int bottom = H - m;
    place(IDCANCEL, W - m - S(80), bottom - bh, S(80), bh);
    place(IDC_HF_DOWNLOAD, W - m - S(80) - gap - S(100), bottom - bh, S(100), bh);
    place(IDC_HF_OPEN, W - m - S(80) - S(100) - 2 * gap - S(170), bottom - bh, S(170), bh);
    place(IDC_HF_STATUS, m, bottom - bh + S(5), W - 2 * m - S(80) - S(100) - S(170) - 4 * gap, lh);
    bottom -= bh + gap;

    const int leftW = (W - 2 * m - gap) * 48 / 100;
    place(IDC_HF_RESULTS, m, y, leftW, bottom - y);
    const int rx = m + leftW + gap, rw = W - m - rx, infoH = (bottom - y) * 46 / 100;
    place(IDC_HF_INFO, rx, y, rw, infoH);
    place(IDC_HF_FILES, rx, y + infoH + gap, rw, bottom - y - infoH - gap);
}

INT_PTR CALLBACK Proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_hf = dlg;
            HICON big = nullptr, little = nullptr;
            LoadIconMetric(TheApp().inst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &big);
            LoadIconMetric(TheApp().inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &little);
            SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(little));
            for (const auto& k : kKinds) SendDlgItemMessageW(dlg, IDC_HF_KIND, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Widen(k[1]).c_str()));
            for (const auto& s : kSorts) SendDlgItemMessageW(dlg, IDC_HF_SORT, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Widen(s[1]).c_str()));
            SendDlgItemMessageW(dlg, IDC_HF_KIND, CB_SETCURSEL, 0, 0);
            SendDlgItemMessageW(dlg, IDC_HF_SORT, CB_SETCURSEL, 0, 0);
            SendDlgItemMessageW(dlg, IDC_HF_QUERY, EM_SETCUEBANNER, TRUE,
                                reinterpret_cast<LPARAM>(L"a name (qwen coder, llama, phi...), owner/repo, or a huggingface.co address"));
            CheckDlgButton(dlg, IDC_HF_FITONLY, BST_CHECKED);
            AddColumns(IDC_HF_RESULTS, {{L"Model", 250}, {L"Best for", 104}, {L"Size", 84}, {L"On this PC (Q4)", 116}, {L"Downloads", 66}});
            AddColumns(IDC_HF_FILES, {{L"File", 100}, {L"Size", 60}, {L"Quality", 200}, {L"Runs on", 124}, {L"Speed", 62}, {L"Status", 190}});
            RECT work;
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            int w = std::min<int>(S(1280), work.right - work.left), h = std::min<int>(S(740), work.bottom - work.top);
            SetWindowPos(dlg, nullptr, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h,
                         SWP_NOZORDER);
            Layout();
            UpdateButtons();
            StartSearch();
            return TRUE;
        }
        case WM_SIZE: Layout(); return TRUE;
        case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {S(900), S(520)}; return TRUE;
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(dlg, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return TRUE;
        }
        case WM_HF_DONE: OnDone(std::unique_ptr<Work>(reinterpret_cast<Work*>(lp))); return TRUE;
        case WM_NOTIFY: {
            auto* h = reinterpret_cast<NMHDR*>(lp);
            if (h->idFrom == IDC_HF_RESULTS && h->code == LVN_ITEMCHANGED) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(lp);
                if ((nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED) && nm->iItem >= 0 &&
                    nm->iItem < static_cast<int>(g_results.size()))
                    LoadRepo(g_results[nm->iItem].value("repo", ""));
            } else if (h->idFrom == IDC_HF_FILES && h->code == LVN_ITEMCHANGED) {
                UpdateButtons();
            } else if (h->idFrom == IDC_HF_FILES && h->code == NM_DBLCLK) {
                DownloadSelected();
            }
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_HF_SEARCH: StartSearch(); break;
                case IDOK: StartSearch(); break;  // Enter in the search box
                case IDC_HF_KIND:
                case IDC_HF_SORT:
                    if (HIWORD(wp) == CBN_SELCHANGE) StartSearch();
                    break;
                case IDC_HF_FITONLY: FillResults(); break;
                case IDC_HF_DOWNLOAD: DownloadSelected(); break;
                case IDC_HF_OPEN:
                    if (g_repo.is_object() && g_repo.contains("url"))
                        ShellExecuteW(dlg, L"open", Widen(g_repo.value("url", "")).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case IDCANCEL: DestroyWindow(dlg); break;
            }
            return TRUE;
        case WM_CLOSE: DestroyWindow(dlg); return TRUE;
        case WM_DESTROY:
            g_hf = nullptr;
            ++g_searchGen;
            ++g_repoGen;
            RefreshManager();  // models may have been added
            return TRUE;
    }
    return FALSE;
}

}  // namespace

void ShowHfBrowser(HWND owner) {
    if (!g_hf) CreateDialogParamW(TheApp().inst, MAKEINTRESOURCEW(IDD_HF_BROWSER), owner, Proc, 0);
    if (!g_hf) return;
    if (IsIconic(g_hf)) ShowWindow(g_hf, SW_RESTORE);
    ShowWindow(g_hf, SW_SHOW);
    SetForegroundWindow(g_hf);
}

HWND HfBrowserWindow() { return g_hf; }

}  // namespace mgr
