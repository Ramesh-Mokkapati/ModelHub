#include "ChatDlg.h"

#include "AppMessages.h"
#include "ClaudeSetup.h"
#include "Clipboard.h"
#include "Dialogs.h"
#include "Providers.h"
#include "Router.h"
#include "Tools.h"
#include "Util.h"

#include <algorithm>
#include <chrono>
#include <future>

namespace chat
{

namespace
{

constexpr COLORREF kSidebar = RGB(247, 247, 248);
constexpr COLORREF kText = RGB(32, 33, 36);
constexpr COLORREF kMuted = RGB(110, 110, 118);
constexpr COLORREF kAccent = RGB(37, 99, 235);
constexpr COLORREF kCodeBack = RGB(243, 244, 246);
constexpr DWORD_PTR kGroupItem = static_cast<DWORD_PTR>(-1);

const char* const kReasoningIds[] = { "default", "off", "low", "medium", "high" };
const wchar_t* const kReasoningLabels[] = { L"Default", L"Off", L"Low", L"Medium", L"High" };

struct UiEvent
{
    enum class Kind
    {
        AssistantBegin,
        Delta,
        AssistantDone,
        ToolStart,
        ToolResult,
        ToolNote,  // a tool call Copilot made inside its own loop: shown, not stored
        Status,
        Route,
        Finished
    } kind;

    DeltaKind delta = DeltaKind::Content;
    std::string text;
    json data;
};

struct ConfirmRequest
{
    std::string title;
    std::string detail;
    std::promise<bool> answer;
};

struct DownloadEvent
{
    std::string id;
    std::string text;
    bool finished = false;
    bool ok = false;
};

std::string FormatGB(unsigned long long bytes)
{
    char buf[32];
    if (bytes >= (1ull << 30))
    {
        std::snprintf(buf, sizeof buf, "%.1f GB", bytes / double(1ull << 30));
    }
    else
    {
        std::snprintf(buf, sizeof buf, "%.0f MB", bytes / double(1ull << 20));
    }

    return buf;
}

struct ModelsResult
{
    int request = 0;
    bool ok = false;
    std::vector<std::string> ids;
    std::string error;
    std::string label;
    std::vector<std::pair<std::string, std::vector<std::string>>> providerModels;  // provider id, its models
    std::vector<std::string> providerErrors;
    bool startedHub = false;  // ModelHub was not running: this window started it
};

void Post(HWND hwnd, UiEvent* ev)
{
    if (!::PostMessage(hwnd, WM_APP_EVENT, 0, reinterpret_cast<LPARAM>(ev)))
    {
        delete ev;
    }
}

void PostStatus(HWND hwnd, const std::string& text)
{
    Post(hwnd, new UiEvent{ UiEvent::Kind::Status, DeltaKind::Content, text, json() });
}

// What goes to the server: no display-only fields.
json ForServer(const json& m)
{
    json out = json::object();
    for (auto it = m.begin(); it != m.end(); ++it)
    {
        if ((it.key().rfind("x_", 0) != 0 || it.key() == "x_anthropic") && it.key() != "reasoning_content")
        {
            out[it.key()] = it.value();
        }
    }

    return out;
}

struct Job
{
    HWND hwnd = nullptr;
    Settings settings;
    std::string model;
    json messages = json::array();  // with the system message
    json tools = json::array();
    std::map<std::string, std::pair<std::shared_ptr<McpServer>, std::string>> mcpRoutes;
    std::wstring workspace;
    std::shared_ptr<CancelToken> cancel;
    bool autoMode = false;
    RouteRequest routeReq;
    std::vector<std::string> models;  // the models Auto mode may use
    std::set<std::string> cloudProviders;
};

using McpRoutes = std::map<std::string, std::pair<std::shared_ptr<McpServer>, std::string>>;

// One tool call: a built-in tool or an MCP server's.
std::string RunTool(const std::string& name, const json& args, const ToolContext& ctx, const McpRoutes& routes,
    CancelToken* cancel)
{
    if (IsBuiltinTool(name))
    {
        return RunBuiltinTool(name, args, ctx);
    }

    if (auto it = routes.find(name); it != routes.end())
    {
        bool isError = false;
        std::string result = it->second.first->CallTool(it->second.second, args, cancel, isError);
        return isError && result.rfind("Error", 0) != 0 ? "Error: " + result : result;
    }

    return "Error: there is no tool named " + name;
}

void RunJob(const std::shared_ptr<Job>& job)
{
    HWND hwnd = job->hwnd;
    CancelToken* cancel = job->cancel.get();
    auto finish = [hwnd](const std::string& error)
    { Post(hwnd, new UiEvent{ UiEvent::Kind::Finished, DeltaKind::Content, error, json() }); };

    PostStatus(hwnd, "Connecting...");
    Endpoint ep;
    std::string error, providerId, providerModel;
    bool localNeeded = job->autoMode || !SplitQualified(job->model, providerId, providerModel);
    bool localOk = localNeeded && ResolveEndpoint(job->settings, ep, error, [hwnd](const std::string& m)
                                      { PostStatus(hwnd, m); });
    if (localNeeded && !localOk)
    {
        if (!job->autoMode)
        {
            return finish(error);
        }

        // Auto mode without ModelHub (or the custom server): the providers' models only.
        job->models.erase(std::remove_if(job->models.begin(), job->models.end(),
                              [](const std::string& m)
                              { return m.find(kProviderSep) == std::string::npos; }),
            job->models.end());

        if (job->models.empty())
        {
            return finish(error);
        }
    }

    if (job->autoMode)
    {
        PostStatus(hwnd, "Auto: choosing a model...");
        RouteResult rr = Route(ep, job->settings, job->routeReq, job->models, cancel, [hwnd](const std::string& s)
            { PostStatus(hwnd, s); }, job->cloudProviders);

        if (!rr.error.empty())
        {
            return finish(rr.error == "cancelled" ? "stopped" : rr.error);
        }

        job->model = rr.model;
        if (ep.modelhub && rr.contextTokens > 8192)  // a long conversation: a server with room for it
        {
            ep.headers.push_back({ "X-ModelHub-Context", std::to_string(rr.contextTokens) });
        }

        json route = { { "model", rr.model }, { "category", CategoryId(rr.category) }, { "reason", rr.reason } };
        if (job->settings.autoSuggestDownloads && ep.modelhub && !rr.suggestion.id.empty())
        {
            route["suggest"] = { { "id", rr.suggestion.id }, { "size", rr.suggestion.sizeBytes }, { "why", rr.suggestion.why } };
        }

        Post(hwnd, new UiEvent{ UiEvent::Kind::Route, DeltaKind::Content, "", route });
    }

    // A provider's model (Claude, OpenAI, Ollama, ...): its own endpoint, and the model's own name.
    std::string requestModel = job->model;
    if (SplitQualified(job->model, providerId, providerModel))
    {
        const ProviderConfig* p = job->settings.FindProvider(providerId);
        if (!p)
        {
            return finish("the provider of " + providerModel + " is no longer set up (Model Providers)");
        }

        if (!ProviderEndpoint(*p, ep, error))
        {
            return finish(error);
        }

        requestModel = providerModel;
    }

    const std::string shownModel = DisplayModel(job->settings, job->model);

    ToolContext ctx;
    ctx.workspace = job->workspace;
    ctx.shell = job->settings.shell;
    ctx.confirmShell = job->settings.confirmShell;
    ctx.confirmWrites = job->settings.confirmWrites;
    ctx.cancel = cancel;
    ctx.confirm = [hwnd, cancel](const std::string& title, const std::string& detail)
    {
        auto req = std::make_shared<ConfirmRequest>();
        req->title = title;
        req->detail = detail;
        std::future<bool> answer = req->answer.get_future();
        auto* p = new std::shared_ptr<ConfirmRequest>(req);

        if (!::PostMessage(hwnd, WM_APP_CONFIRM, 0, reinterpret_cast<LPARAM>(p)))
        {
            delete p;
            return false;
        }

        req.reset();
        while (answer.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        {
            if (cancel->IsCancelled())
            {
                return false;
            }
        }

        try
        {
            return answer.get();
        }
        catch (...)
        {
            return false;
        }
    };

    json messages = job->messages;
    for (int round = 0;; ++round)
    {
        if (round >= job->settings.maxToolRounds)
        {
            return finish("stopped after " + std::to_string(round) + " tool rounds (Settings: most tool rounds)");
        }

        json req = { { "model", requestModel }, { "stream", true }, { "messages", messages } };
        if (!job->tools.empty())
        {
            req["tools"] = job->tools;
        }

        const std::string& reasoning = job->settings.reasoning;
        if (reasoning == "off")
        {
            req["chat_template_kwargs"] = { { "enable_thinking", false } };
        }
        else if (reasoning == "low" || reasoning == "medium" || reasoning == "high")
        {
            req["reasoning_effort"] = reasoning;
            req["chat_template_kwargs"] = { { "reasoning_effort", reasoning } };
        }

        if (!job->settings.temperature.empty() && ep.kind != "copilot" && ep.kind != "codex")
        {
            req["temperature"] = std::strtod(job->settings.temperature.c_str(), nullptr);
        }

        if (ep.kind == "copilot" || ep.kind == "codex")
        {
            req["x_workspace"] = Utf8(job->workspace);  // the agent works in the conversation's folder
        }

        Post(hwnd, new UiEvent{ UiEvent::Kind::AssistantBegin, DeltaKind::Content, job->model, json() });
        PostStatus(hwnd, "Waiting for " + shownModel +
                             (round == 0 && ep.modelhub ? " (loading a model can take a minute)..." : "..."));
        bool first = true;
        // GitHub Copilot runs its tool loop itself and calls the app's tools through this; each call is shown in the
        // transcript (Copilot keeps the loop's messages in its own session).
        ToolRunner copilotTools = [&](const std::string& name, const json& args)
        {
            PostStatus(hwnd, "Running " + name + "...");
            std::string result = RunTool(name, args, ctx, job->mcpRoutes, cancel);
            json note = { { "role", "tool" }, { "content", Truncate(result, 40000) }, { "x_name", name },
                { "x_arguments", args.dump() } };
            Post(hwnd, new UiEvent{ UiEvent::Kind::ToolNote, DeltaKind::Content, "", note });
            return Truncate(result, 40000);
        };

        StreamResult res = StreamChat(ep, req, [&](DeltaKind kind, const std::string& text)
            {
                if (first)
                {
                    first = false;
                    PostStatus(hwnd, "Generating...");
                }
                Post(hwnd, new UiEvent{ UiEvent::Kind::Delta, kind, text, json() }); }, cancel, copilotTools);

        json msg = { { "role", "assistant" }, { "content", res.content }, { "x_model", job->model } };
        if (!res.reasoning.empty())
        {
            msg["reasoning_content"] = res.reasoning;
        }

        if (!res.toolCalls.empty() && !res.cancelled && res.error.empty())
        {
            msg["tool_calls"] = res.toolCalls;
        }

        if (res.tokensPerSecond > 0)
        {
            msg["x_tps"] = res.tokensPerSecond;
        }

        if (res.providerContent.is_array() && !res.providerContent.empty())
        {
            msg["x_anthropic"] = res.providerContent;  // Claude's blocks (thinking signatures, tool_use) for replay
        }

        bool partial = !res.content.empty() || !res.reasoning.empty();
        if (res.cancelled || !res.error.empty())
        {
            if (partial)
            {
                msg["x_stopped"] = true;
                Post(hwnd, new UiEvent{ UiEvent::Kind::AssistantDone, DeltaKind::Content, "", msg });
            }

            return finish(res.cancelled ? "stopped" : res.error);
        }

        Post(hwnd, new UiEvent{ UiEvent::Kind::AssistantDone, DeltaKind::Content, "", msg });
        messages.push_back(ForServer(msg));
        if (res.toolCalls.empty())
        {
            std::string done = "Done";
            if (res.tokensPerSecond > 0)
            {
                char buf[64];
                std::snprintf(buf, sizeof buf, " - %d tokens, %.1f tokens/s", res.completionTokens, res.tokensPerSecond);
                done += buf;
            }

            PostStatus(hwnd, done);
            return finish("");
        }

        for (const auto& call : res.toolCalls)
        {
            std::string id = call.value("id", "");
            std::string name = call["function"].value("name", "");
            std::string argText = call["function"].value("arguments", "");
            json args = json::parse(TrimCopy(argText).empty() ? "{}" : argText, nullptr, false);
            Post(hwnd, new UiEvent{ UiEvent::Kind::ToolStart, DeltaKind::Content, name, { { "arguments", argText } } });
            PostStatus(hwnd, "Running " + name + "...");
            std::string result = args.is_discarded() || !args.is_object()
                                     ? "Error: the arguments are not a JSON object: " + Truncate(argText, 300)
                                     : RunTool(name, args, ctx, job->mcpRoutes, cancel);
            json toolMsg = { { "role", "tool" }, { "tool_call_id", id }, { "content", Truncate(result, 40000) },
                { "x_name", name } };
            Post(hwnd, new UiEvent{ UiEvent::Kind::ToolResult, DeltaKind::Content, "", toolMsg });
            messages.push_back(ForServer(toolMsg));
            if (cancel->IsCancelled())
            {
                return finish("stopped");
            }
        }
    }
}

std::wstring FileName(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::string ImageMime(const std::wstring& path)
{
    std::wstring ext = path.substr(path.find_last_of(L'.') == std::wstring::npos ? path.size() : path.find_last_of(L'.'));
    for (auto& c : ext)
    {
        c = static_cast<wchar_t>(towlower(c));
    }

    if (ext == L".png")
    {
        return "image/png";
    }

    if (ext == L".jpg" || ext == L".jpeg")
    {
        return "image/jpeg";
    }

    if (ext == L".gif")
    {
        return "image/gif";
    }

    if (ext == L".webp")
    {
        return "image/webp";
    }

    if (ext == L".bmp")
    {
        return "image/bmp";
    }

    return {};
}

std::wstring Crlf(const std::string& s)
{
    std::wstring w = Wide(s);
    std::wstring out;
    out.reserve(w.size());
    for (wchar_t c : w)
    {
        if (c != L'\r')
        {
            out += c;
        }
    }

    return out;
}

}  // namespace

BEGIN_MESSAGE_MAP(CTranscriptCtrl, CRichEditCtrl)
    ON_WM_SETCURSOR()
    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
END_MESSAGE_MAP()

long CTranscriptCtrl::HitChar(CPoint pt)
{
    int cp = CharFromPos(pt);
    if (cp < 0)
    {
        return -1;
    }

    CPoint a = PosFromChar(cp), b = PosFromChar(cp + 1);
    int lineH = 24;
    if (CDC* dc = GetDC())
    {
        lineH = dc->GetDeviceCaps(LOGPIXELSY) / 4;  // about a line of 11 pt text
        ReleaseDC(dc);
    }

    int right = b.y == a.y && b.x > a.x ? b.x : a.x + lineH / 2;
    return pt.x >= a.x && pt.x < right && pt.y >= a.y && pt.y < a.y + lineH ? cp : -1;
}

BOOL CTranscriptCtrl::OnSetCursor(CWnd* wnd, UINT hitTest, UINT message)
{
    CPoint pt;
    GetCursorPos(&pt);
    ScreenToClient(&pt);
    long cp = hitTest == HTCLIENT ? HitChar(pt) : -1;
    if (cp >= 0 && isLink && isLink(cp))
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_HAND));
        return TRUE;
    }

    return CRichEditCtrl::OnSetCursor(wnd, hitTest, message);
}

void CTranscriptCtrl::OnLButtonDown(UINT flags, CPoint pt)
{
    downChar_ = HitChar(pt);
    CRichEditCtrl::OnLButtonDown(flags, pt);
}

void CTranscriptCtrl::OnLButtonUp(UINT flags, CPoint pt)
{
    CRichEditCtrl::OnLButtonUp(flags, pt);
    long cp = HitChar(pt);
    long a = 0, b = 0;
    GetSel(a, b);
    bool dragged = b > a + 1;  // selecting text is not a click
    if (cp >= 0 && cp == downChar_ && !dragged && isLink && isLink(cp) && onClick)
    {
        onClick(cp);
    }

    downChar_ = -1;
}

BEGIN_MESSAGE_MAP(CCueEdit, CEdit)
    ON_WM_PAINT()
    ON_CONTROL_REFLECT_EX(EN_CHANGE, &CCueEdit::OnChange)
    ON_MESSAGE(WM_PASTE, &CCueEdit::OnPasteMessage)
END_MESSAGE_MAP()

LRESULT CCueEdit::OnPasteMessage(WPARAM, LPARAM)
{
    if (onPaste && onPaste())
    {
        return 0;
    }

    return Default();
}

void CCueEdit::OnPaint()
{
    Default();
    if (GetWindowTextLength() > 0 || cue.IsEmpty())
    {
        return;
    }

    CClientDC dc(this);
    CRect rc;
    GetRect(&rc);
    CFont* old = dc.SelectObject(GetFont());
    dc.SetTextColor(RGB(140, 140, 148));
    dc.SetBkMode(TRANSPARENT);
    dc.DrawText(cue, rc, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
    dc.SelectObject(old);
}

BOOL CCueEdit::OnChange()
{
    Invalidate();
    return FALSE;  // the parent sees EN_CHANGE too
}

BEGIN_MESSAGE_MAP(CChatDlg, CDialogEx)
    ON_WM_SIZE()
    ON_WM_LBUTTONDOWN()
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_SETCURSOR()
    ON_WM_GETMINMAXINFO()
    ON_WM_ERASEBKGND()
    ON_WM_DRAWITEM()
    ON_WM_CTLCOLOR()
    ON_WM_CLOSE()
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_WM_DROPFILES()
    ON_BN_CLICKED(IDC_NEW_CHAT, &CChatDlg::OnNewChat)
    ON_EN_CHANGE(IDC_SEARCH, &CChatDlg::OnSearchChange)
    ON_EN_CHANGE(IDC_INPUT, &CChatDlg::OnInputChange)
    ON_BN_CLICKED(IDC_MIC, &CChatDlg::OnMic)
    ON_BN_CLICKED(IDC_MCP_SERVERS, &CChatDlg::OnMcpServers)
    ON_BN_CLICKED(IDC_PROVIDERS, &CChatDlg::OnProviders)
    ON_BN_CLICKED(IDC_SETTINGS, &CChatDlg::OnSettings)
    ON_BN_CLICKED(IDC_WORKSPACE_CHANGE, &CChatDlg::OnWorkspaceChange)
    ON_BN_CLICKED(IDC_CLEAR_ATTACHMENTS, &CChatDlg::OnClearAttachments)
    ON_BN_CLICKED(IDC_PLUS, &CChatDlg::OnPlus)
    ON_CBN_SELCHANGE(IDC_MODEL, &CChatDlg::OnModelChange)
    ON_BN_CLICKED(IDC_REFRESH_MODELS, &CChatDlg::OnRefreshModels)
    ON_BN_CLICKED(IDC_SEND, &CChatDlg::OnSend)
    ON_NOTIFY(TVN_SELCHANGED, IDC_CONVERSATIONS, &CChatDlg::OnTreeSelChanged)
    ON_NOTIFY(NM_RCLICK, IDC_CONVERSATIONS, &CChatDlg::OnTreeRClick)
    ON_MESSAGE(WM_APP_EVENT, &CChatDlg::OnAppEvent)
    ON_MESSAGE(WM_APP_MODELS, &CChatDlg::OnModels)
    ON_MESSAGE(WM_APP_MCP_CHANGED, &CChatDlg::OnMcpChanged)
    ON_MESSAGE(WM_APP_CONFIRM, &CChatDlg::OnConfirm)
    ON_MESSAGE(WM_APP_DOWNLOAD, &CChatDlg::OnDownloadProgress)
    ON_MESSAGE(WM_APP_IMPORT, &CChatDlg::OnImportProviders)
    ON_MESSAGE(WM_APP_MIGRATED, &CChatDlg::OnMigrated)
END_MESSAGE_MAP()

CChatDlg::CChatDlg()
    : CDialogEx(IDD_MAIN)
{
}

CChatDlg::~CChatDlg() = default;

int CChatDlg::Px(int v) const
{
    UINT dpi = m_hWnd ? GetDpiForWindow(m_hWnd) : 96;
    return MulDiv(v, static_cast<int>(dpi), 96);
}

// ---------------------------------------------------------------- setup

void CChatDlg::CreateFonts()
{
    auto make = [this](CFont& f, int pt, int weight, const wchar_t* face)
    {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(pt, static_cast<int>(GetDpiForWindow(m_hWnd)), 72);
        lf.lfWeight = weight;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, face);
        f.DeleteObject();
        f.CreateFontIndirectW(&lf);
    };

    make(fontTitle_, 12, FW_SEMIBOLD, L"Segoe UI");
    make(fontHello_, 24, FW_BOLD, L"Segoe UI");
    make(fontHelloSub_, 12, FW_NORMAL, L"Segoe UI");
    make(fontInput_, 11, FW_NORMAL, L"Segoe UI");
    make(fontSidebar_, 10, FW_NORMAL, L"Segoe UI");
    GetDlgItem(IDC_TITLE)->SetFont(&fontTitle_);
    GetDlgItem(IDC_HELLO)->SetFont(&fontHello_);
    GetDlgItem(IDC_HELLO_SUB)->SetFont(&fontHelloSub_);
    input_.SetFont(&fontInput_);
    tree_.SetFont(&fontSidebar_);
    for (int id : { IDC_NEW_CHAT, IDC_MCP_SERVERS, IDC_PROVIDERS, IDC_SETTINGS, IDC_SEARCH })
    {
        GetDlgItem(id)->SetFont(&fontSidebar_);
    }
}

BOOL CChatDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    icon_ = AfxGetApp()->LoadIcon(IDI_APP);
    SetIcon(icon_, TRUE);
    SetIcon(icon_, FALSE);

    settings_.Load();
    if (settings_.defaultWorkspace.empty())
    {
        settings_.defaultWorkspace = Utf8(UserProfileDir());
    }

    if (settings_.model.empty())
    {
        settings_.model = "auto";
    }

    transcript_.SubclassDlgItem(IDC_TRANSCRIPT, this);
    tree_.SubclassDlgItem(IDC_CONVERSATIONS, this);
    input_.SubclassDlgItem(IDC_INPUT, this);
    search_.SubclassDlgItem(IDC_SEARCH, this);
    model_.SubclassDlgItem(IDC_MODEL, this);

    sidebarBrush_.CreateSolidBrush(kSidebar);
    whiteBrush_.CreateSolidBrush(RGB(255, 255, 255));
    CreateFonts();

    search_.SetCueBanner(L"Search");
    input_.cue = L"Message ModelHub...   (Shift+Enter: new line; paste or drop images and files)";
    input_.onPaste = [this]()
    { return PasteFromClipboard(); };
    SetDlgItemText(IDC_REFRESH_MODELS, L"↻");
    SetDlgItemText(IDC_PLUS, L"+");
    tree_.SetBkColor(kSidebar);
    tree_.SetItemHeight(static_cast<SHORT>(Px(26)));
    tree_.SetIndent(Px(14));

    transcript_.SetBackgroundColor(FALSE, RGB(255, 255, 255));
    transcript_.isLink = [this](long cp)
    {
        for (const auto& t : links_)
        {
            if (cp >= t.cpMin && cp < t.cpMax)
            {
                return true;
            }
        }

        return false;
    };

    transcript_.onClick = [this](long cp)
    { OnTranscriptClick(cp); };
    transcript_.SendMessage(EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(Px(8), Px(8)));
    transcript_.SetTargetDevice(nullptr, 0);  // wrap to the window

    // The panels replace the dialog's own sidebar controls and the workspace line.
    for (int id : { IDC_TITLE, IDC_NEW_CHAT, IDC_SEARCH, IDC_MCP_SERVERS, IDC_PROVIDERS, IDC_SETTINGS, IDC_RECENT_LABEL,
             IDC_CONVERSATIONS, IDC_WORKSPACE, IDC_WORKSPACE_CHANGE })
    {
        GetDlgItem(id)->ShowWindow(SW_HIDE);
    }

    sidebar_.CreatePanel(this, 2001);
    info_.CreatePanel(this, 2002);
    titleStrip_.CreatePanel(this, 2003);
    footer_.CreatePanel(this, 2004);

    // The status line, the model list and its refresh button live in the footer now; + and Send are round icons.
    for (int id : { IDC_STATUS, IDC_MODEL, IDC_REFRESH_MODELS })
    {
        GetDlgItem(id)->ShowWindow(SW_HIDE);
    }

    for (int id : { IDC_PLUS, IDC_SEND })
    {
        GetDlgItem(id)->ModifyStyle(BS_TYPEMASK, BS_OWNERDRAW);
    }

    mic_.Create(L"Voice typing", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, CRect(0, 0, 1, 1), this, IDC_MIC);
    footer_.onClick = [this](int part, CPoint screen)
    {
        if (part == CFooterBar::PartModel)
        {
            ShowModelMenu(screen);
        }
        else if (part == CFooterBar::PartEffort)
        {
            ShowEffortMenu(screen);
        }
        else
        {
            RefreshModels();
        }
    };

    sidebar_.onNav = [this](int nav, size_t data, bool right, CPoint screen)
    {
        switch (nav)
        {
        case CSidebarPanel::NavNewChat:
            OnNewChat();
            break;
        case CSidebarPanel::NavMcp:
            OnMcpServers();
            break;
        case CSidebarPanel::NavProviders:
            OnProviders();
            break;
        case CSidebarPanel::NavSettings:
            OnSettings();
            break;
        case CSidebarPanel::NavCollapse:
            settings_.sidebarVisible = false;
            settings_.Save();
            Layout();
            break;
        case CSidebarPanel::NavConversation:
            if (right && data < treeItems_.size())
            {
                ShowConversationMenu(treeItems_[data], screen);
            }
            else
            {
                SelectConversation(data);
            }
            break;
        }
    };

    sidebar_.onSearch = [this]()
    { FillTree(); };
    info_.onAction = [this](int action, size_t data)
    {
        const auto& d = info_.GetData();
        if (action == CInfoPanel::ActChangeFolder && !busy_)
        {
            OnWorkspaceChange();
        }
        else if (action == CInfoPanel::ActOpenFolder && current_)
        {
            ShellExecuteW(m_hWnd, L"open", WorkspaceOf(*current_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (action == CInfoPanel::ActOpenChange && data < d.changes.size())
        {
            std::wstring args = L"/select,\"" + d.changes[data].second + L"\"";
            ShellExecuteW(m_hWnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        else if (action == CInfoPanel::ActClose)
        {
            settings_.infoVisible = false;
            settings_.Save();
            Layout();
        }
    };

    titleStrip_.onButton = [this](int button, CPoint screen)
    { OnToolbar(button, screen); };

    // The message box sits in the composer card: no frame of its own.
    input_.ModifyStyle(WS_BORDER | WS_VSCROLL, 0, SWP_FRAMECHANGED);
    input_.ModifyStyleEx(WS_EX_CLIENTEDGE, 0, SWP_FRAMECHANGED);

    // Size: most of the work area, centered.
    CRect work;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min(work.Width() * 85 / 100, Px(1400)), h = std::min(work.Height() * 85 / 100, Px(900));
    SetWindowPos(nullptr, work.left + (work.Width() - w) / 2, work.top + (work.Height() - h) / 2, w, h, SWP_NOZORDER);

    conversations_ = LoadConversations();
    if (!conversations_.empty())
    {
        OpenConversation(conversations_.front());
    }
    else
    {
        OpenConversation(NewConversation());
    }

    FillTree();
    UpdateAttachments();
    SetBusy(false);
    ApplyMcp();
    RefreshModels();
    if (settings_.autoImportProviders)
    {
        // Model providers of installed applications (GitHub Copilot's accounts, LM Studio, Ollama, ...), in the
        // background: it runs a few command-line tools and asks local servers for their models.
        HWND hwnd = m_hWnd;
        auto detect = [hwnd]()
        {
            auto* d = new Detection(DetectProviders());
            if (!::PostMessage(hwnd, WM_APP_IMPORT, 0, reinterpret_cast<LPARAM>(d)))
            {
                delete d;
            }
        };

        std::thread(detect).detach();
    }

    Layout();
    input_.SetFocus();
    return FALSE;
}

void CChatDlg::OnOK()
{
    // Enter is handled in PreTranslateMessage; the dialog does not close on it.
}

void CChatDlg::OnCancel()
{
    if (busy_ && cancel_)
    {
        cancel_->Cancel();  // Escape stops a running answer; it does not close the window
    }
}

void CChatDlg::OnClose()
{
    if (cancel_)
    {
        cancel_->Cancel();
    }

    EndDialog(IDCANCEL);
}

void CChatDlg::OnDestroy()
{
    if (cancel_)
    {
        cancel_->Cancel();
    }

    if (worker_.joinable())
    {
        worker_.join();
    }

    // Events the worker posted after the window stopped reading them.
    MSG msg;
    while (::PeekMessage(&msg, m_hWnd, WM_APP_EVENT, WM_APP_MIGRATED, PM_REMOVE))
    {
        if (msg.message == WM_APP_EVENT)
        {
            delete reinterpret_cast<UiEvent*>(msg.lParam);
        }
        else if (msg.message == WM_APP_MODELS)
        {
            delete reinterpret_cast<ModelsResult*>(msg.lParam);
        }
        else if (msg.message == WM_APP_CONFIRM)
        {
            delete reinterpret_cast<std::shared_ptr<ConfirmRequest>*>(msg.lParam);
        }
        else if (msg.message == WM_APP_DOWNLOAD)
        {
            delete reinterpret_cast<DownloadEvent*>(msg.lParam);
        }
        else if (msg.message == WM_APP_IMPORT)
        {
            delete reinterpret_cast<Detection*>(msg.lParam);
        }
        else if (msg.message == WM_APP_MIGRATED)
        {
            delete reinterpret_cast<MigrationResult*>(msg.lParam);
        }
    }

    SaveCurrent();
    settings_.Save();
    mcp_.Shutdown();
    CDialogEx::OnDestroy();
}

BOOL CChatDlg::PreTranslateMessage(MSG* msg)
{
    if (msg->message == WM_KEYDOWN)
    {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (msg->wParam == VK_RETURN && msg->hwnd == input_.m_hWnd && !shift && !ctrl)
        {
            if (!busy_)
            {
                OnSend();
            }

            return TRUE;
        }

        if (msg->wParam == VK_RETURN && msg->hwnd == sidebar_.SearchHwnd())
        {
            return TRUE;
        }

        if (msg->wParam == 'N' && ctrl)
        {
            OnNewChat();
            return TRUE;
        }

        if (msg->wParam == 'A' && ctrl && msg->hwnd == input_.m_hWnd)
        {
            input_.SetSel(0, -1);
            return TRUE;
        }

        // Ctrl+V / Shift+Insert: images, copied files and long text become attachments; short text pastes as usual.
        bool paste = (msg->wParam == 'V' && ctrl && !shift) || (msg->wParam == VK_INSERT && shift && !ctrl);
        if (paste && msg->hwnd == input_.m_hWnd && PasteFromClipboard())
        {
            return TRUE;
        }
    }

    return CDialogEx::PreTranslateMessage(msg);
}

// ---------------------------------------------------------------- look and layout

void CChatDlg::OnGetMinMaxInfo(MINMAXINFO* mmi)
{
    mmi->ptMinTrackSize = { Px(820), Px(520) };
    CDialogEx::OnGetMinMaxInfo(mmi);
}

void CChatDlg::OnSize(UINT type, int cx, int cy)
{
    CDialogEx::OnSize(type, cx, cy);
    if (transcript_.m_hWnd)
    {
        Layout();
    }
}

void CChatDlg::Layout()
{
    CRect rc;
    GetClientRect(&rc);
    auto place = [this](int id, int x, int y, int w, int h)
    {
        if (CWnd* c = GetDlgItem(id))
        {
            c->SetWindowPos(nullptr, x, y, std::max(0, w), std::max(0, h), SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    const int edge = Px(6);  // the draggable gap beside each panel

    // Left panel, right panel (both resizable and collapsible), and the centre between them.
    int sideW = settings_.sidebarVisible ? std::min(Px(settings_.sidebarWidth), rc.Width() / 3) : 0;
    int infoW = settings_.infoVisible ? std::min(Px(settings_.infoWidth), rc.Width() / 3) : 0;
    sidebar_.SetWindowPos(nullptr, 0, 0, sideW, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE | (sideW ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    info_.SetWindowPos(nullptr, rc.right - infoW, 0, infoW, rc.bottom,
        SWP_NOZORDER | SWP_NOACTIVATE | (infoW ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    sidebarEdge_ = sideW ? CRect(sideW, 0, sideW + edge, rc.bottom) : CRect();
    infoEdge_ = infoW ? CRect(rc.right - infoW - edge, 0, rc.right - infoW, rc.bottom) : CRect();
    int centerL = sideW + (sideW ? edge : 0), centerR = rc.right - infoW - (infoW ? edge : 0);
    titleStrip_.SetWindowPos(nullptr, centerL, 0, centerR - centerL, Px(44), SWP_NOZORDER | SWP_NOACTIVATE);
    titleStrip_.SetState(current_ && !current_->messages.empty() ? Wide(current_->title) : L"New chat", sideW > 0, infoW > 0);

    // The conversation column: at most 860 px wide, centred, like Claude's.
    int colW = std::min(centerR - centerL - Px(48), Px(860));
    int cx = centerL + (centerR - centerL - colW) / 2;

    // The composer card (attachments, the message box, then + on the left and Send on the right), and below it the
    // footer: the status, the model, the effort and the connection.
    int bottom = rc.bottom - Px(6);
    int footerH = Px(28);
    footer_.SetWindowPos(nullptr, cx, bottom - footerH, colW, footerH, SWP_NOZORDER | SWP_NOACTIVATE);
    bottom -= footerH + Px(4);
    bool hasAttachments = !attachments_.empty();
    const int btn = Px(32), inner = Px(9), gap = Px(4);
    int inputL = cx + Px(10) + btn + Px(8), inputR = cx + colW - Px(10) - 2 * btn - gap - Px(8);

    // The message box is as tall as its text (wrapped lines count), from one line up to eight.
    int lineH = Px(20);
    {
        CClientDC dc(&input_);
        CFont* old = dc.SelectObject(&fontInput_);
        TEXTMETRIC tm{};
        dc.GetTextMetrics(&tm);
        dc.SelectObject(old);
        lineH = tm.tmHeight;
    }

    input_.SetWindowPos(nullptr, 0, 0, std::max(0, inputR - inputL), lineH * inputLines_, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    inputLines_ = std::clamp(input_.GetLineCount(), 1, 8);
    int inputH = lineH * inputLines_ + Px(6);  // a little slack: the edit scrolls when its last line is cut at all
    int rowH = std::max(btn, inputH + Px(4));
    int cardH = inner + (hasAttachments ? Px(30) : 0) + rowH + inner;
    composerRect_ = CRect(cx, bottom - cardH, cx + colW, bottom);
    int y = composerRect_.top + inner;

    if (hasAttachments)
    {
        place(IDC_ATTACHMENTS, cx + Px(18), y + Px(4), colW - Px(110), Px(20));
        place(IDC_CLEAR_ATTACHMENTS, cx + colW - Px(84), y, Px(68), Px(24));
        y += Px(30);
    }

    GetDlgItem(IDC_ATTACHMENTS)->ShowWindow(hasAttachments ? SW_SHOW : SW_HIDE);
    GetDlgItem(IDC_CLEAR_ATTACHMENTS)->ShowWindow(hasAttachments ? SW_SHOW : SW_HIDE);
    place(IDC_INPUT, inputL, y + (rowH - inputH) / 2, inputR - inputL, inputH);

    // It scrolled while it was smaller: show its text from the top again (or the caret, past eight lines).
    input_.LineScroll(-input_.GetFirstVisibleLine());
    if (inputLines_ >= 8)
    {
        input_.SendMessage(EM_SCROLLCARET);
    }

    // The buttons sit on the last line: centred while the text is one line, at the bottom when it is more.
    int by = y + rowH - btn;
    place(IDC_PLUS, cx + Px(10), by, btn, btn);
    place(IDC_SEND, inputR + Px(8), by, btn, btn);
    place(IDC_MIC, inputR + Px(8) + btn + gap, by, btn, btn);

    // The transcript fills the rest.
    int tTop = Px(52), tBottom = composerRect_.top - Px(10);

    // It spans the whole centre (its scroll bar at the centre's right edge, like Claude's); margins keep the text in
    // the column.
    place(IDC_TRANSCRIPT, centerL, tTop, centerR - centerL, tBottom - tTop);
    int scrollW = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(m_hWnd));
    int marginL = cx - centerL + Px(8), marginR = std::max<int>(Px(8), centerR - (cx + colW) - scrollW + Px(8));
    transcript_.SendMessage(EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(marginL, marginR));
    int mid = tTop + (tBottom - tTop) / 2;
    place(IDC_HELLO, cx, mid - Px(50), colW, Px(48));
    place(IDC_HELLO_SUB, cx, mid + Px(2), colW, Px(28));

    // Everything, children too: panels that moved or changed size must not leave old pixels behind.
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

BOOL CChatDlg::OnEraseBkgnd(CDC* dc)
{
    CRect rc;
    GetClientRect(&rc);
    dc->FillSolidRect(rc, RGB(255, 255, 255));
    if (!sidebarEdge_.IsRectEmpty())
    {
        dc->FillSolidRect(CRect(sidebarEdge_.left, 0, sidebarEdge_.left + 1, rc.bottom), RGB(229, 229, 232));
    }

    // The composer card: a rounded frame around the message box and its buttons.
    CPen pen(PS_SOLID, std::max(1, Px(1)), RGB(214, 214, 220));
    CPen* op = dc->SelectObject(&pen);
    CBrush* ob = static_cast<CBrush*>(dc->SelectStockObject(WHITE_BRUSH));
    dc->RoundRect(composerRect_, CPoint(Px(18), Px(18)));
    dc->SelectObject(op);
    dc->SelectObject(ob);
    return TRUE;
}

int CChatDlg::SplitterAt(CPoint pt) const
{
    if (sidebarEdge_.PtInRect(pt))
    {
        return 1;
    }

    if (infoEdge_.PtInRect(pt))
    {
        return 2;
    }

    return 0;
}

BOOL CChatDlg::OnSetCursor(CWnd* wnd, UINT hit, UINT msg)
{
    CPoint pt;
    GetCursorPos(&pt);
    ScreenToClient(&pt);

    if (wnd == this && hit == HTCLIENT && (dragging_ || SplitterAt(pt)))
    {
        ::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
        return TRUE;
    }

    return CDialogEx::OnSetCursor(wnd, hit, msg);
}

void CChatDlg::OnLButtonDown(UINT flags, CPoint pt)
{
    dragging_ = SplitterAt(pt);
    if (dragging_)
    {
        SetCapture();
    }

    CDialogEx::OnLButtonDown(flags, pt);
}

void CChatDlg::OnMouseMove(UINT flags, CPoint pt)
{
    if (dragging_)
    {
        CRect rc;
        GetClientRect(&rc);
        int dpi = static_cast<int>(GetDpiForWindow(m_hWnd));

        if (dragging_ == 1)
        {
            settings_.sidebarWidth = std::clamp(MulDiv(pt.x, 96, dpi), 200, 480);
        }
        else
        {
            settings_.infoWidth = std::clamp(MulDiv(rc.right - pt.x, 96, dpi), 220, 520);
        }

        Layout();
        UpdateWindow();
    }

    CDialogEx::OnMouseMove(flags, pt);
}

void CChatDlg::OnLButtonUp(UINT flags, CPoint pt)
{
    if (dragging_)
    {
        dragging_ = 0;
        ReleaseCapture();
        settings_.Save();
    }

    CDialogEx::OnLButtonUp(flags, pt);
}

HBRUSH CChatDlg::OnCtlColor(CDC* dc, CWnd* wnd, UINT ctlColor)
{
    HBRUSH br = CDialogEx::OnCtlColor(dc, wnd, ctlColor);
    if (ctlColor == CTLCOLOR_STATIC)
    {
        int id = wnd->GetDlgCtrlID();
        bool side = id == IDC_TITLE || id == IDC_RECENT_LABEL;
        dc->SetBkColor(side ? kSidebar : RGB(255, 255, 255));
        dc->SetTextColor(id == IDC_RECENT_LABEL || id == IDC_HELLO_SUB || id == IDC_STATUS || id == IDC_WORKSPACE ||
                                 id == IDC_ATTACHMENTS
                             ? kMuted
                             : kText);
        return side ? static_cast<HBRUSH>(sidebarBrush_) : static_cast<HBRUSH>(whiteBrush_);
    }

    return br;
}

// ---------------------------------------------------------------- conversations

std::shared_ptr<Conversation> CChatDlg::NewConversation()
{
    auto c = std::make_shared<Conversation>();
    c->id = NewId();
    c->title = "New chat";
    c->workspace = current_ ? current_->workspace : settings_.defaultWorkspace;

    if (c->workspace.empty())
    {
        c->workspace = settings_.defaultWorkspace;
    }

    c->systemMessage = settings_.defaultSystemMessage;
    c->created = c->updated = NowEpoch();
    return c;
}

std::wstring CChatDlg::WorkspaceOf(const Conversation& c) const
{
    return Wide(c.workspace.empty() ? settings_.defaultWorkspace : c.workspace);
}

void CChatDlg::OpenConversation(const std::shared_ptr<Conversation>& c)
{
    current_ = c;
    attachments_.clear();
    UpdateAttachments();
    RenderConversation();
    UpdateWorkspaceLabel();
    CString title = L"ModelHub Chat";
    if (!c->messages.empty())
    {
        title = W(c->title) + L" - ModelHub Chat";
    }

    SetWindowText(title);
}

void CChatDlg::SaveCurrent()
{
    if (current_ && !current_->messages.empty())
    {
        current_->Save();
    }
}

void CChatDlg::UpdateWorkspaceLabel()
{
    UpdateInfoPanel();
}

void CChatDlg::UpdateInfoPanel()
{
    if (!current_ || !info_.m_hWnd)
    {
        return;
    }

    CInfoPanel::Data d;
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;

    if (GetComputerNameW(name, &n))
    {
        d.computer = name;
    }

    std::wstring folder = WorkspaceOf(*current_);
    d.folderPath = folder;
    d.folderName = FileName(folder).empty() ? folder : FileName(folder);
    d.branch = GitBranch(folder);

    // The model: the chosen one, or (Auto) the one that wrote the last answer.
    std::string model = settings_.model;
    if (model == "auto")
    {
        model.clear();
        for (auto it = current_->messages.rbegin(); it != current_->messages.rend() && model.empty(); ++it)
        {
            model = it->value("x_model", "");
        }

        d.model = model.empty() ? L"Auto" : L"Auto → " + Wide(DisplayModel(settings_, model));
    }
    else
    {
        d.model = Wide(DisplayModel(settings_, model));
    }

    // Who runs it: a provider (the model shows its name already), else ModelHub or the custom server.
    std::string servicePid, serviceModel;
    if (SplitQualified(model, servicePid, serviceModel))
    {
        d.service.clear();
    }
    else
    {
        d.service = settings_.backend == "custom" ? L"custom server" : L"ModelHub";
    }

    // Context: about 3.5 characters a token, against the model's window.
    size_t systemChars = current_->systemMessage.size() + 600, chatChars = 0, attachChars = 0;
    for (const auto& m : current_->messages)
    {
        std::string text = m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>()
                           : m.contains("content")                           ? m["content"].dump()
                                                                             : std::string();
        size_t typed = m.contains("x_text") ? m.value("x_text", "").size() : text.size();
        chatChars += typed;
        attachChars += text.size() > typed ? text.size() - typed : 0;
    }

    size_t toolTokens = BuiltinToolNames().size() * 150;
    size_t tokens = (systemChars + chatChars + attachChars) * 2 / 7 + toolTokens;
    std::string lower = Lower(model);
    size_t window = lower.find("claude") != std::string::npos ? 1000000
                    : lower.find('|') != std::string::npos    ? 128000
                                                              : 32768;  // ModelHub's models: their trained context (the server may use less)
    d.contextPercent = static_cast<int>(std::min<size_t>(100, tokens * 100 / window));
    auto k = [](size_t v)
    {
        char buf[32];
        if (v >= 1000000)
        {
            std::snprintf(buf, sizeof buf, "%.1fM", v / 1e6);
        }
        else if (v >= 1000)
        {
            std::snprintf(buf, sizeof buf, "%.1fk", v / 1e3);
        }
        else
        {
            std::snprintf(buf, sizeof buf, "%zu", v);
        }

        return Wide(buf);
    };

    d.contextSummary = L"about " + k(tokens) + L" of " + k(window) + L" tokens";
    d.contextDetails = { { L"System message and tools", k(systemChars * 2 / 7 + toolTokens) },
        { L"Conversation (" + std::to_wstring(current_->messages.size()) + L" messages)", k(chatChars * 2 / 7) },
        { L"Attached files and images", k(attachChars * 2 / 7) } };

    // Changes: files written or edited by tools (the app's loop, and Copilot's).
    std::vector<std::wstring> seen;
    auto addChange = [&](const std::string& tool, const json& args)
    {
        if (tool != "write_file" && tool != "edit_file")
        {
            return;
        }

        std::wstring path = Wide(args.value("path", ""));
        if (path.empty())
        {
            return;
        }

        if (std::filesystem::path(path).is_relative())
        {
            path = (std::filesystem::path(folder) / path).lexically_normal().wstring();
        }

        if (std::find(seen.begin(), seen.end(), path) != seen.end())
        {
            return;
        }

        seen.push_back(path);
        d.changes.push_back({ std::wstring(tool == "write_file" ? L"Wrote " : L"Edited ") + FileName(path), path });
    };

    for (const auto& m : current_->messages)
    {
        for (const auto& tc : m.value("tool_calls", json::array()))
        {
            json args = json::parse(tc["function"].value("arguments", "{}"), nullptr, false);
            if (args.is_object())
            {
                addChange(tc["function"].value("name", ""), args);
            }
        }

        for (const auto& note : m.value("x_tool_notes", json::array()))
        {
            json args = json::parse(note.value("x_arguments", "{}"), nullptr, false);
            if (args.is_object())
            {
                addChange(note.value("x_name", ""), args);
            }
        }
    }

    info_.SetData(std::move(d));
    titleStrip_.SetState(!current_->messages.empty() ? Wide(current_->title) : L"New chat", settings_.sidebarVisible,
        settings_.infoVisible);
}

void CChatDlg::FillTree()
{
    std::string filter = Lower(TrimCopy(Utf8(sidebar_.Filter())));
    treeItems_.clear();

    // Groups by workspace folder (project), in order of their newest conversation.
    std::vector<std::string> order;
    std::map<std::string, std::vector<std::shared_ptr<Conversation>>> groups;
    for (const auto& c : conversations_)
    {
        if (!filter.empty())
        {
            bool hit = Lower(c->title).find(filter) != std::string::npos;
            for (size_t i = 0; !hit && i < c->messages.size(); ++i)
            {
                const json& m = c->messages[i];
                if (m.contains("content") && m["content"].is_string())
                {
                    hit = Lower(m["content"].get<std::string>()).find(filter) != std::string::npos;
                }
            }

            if (!hit)
            {
                continue;
            }
        }

        std::string key = Lower(c->workspace.empty() ? settings_.defaultWorkspace : c->workspace);
        if (!groups.count(key))
        {
            order.push_back(key);
        }

        groups[key].push_back(c);
    }

    std::vector<CSidebarPanel::Group> out;
    for (const auto& key : order)
    {
        const auto& list = groups[key];
        std::wstring path = WorkspaceOf(*list.front());
        CSidebarPanel::Group g;
        g.key = Wide(key);
        g.label = FileName(path).empty() ? path : FileName(path);

        for (const auto& c : list)
        {
            treeItems_.push_back(c);
            g.items.push_back({ Wide(c->title), treeItems_.size() - 1, c == current_ });
        }

        out.push_back(std::move(g));
    }

    sidebar_.SetGroups(std::move(out));
}

void CChatDlg::SelectConversation(size_t index)
{
    if (index >= treeItems_.size())
    {
        return;
    }

    auto c = treeItems_[index];
    if (c == current_)
    {
        return;
    }

    if (busy_)
    {
        SetStatus("Stop the running answer before switching conversations.");
        return;
    }

    SaveCurrent();
    OpenConversation(c);
    FillTree();
}

void CChatDlg::OnTreeSelChanged(NMHDR*, LRESULT* result)
{
    *result = 0;
    return;  // the hidden tree control; the sidebar panel selects conversations

#if 0
    if (fillingTree_)
        return;
    HTREEITEM item = tree_.GetSelectedItem();
    if (!item)
    {
        return;
    }

    DWORD_PTR data = tree_.GetItemData(item);
    if (data == kGroupItem || data >= treeItems_.size())
    {
        return;
    }
    auto c = treeItems_[data];
    if (c == current_)
    {    
        return;
    }
    
    if (busy_)
    {
        SetStatus("Stop the running answer before switching conversations.");
        FillTree();
        return;
    }

    SaveCurrent();
    OpenConversation(c);
#endif
}

void CChatDlg::OnTreeRClick(NMHDR*, LRESULT* result)
{
    *result = 1;
    CPoint screen;
    GetCursorPos(&screen);
    CPoint pt = screen;
    tree_.ScreenToClient(&pt);
    UINT flags = 0;
    HTREEITEM item = tree_.HitTest(pt, &flags);
    if (!item)
    {
        return;
    }

    DWORD_PTR data = tree_.GetItemData(item);
    if (data == kGroupItem || data >= treeItems_.size())
    {
        return;
    }

    ShowConversationMenu(treeItems_[data], screen);
}

void CChatDlg::ShowConversationMenu(const std::shared_ptr<Conversation>& c, CPoint screen)
{
    CMenu menu;
    menu.CreatePopupMenu();
    menu.AppendMenu(MF_STRING, ID_CONV_RENAME, L"Rename...");
    menu.AppendMenu(MF_STRING, ID_CONV_EXPORT, L"Export as Markdown...");
    menu.AppendMenu(MF_STRING, ID_CONV_OPEN_FOLDER, L"Open workspace folder");
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING | (busy_ && c == current_ ? MF_GRAYED : 0), ID_CONV_DELETE, L"Delete...");
    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, this);
    if (cmd == ID_CONV_RENAME)
    {
        CRenameDlg dlg(this, c->title);
        if (dlg.DoModal() == IDOK)
        {
            c->title = dlg.text;
            c->Save();
            FillTree();
            if (c == current_)
            {
                SetWindowText(W(c->title) + L" - ModelHub Chat");
            }
        }
    }
    else if (cmd == ID_CONV_EXPORT)
    {
        ExportConversation(*c);
    }
    else if (cmd == ID_CONV_OPEN_FOLDER)
    {
        ShellExecuteW(m_hWnd, L"open", WorkspaceOf(*c).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (cmd == ID_CONV_DELETE)
    {
        if (AfxMessageBox(L"Delete \"" + W(c->title) + L"\"? This cannot be undone.", MB_YESNO | MB_ICONWARNING) != IDYES)
        {
            return;
        }

        c->Delete();
        conversations_.erase(std::remove(conversations_.begin(), conversations_.end(), c), conversations_.end());
        if (c == current_)
        {
            OpenConversation(conversations_.empty() ? NewConversation() : conversations_.front());
        }

        FillTree();
    }
}

void CChatDlg::ExportConversation(const Conversation& c)
{
    CString name = W(c.title);
    for (const wchar_t* bad : { L"\\", L"/", L":", L"*", L"?", L"\"", L"<", L">", L"|" })
    {
        name.Replace(bad, L"_");
    }
    CFileDialog dlg(FALSE, L"md", name + L".md", OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
        L"Markdown (*.md)|*.md|All files (*.*)|*.*||", this);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    std::string md = "# " + c.title + "\n\n";
    if (!c.systemMessage.empty())
    {
        md += "**System:** " + c.systemMessage + "\n\n";
    }

    for (const auto& m : c.messages)
    {
        std::string role = m.value("role", "");
        std::string content = m.contains("x_text")                                ? m.value("x_text", "")
                              : m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>()
                                                                                  : std::string();
        if (role == "user")
        {
            md += "## You\n\n" + content + "\n\n";
        }
        else if (role == "assistant")
        {
            md += "## Assistant\n\n";
            if (!content.empty())
            {
                md += content + "\n\n";
            }

            if (m.contains("tool_calls"))
            {
                for (const auto& tc : m["tool_calls"])
                {
                    md += "> tool: `" + tc["function"].value("name", "") + "` " + tc["function"].value("arguments", "") + "\n\n";
                }
            }
        }
        else if (role == "tool")
        {
            md += "```text\n" + Truncate(content, 4000) + "\n```\n\n";
        }
    }

    if (!WriteFileAtomic(std::wstring(dlg.GetPathName().GetString()), md))
    {
        AfxMessageBox(L"Could not write the file.", MB_ICONERROR);
    }
}

void CChatDlg::OnNewChat()
{
    if (busy_)
    {
        SetStatus("Stop the running answer before starting a new chat.");
        return;
    }

    SaveCurrent();
    if (current_ && current_->messages.empty())
    {
        input_.SetFocus();
        return;  // already a new chat
    }

    OpenConversation(NewConversation());
    FillTree();
    input_.SetWindowText(L"");
    input_.SetFocus();
}

void CChatDlg::OnSearchChange()
{
    FillTree();
}

void CChatDlg::OnWorkspaceChange()
{
    if (!current_)
    {
        return;
    }

    CFolderPickerDialog dlg(WorkspaceOf(*current_).c_str(), 0, this);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    current_->workspace = U8(dlg.GetPathName());
    if (current_->messages.empty())
    {
        settings_.defaultWorkspace = current_->workspace;  // new chats start there too
    }

    SaveCurrent();
    settings_.Save();
    UpdateWorkspaceLabel();
    FillTree();
}

// ---------------------------------------------------------------- transcript

void CChatDlg::Append(const std::wstring& text, const Style& s)
{
    if (text.empty())
    {
        return;
    }

    transcript_.SetSel(-1, -1);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_COLOR | CFM_FACE | CFM_SIZE | CFM_BACKCOLOR | CFM_LINK | CFM_UNDERLINE;
    cf.dwEffects = (s.bold ? CFE_BOLD : 0) | (s.italic ? CFE_ITALIC : 0) | (s.back == CLR_NONE ? CFE_AUTOBACKCOLOR : 0) |
                   (s.link ? CFE_UNDERLINE : 0);
    cf.crTextColor = s.color;
    cf.crBackColor = s.back == CLR_NONE ? RGB(255, 255, 255) : s.back;
    int pt = s.size ? s.size : (s.code ? 10 : 11);
    cf.yHeight = pt * 20;
    wcscpy_s(cf.szFaceName, s.code ? L"Consolas" : L"Segoe UI");
    transcript_.SetSelectionCharFormat(cf);
    transcript_.ReplaceSel(text.c_str());
}

long CChatDlg::EndPos()
{
    transcript_.SetSel(-1, -1);
    long a = 0, b = 0;
    transcript_.GetSel(a, b);
    return b;
}

size_t CChatDlg::AddLink(const std::wstring& label, LinkKind kind, const std::wstring& payload, Style style)
{
    style.link = true;
    LinkTarget t;
    t.kind = kind;
    t.payload = payload;
    t.cpMin = EndPos();
    Append(label, style);
    t.cpMax = EndPos();
    links_.push_back(std::move(t));
    return links_.size() - 1;
}

void CChatDlg::CopyToClipboard(const std::wstring& text, const char* what)
{
    std::wstring crlf;
    for (wchar_t c : text)
    {
        if (c == L'\n')
        {
            crlf += L'\r';
        }

        if (c != L'\r')
        {
            crlf += c;
        }
    }

    if (!OpenClipboard())
    {
        SetStatus("The clipboard is busy; try again.");
        return;
    }

    EmptyClipboard();
    size_t bytes = (crlf.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
    {
        memcpy(GlobalLock(mem), crlf.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
        {
            GlobalFree(mem);
        }
    }

    CloseClipboard();
    size_t lines = std::count(text.begin(), text.end(), L'\n') + (text.empty() || text.back() == L'\n' ? 0 : 1);
    SetStatus(std::string("Copied ") + what + " (" + std::to_string(lines) + (lines == 1 ? " line)" : " lines)"));
}

void CChatDlg::AppendInline(const std::wstring& line, const Style& base)
{
    std::wstring buf;
    auto flush = [&]
    {
        Append(buf, base);
        buf.clear();
    };

    for (size_t i = 0; i < line.size();)
    {
        if (line.compare(i, 7, L"http://") == 0 || line.compare(i, 8, L"https://") == 0)
        {
            size_t end = line.find_first_of(L" \t<>\"')`", i);
            if (end == std::wstring::npos)
            {
                end = line.size();
            }

            while (end > i && std::wstring(L".,;:!?").find(line[end - 1]) != std::wstring::npos)
            {
                --end;
            }

            flush();
            Style u = base;
            u.color = kAccent;
            std::wstring url = line.substr(i, end - i);
            AddLink(url, LinkKind::Url, url, u);
            i = end;
            continue;
        }

        if (line.compare(i, 2, L"**") == 0)
        {
            size_t end = line.find(L"**", i + 2);
            if (end != std::wstring::npos && end > i + 2)
            {
                flush();
                Style b = base;
                b.bold = true;
                Append(line.substr(i + 2, end - i - 2), b);
                i = end + 2;
                continue;
            }
        }

        if (line[i] == L'`')
        {
            size_t end = line.find(L'`', i + 1);
            if (end != std::wstring::npos && end > i + 1)
            {
                flush();
                Style c = base;
                c.code = true;
                c.back = kCodeBack;
                c.color = RGB(180, 35, 80);
                Append(line.substr(i + 1, end - i - 1), c);
                i = end + 1;
                continue;
            }
        }

        buf += line[i++];
    }

    flush();
}

// A small Markdown subset: code fences, headings, bullets, **bold**, `code`.
void CChatDlg::AppendMarkdown(const std::string& text)
{
    std::wstring all = Crlf(text);
    bool inCode = false;
    size_t codeLink = static_cast<size_t>(-1);
    size_t pos = 0;
    Style body;
    while (pos <= all.size())
    {
        size_t eol = all.find(L'\n', pos);
        if (eol == std::wstring::npos)
        {
            eol = all.size();
        }

        std::wstring line = all.substr(pos, eol - pos);
        bool last = eol >= all.size();
        std::wstring trimmed = line.substr(std::min(line.find_first_not_of(L" \t"), line.size()));
        if (trimmed.rfind(L"```", 0) == 0)
        {
            inCode = !inCode;
            if (inCode)
            {
                // A "Copy code" button above every code block.
                Style lang;
                lang.color = kMuted;
                lang.size = 9;
                if (trimmed.size() > 3)
                {
                    Append(trimmed.substr(3) + L"   ", lang);
                }

                Style button = lang;
                button.color = kAccent;
                button.bold = true;
                codeLink = AddLink(L"\u29C9 Copy code", LinkKind::Copy, L"", button);
                Append(L"\n", lang);
            }
            else if (codeLink < links_.size() && !links_[codeLink].payload.empty() &&
                     links_[codeLink].payload.back() == L'\n')
            {
                links_[codeLink].payload.pop_back();
            }
        }
        else if (inCode)
        {
            Style code;
            code.code = true;
            code.back = kCodeBack;
            Append(line + L"\n", code);
            if (codeLink < links_.size())
            {
                links_[codeLink].payload += line + L"\n";
            }
        }
        else if (!trimmed.empty() && trimmed[0] == L'#')
        {
            size_t level = trimmed.find_first_not_of(L'#');
            Style h;
            h.bold = true;
            h.size = level <= 1 ? 15 : level == 2 ? 13
                                                  : 12;
            Append(trimmed.substr(std::min(trimmed.find_first_not_of(L"# "), trimmed.size())) + L"\n", h);
        }
        else if (trimmed.rfind(L"- ", 0) == 0 || trimmed.rfind(L"* ", 0) == 0)
        {
            Append(std::wstring(line.size() - trimmed.size(), L' ') + L"  • ", body);
            AppendInline(trimmed.substr(2), body);
            Append(L"\n", body);
        }
        else
        {
            AppendInline(line, body);
            if (!last)
            {
                Append(L"\n", body);
            }
        }

        if (last)
        {
            break;
        }

        pos = eol + 1;
    }
}

void CChatDlg::AppendMessage(const json& m)
{
    std::string role = m.value("role", "");
    Style header;
    header.bold = true;
    if (role == "user")
    {
        header.color = kAccent;
        Append(L"You\n", header);
        std::string text = m.contains("x_text")                                ? m.value("x_text", "")
                           : m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>()
                                                                               : std::string();
        if (text.empty() && m.contains("content") && m["content"].is_array())
        {
            for (const auto& part : m["content"])
            {
                if (part.value("type", "") == "text")
                {
                    text += part.value("text", "");
                }
            }
        }

        AppendMarkdown(text);
        Append(L"\n", Style{});
        if (m.contains("x_attachments") && m["x_attachments"].is_array() && !m["x_attachments"].empty())
        {
            std::string names;
            for (const auto& a : m["x_attachments"])
            {
                names += (names.empty() ? "" : ", ") + a.get<std::string>();
            }

            Style muted;
            muted.color = kMuted;
            muted.size = 9;
            Append(L"Attached: " + Wide(names) + L"\n", muted);
        }

        Append(L"\n", Style{});
    }
    else if (role == "assistant")
    {
        AppendRoute(m);
        Append(L"Assistant", header);
        if (m.contains("x_model"))
        {
            Style model;
            model.color = kMuted;
            model.size = 9;
            Append(L"  \u00B7  " + Wide(DisplayModel(settings_, m.value("x_model", ""))), model);
        }

        Append(L"\n", header);
        for (const auto& note : m.value("x_tool_notes", json::array()))
        {
            AppendToolNote(note);  // tools Copilot called on the way to this answer
        }

        std::string reasoning = m.value("reasoning_content", "");
        if (settings_.showReasoning && !TrimCopy(reasoning).empty())
        {
            Style r;
            r.color = kMuted;
            r.italic = true;
            r.size = 10;
            Style label = r;
            label.bold = true;
            Append(L"Thinking\n", label);
            Append(Crlf(TrimCopy(reasoning)) + L"\n\n", r);
        }

        std::string content = m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>() : "";
        if (!TrimCopy(content).empty())
        {
            AppendMarkdown(TrimCopy(content));
            Append(L"\n", Style{});
            Style button;
            button.color = kAccent;
            button.size = 9;
            AddLink(L"\u29C9 Copy answer", LinkKind::Copy, Crlf(TrimCopy(content)), button);
            links_.back().kind = LinkKind::Copy;
            Append(L"\n", Style{});
        }

        if (m.contains("tool_calls") && m["tool_calls"].is_array())
        {
            for (const auto& tc : m["tool_calls"])
            {
                Style t;
                t.code = true;
                t.color = RGB(80, 80, 90);
                t.back = kCodeBack;
                t.size = 9;
                std::string line = "→ " + tc["function"].value("name", "") + " " +
                                   Truncate(tc["function"].value("arguments", ""), 400, "...");
                Append(Crlf(line) + L"\n", t);
            }
        }

        if (m.value("x_stopped", false))
        {
            Style s;
            s.color = RGB(180, 83, 9);
            s.size = 9;
            Append(L"(stopped)\n", s);
        }

        Append(L"\n", Style{});
    }
    else if (role == "tool")
    {
        Style t;
        t.code = true;
        t.color = RGB(90, 90, 98);
        t.size = 9;
        Style label = t;
        label.bold = true;
        Append(L"↳ " + Wide(m.value("x_name", "tool")) + L" result\n", label);
        std::string content = m.value("content", "");
        Append(Crlf(Truncate(TrimCopy(content), 1500, "\n... (shortened here; the model got all of it)")) + L"\n\n", t);
    }
}

void CChatDlg::AppendRoute(const json& m)
{
    if (!m.contains("x_route") || !m["x_route"].is_object())
    {
        return;
    }

    const json& r = m["x_route"];
    Style muted;
    muted.color = kMuted;
    muted.size = 9;
    Category c = Category::General;
    ParseCategory(r.value("category", ""), c);
    Append(L"Auto \u2192 " + Wide(DisplayModel(settings_, r.value("model", ""))) + L"  \u00B7  " + Wide(CategoryLabel(c)) + L": " +
               Wide(r.value("reason", "")) + L"\n",
        muted);
    if (r.contains("suggest") && r["suggest"].is_object())
    {
        std::string id = r["suggest"].value("id", "");
        bool installed = std::find(models_.begin(), models_.end(), id) != models_.end();
        if (!id.empty() && !installed)
        {
            Style tip = muted;
            tip.color = RGB(146, 64, 14);
            Append(L"Suggested: " + Wide(id) + L" (" + Wide(FormatGB(r["suggest"].value("size", 0ULL))) + L", " +
                       Wide(r["suggest"].value("why", "")) + L")   ",
                tip);

            if (downloading_.count(id))
            {
                Append(L"downloading...", tip);
            }
            else
            {
                Style button = tip;
                button.bold = true;
                button.color = kAccent;
                AddLink(L"\u2B07 Download", LinkKind::Download, Wide(id), button);
            }

            Append(L"\n", muted);
        }
    }
}

void CChatDlg::AppendToolNote(const json& note)
{
    Style t;
    t.code = true;
    t.color = RGB(80, 80, 90);
    t.back = kCodeBack;
    t.size = 9;
    Append(Crlf("→ " + note.value("x_name", "") + " " + Truncate(note.value("x_arguments", ""), 400, "...")) + L"\n", t);
    AppendMessage(note);  // the result, as for any tool
}

void CChatDlg::RenderConversation()
{
    links_.clear();
    transcript_.SetRedraw(FALSE);
    transcript_.SetWindowText(L"");
    if (current_)
    {
        if (!current_->systemMessage.empty())
        {
            Style s;
            s.color = kMuted;
            s.size = 9;
            s.italic = true;
            Append(L"System: " + Crlf(Truncate(current_->systemMessage, 400, "...")) + L"\n\n", s);
        }

        for (const auto& m : current_->messages)
        {
            AppendMessage(m);
        }
    }

    transcript_.SetRedraw(TRUE);
    transcript_.Invalidate();
    ScrollToEnd();
    UpdateEmptyState();
}

void CChatDlg::ScrollToEnd()
{
    transcript_.SendMessage(WM_VSCROLL, SB_BOTTOM, 0);
}

void CChatDlg::UpdateEmptyState()
{
    bool empty = !current_ || (current_->messages.empty() && !busy_);
    transcript_.ShowWindow(empty ? SW_HIDE : SW_SHOW);
    GetDlgItem(IDC_HELLO)->ShowWindow(empty ? SW_SHOW : SW_HIDE);
    GetDlgItem(IDC_HELLO_SUB)->ShowWindow(empty ? SW_SHOW : SW_HIDE);
}

void CChatDlg::OnTranscriptClick(long cp)
{
    for (const auto& t : links_)
    {
        if (cp < t.cpMin || cp >= t.cpMax)
        {
            continue;
        }

        CString label;
        transcript_.GetTextRange(t.cpMin, t.cpMax, label);
        switch (t.kind)
        {
        case LinkKind::Copy:
            CopyToClipboard(t.payload, label.Find(L"answer") >= 0 ? "the answer" : "the code");
            break;
        case LinkKind::Url:
            if (t.payload.rfind(L"http://", 0) == 0 || t.payload.rfind(L"https://", 0) == 0)
            {
                ShellExecuteW(m_hWnd, L"open", t.payload.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case LinkKind::Download:
        {
            std::string id = Utf8(t.payload);
            unsigned long long size = 0;
            if (current_)
            {
                for (const auto& m : current_->messages)
                {
                    if (m.contains("x_route") && m["x_route"].contains("suggest") &&
                        m["x_route"]["suggest"].value("id", "") == id)
                    {
                        size = m["x_route"]["suggest"].value("size", 0ULL);
                    }
                }
            }

            StartDownload(id, size);
            break;
        }
        }
        return;
    }
}

// ---------------------------------------------------------------- composer

void CChatDlg::SetStatus(const std::string& text)
{
    SetDlgItemText(IDC_STATUS, W(text));
    footer_.SetStatus(W(text).GetString());

    // While a model has not answered yet, the footer counts the seconds (a slow or overloaded cloud model can take
    // minutes; this shows it is still waiting, and Escape or the stop button ends it).
    bool waiting = text.rfind("Waiting for", 0) == 0;
    waitStatus_ = waiting ? std::wstring(W(text).GetString()) : std::wstring();
    waitSince_ = GetTickCount64();
    if (waiting)
    {
        SetTimer(7, 1000, nullptr);
    }
    else
    {
        KillTimer(7);
    }
}

void CChatDlg::OnTimer(UINT_PTR id)
{
    if (id != 7)
    {
        CDialogEx::OnTimer(id);
        return;
    }

    if (!busy_ || waitStatus_.empty())
    {
        KillTimer(7);
        return;
    }
    ULONGLONG secs = (GetTickCount64() - waitSince_) / 1000;
    if (secs >= 3)
    {
        footer_.SetStatus(waitStatus_ + L"  " + std::to_wstring(secs) + L" s (Esc stops it)");
    }
}

void CChatDlg::SetBusy(bool busy)
{
    UpdateInfoPanel();
    busy_ = busy;
    SetDlgItemText(IDC_SEND, busy ? L"Stop" : L"Send");
    GetDlgItem(IDC_SEND)->Invalidate(FALSE);
    GetDlgItem(IDC_NEW_CHAT)->EnableWindow(!busy);
    GetDlgItem(IDC_WORKSPACE_CHANGE)->EnableWindow(!busy);
    GetDlgItem(IDC_PLUS)->EnableWindow(!busy);
    model_.EnableWindow(!busy);
    UpdateFooter();
    UpdateEmptyState();
}

void CChatDlg::AddAttachment(const std::wstring& path)
{
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        SetStatus("Not a file: " + Utf8(path));
        return;
    }

    Attachment a;
    a.name = Utf8(path);
    a.mime = ImageMime(path);
    if (!a.mime.empty())
    {
        a.kind = Attachment::Kind::Image;
        if (!ReadFileBytes(path, a.content, 20 * 1024 * 1024 + 1) || a.content.size() > 20 * 1024 * 1024)
        {
            SetStatus("Images up to 20 MB can be attached: " + a.name);
            return;
        }
    }
    else
    {
        if (!ReadFileBytes(path, a.content, 512 * 1024 + 1))
        {
            SetStatus("Cannot read " + a.name);
            return;
        }

        if (a.content.find('\0') != std::string::npos)
        {
            SetStatus("Only text files and images can be attached: " + a.name);
            return;
        }

        if (a.content.size() > 512 * 1024)
        {
            a.content = Truncate(a.content, 512 * 1024, "\n... (file truncated at 512 KB)");
        }
        if (!IsValidUtf8(a.content))
        {
            a.content = ToUtf8FromCodePage(a.content, CP_ACP);
        }
    }

    attachments_.push_back(std::move(a));
    UpdateAttachments();
}

void CChatDlg::UpdateAttachments()
{
    std::wstring text;
    for (const auto& a : attachments_)
    {
        std::wstring n = a.kind == Attachment::Kind::Resource ? Wide(a.name) : FileName(Wide(a.name));
        text += (text.empty() ? L"Attached: " : L", ") + n;
    }

    SetDlgItemText(IDC_ATTACHMENTS, text.c_str());
    if (transcript_.m_hWnd)
    {
        Layout();
    }
}

bool CChatDlg::PasteFromClipboard()
{
    // Longer than this, pasted text goes with the message as an attachment instead of filling the box.
    constexpr size_t kLongPasteChars = 4000, kLongPasteLines = 60;
    ClipboardContent c = ReadClipboard(m_hWnd);
    switch (c.kind)
    {
    case ClipboardContent::Kind::Files:
    {
        size_t before = attachments_.size();
        for (const auto& f : c.files)
        {
            AddAttachment(f);
        }

        size_t added = attachments_.size() - before;
        if (added)
        {
            SetStatus("Attached " + std::to_string(added) + (added == 1 ? " pasted file" : " pasted files"));
        }

        return true;
    }
    case ClipboardContent::Kind::Image:
    {
        if (c.png.size() > 20 * 1024 * 1024)
        {
            SetStatus("The pasted image is larger than 20 MB as PNG: it was not attached.");
            return true;
        }

        Attachment a;
        a.kind = Attachment::Kind::Image;
        a.name = "pasted-image-" + std::to_string(++pasteCount_) + ".png";
        a.mime = "image/png";
        a.content = std::move(c.png);
        attachments_.push_back(std::move(a));
        UpdateAttachments();
        SetStatus("Pasted an image (" + std::to_string(c.width) + " x " + std::to_string(c.height) +
                  "): it goes with your next message (models that read images: see Auto mode's \"Images\").");
        return true;
    }
    case ClipboardContent::Kind::Text:
    {
        size_t lines = static_cast<size_t>(std::count(c.text.begin(), c.text.end(), L'\n')) + 1;
        if (c.text.size() <= kLongPasteChars && lines <= kLongPasteLines)
        {
            return false;  // short text: the edit box pastes it
        }

        std::wstring text;
        text.reserve(c.text.size());
        for (wchar_t ch : c.text)
        {
            if (ch != L'\r')
            {
                text += ch;
            }
        }

        Attachment a;
        a.kind = Attachment::Kind::Text;
        a.name = "pasted-text-" + std::to_string(++pasteCount_) + ".txt";
        a.content = Utf8(text);
        attachments_.push_back(std::move(a));
        UpdateAttachments();
        SetStatus("Long text pasted as an attachment (" + std::to_string(lines) + " lines)");
        return true;
    }
    case ClipboardContent::Kind::None:
        break;
    }
    return false;
}

void CChatDlg::OnClearAttachments()
{
    attachments_.clear();
    UpdateAttachments();
}

void CChatDlg::OnDropFiles(HDROP drop)
{
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i)
    {
        wchar_t path[MAX_PATH * 4];
        if (DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path))))
        {
            AddAttachment(path);
        }
    }

    DragFinish(drop);
}

void CChatDlg::OnModelChange()
{
    int sel = model_.GetCurSel();
    if (sel < 0 || sel >= static_cast<int>(comboIds_.size()))
    {
        return;
    }

    settings_.model = comboIds_[static_cast<size_t>(sel)];
    settings_.Save();
    UpdateInfoPanel();
    UpdateFooter();
}

void CChatDlg::OnRefreshModels()
{
    RefreshModels();
}

void CChatDlg::RefreshModels()
{
    SetStatus(settings_.backend == "custom" ? "Asking " + settings_.customUrl + " for its models..."
                                            : "Connecting to ModelHub...");
    int request = ++modelsRequest_;
    footer_.SetHealth(CFooterBar::Health::Unknown);
    Settings s = settings_;
    HWND hwnd = m_hWnd;
    auto work = [s, hwnd, request]()
    {
        auto* r = new ModelsResult;
        r->request = request;
        Endpoint ep;
        if (ResolveEndpoint(s, ep, r->error, [hwnd](const std::string& m)
                { PostStatus(hwnd, m); }, &r->startedHub))
        {
            r->label = ep.label;
            r->ok = ListModels(ep, r->ids, r->error);
        }

        for (const auto& p : s.providers)
        {
            if (!p.enabled)
            {
                continue;
            }

            std::vector<std::string> list;
            std::string err;
            if (ListProviderModels(p, list, err))
            {
                r->providerModels.push_back({ p.id, list });
            }
            else
            {
                r->providerErrors.push_back(p.name + ": " + err);
                if (p.Cloud() && !p.models.empty())
                {
                    r->providerModels.push_back({ p.id, p.models });  // the last list it gave
                }
            }
        }

        if (!::PostMessage(hwnd, WM_APP_MODELS, 0, reinterpret_cast<LPARAM>(r)))
        {
            delete r;
        }
    };

    std::thread(work).detach();
}

LRESULT CChatDlg::OnModels(WPARAM, LPARAM lp)
{
    std::unique_ptr<ModelsResult> r(reinterpret_cast<ModelsResult*>(lp));
    if (r->request != modelsRequest_)
    {
        return 0;
    }

    models_ = r->ok ? r->ids : std::vector<std::string>();
    std::sort(models_.begin(), models_.end());
    size_t fromProviders = 0;
    for (const auto& [id, list] : r->providerModels)
    {
        for (auto& p : settings_.providers)
        {
            if (p.id == id)
            {
                p.models = list;  // kept for when the provider cannot be reached
            }
        }

        for (const auto& m : list)
        {
            models_.push_back(QualifiedModel(id, m));
        }

        fromProviders += list.size();
    }

    settings_.Save();

    comboIds_ = { "auto" };
    comboIds_.insert(comboIds_.end(), models_.begin(), models_.end());

    // The chosen provider model stays chosen while its provider cannot be reached (it is not silently replaced).
    std::string pid, pmodel;
    bool keepUnreachable = std::find(comboIds_.begin(), comboIds_.end(), settings_.model) == comboIds_.end() &&
                           SplitQualified(settings_.model, pid, pmodel) && settings_.FindProvider(pid) &&
                           settings_.FindProvider(pid)->enabled;
    if (keepUnreachable)
    {
        comboIds_.push_back(settings_.model);
    }

    model_.ResetContent();
    for (size_t i = 0; i < comboIds_.size(); ++i)
    {
        model_.AddString(i == 0 ? CString(L"Auto (picks a model for each message)")
                                : W(DisplayModel(settings_, comboIds_[i]) +
                                      (keepUnreachable && i + 1 == comboIds_.size() ? "  - not reachable" : "")));
    }

    auto it = std::find(comboIds_.begin(), comboIds_.end(), settings_.model);
    model_.SetCurSel(it == comboIds_.end() ? 0 : static_cast<int>(it - comboIds_.begin()));
    OnModelChange();

    footer_.SetHealth(r->ok ? CFooterBar::Health::Ok : CFooterBar::Health::Failed);
    std::string status;
    if (r->ok)
    {
        status = r->ids.empty() ? (settings_.backend == "custom" ? r->label + ": no models listed"
                                                                 : "ModelHub: no models installed yet (ModelHub Manager downloads them)")
                                : r->label + " - " + std::to_string(r->ids.size()) + (r->ids.size() == 1 ? " model" : " models");
    }
    else
    {
        status = (settings_.backend == "custom" ? "Cannot reach the server: " : "ModelHub: ") + r->error;
    }

    if (r->startedHub)
    {
        status = "Started ModelHub · " + status;
    }

    if (!importNote_.empty())
    {
        status = importNote_ + " · " + status;
        importNote_.clear();
    }

    if (fromProviders)
    {
        status += " \u00B7 " + std::to_string(fromProviders) + " from providers";
    }

    if (!r->providerErrors.empty())
    {
        status += " \u00B7 " + r->providerErrors.front() +
                  (r->providerErrors.size() > 1 ? " (+" + std::to_string(r->providerErrors.size() - 1) + " more)" : "");
    }

    SetStatus(status);
    return 0;
}

void CChatDlg::OnProviders()
{
    CProvidersDlg dlg(this, settings_.providers);
    dlg.autoImport = settings_.autoImportProviders;
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    settings_.providers = dlg.providers;
    settings_.autoImportProviders = dlg.autoImport;
    for (const auto& p : settings_.providers)
    {
        settings_.importedProviders.insert(ProviderSignature(p));  // so that startup does not add them again
    }

    settings_.Save();
    RefreshModels();
}

LRESULT CChatDlg::OnImportProviders(WPARAM, LPARAM lp)
{
    std::unique_ptr<Detection> d(reinterpret_cast<Detection*>(lp));
    std::vector<std::string> added = ImportDetected(settings_, *d);
    settings_.Save();
    if (!added.empty())
    {
        std::string names;
        for (const auto& n : added)
        {
            names += (names.empty() ? "" : ", ") + n;
        }

        importNote_ = "Imported from installed apps: " + names;
        SetStatus(importNote_ + " - loading their models...");
        RefreshModels();
    }

    // Then what needs a yes first: Ollama's models into ModelHub, and Claude's sign-in.
    if (!d->ollama.models.empty() && !settings_.ollamaMoveDeclined && !busy_)
    {
        OfferOllamaMove(d->ollama, false);
    }

    if (d->claudeApp && !settings_.claudeSetupDeclined && !busy_)
    {
        OfferClaudeSetup(false);
    }

    return 0;
}

namespace
{

// A task dialog with command-link buttons: returns the id pressed, 0 when closed.
int AskWithButtons(HWND owner, const std::wstring& title, const std::wstring& main, const std::wstring& content,
    const std::vector<std::pair<int, std::wstring>>& buttons, PCWSTR icon = TD_INFORMATION_ICON)
{
    std::vector<TASKDIALOG_BUTTON> b;
    for (const auto& [id, text] : buttons)
    {
        b.push_back({ id, text.c_str() });
    }

    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = title.c_str();
    c.pszMainIcon = icon;
    c.pszMainInstruction = main.c_str();
    c.pszContent = content.c_str();
    c.pButtons = b.data();
    c.cButtons = static_cast<UINT>(b.size());
    int pressed = 0;
    return SUCCEEDED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr)) ? pressed : 0;
}

}  // namespace

void CChatDlg::OfferOllamaMove(const OllamaInventory& inv, bool fromMenu)
{
    if (inv.models.empty())
    {
        AfxMessageBox(L"No Ollama models were found on this PC (" + CString(inv.modelsDir.c_str()) + L").", MB_ICONINFORMATION);
        return;
    }

    std::string list;
    for (size_t i = 0; i < inv.models.size() && i < 12; ++i)
    {
        list += "\n  • " + inv.models[i].name + "  (" + std::to_string(inv.models[i].size >> 20) + " MB)" +
                (inv.models[i].projector ? "  - text only after the move" : "");
    }

    if (inv.models.size() > 12)
    {
        list += "\n  ... and " + std::to_string(inv.models.size() - 12) + " more";
    }

    std::string content = "Ollama keeps " + std::to_string(inv.models.size()) + " model" +
                          (inv.models.size() == 1 ? "" : "s") + " (" + std::to_string(inv.totalBytes >> 20) +
                          " MB) in " + Utf8(inv.modelsDir) + ":" + list +
                          "\n\nModelHub can take them over: every application that uses ModelHub can then use them, "
                          "without a second copy.";

    if (inv.ollamaInstalled)
    {
        content += "\n\nOllama is installed: after the move it no longer has these models.";
    }

    if (inv.ollamaRunning)
    {
        content += "\nOllama is running: quit it first, or files it has open cannot be moved.";
    }

    int id = AskWithButtons(m_hWnd, L"ModelHub Chat", L"Move Ollama's models to ModelHub?", Wide(content),
        { { 101, L"Move them to ModelHub\nThe files are moved, not copied (instant on the same disk). ModelHub checks "
                 L"each one, then Ollama's models folder is deleted." },
            { 102, L"Not now" }, { 103, fromMenu ? L"Cancel" : L"Don't ask again" } });

    if (id == 103 && !fromMenu)
    {
        settings_.ollamaMoveDeclined = true;
        settings_.Save();
        SetStatus("ModelHub Chat will not ask again (the \"+\" menu still has \"Move Ollama models to ModelHub\").");
    }

    if (id != 101)
    {
        return;
    }

    SetStatus("Moving Ollama's models to ModelHub...");
    Settings s = settings_;
    HWND hwnd = m_hWnd;
    auto move = [s, hwnd, inv]()
    {
        auto* r = new MigrationResult;
        Endpoint ep;
        std::string error;
        if (!ResolveEndpoint(s, ep, error, [hwnd](const std::string& m)
                { PostStatus(hwnd, m); }) ||
            !ep.modelhub)
        {
            r->error = error.empty() ? "moving models needs the ModelHub service (Settings: ModelHub)" : error;
        }
        else
        {
            *r = MigrateOllamaModels(ep, inv, [hwnd](const std::string& m)
                { PostStatus(hwnd, m); });
        }

        if (!::PostMessage(hwnd, WM_APP_MIGRATED, 0, reinterpret_cast<LPARAM>(r)))
        {
            delete r;
        }
    };

    std::thread(move).detach();
}

LRESULT CChatDlg::OnMigrated(WPARAM, LPARAM lp)
{
    std::unique_ptr<MigrationResult> r(reinterpret_cast<MigrationResult*>(lp));
    std::string msg;
    if (r->moved || r->duplicates)
    {
        msg = "Moved " + std::to_string(r->moved) + " model" + (r->moved == 1 ? "" : "s") + " into ModelHub" +
              (r->duplicates ? " (" + std::to_string(r->duplicates) + " it had already)" : "") +
              ". ModelHub checks each file (SHA-256) and then lists it; press ↻ in a minute.";
    }

    for (const auto& f : r->failed)
    {
        msg += "\n\nNot moved: " + f;
    }

    if (!r->error.empty())
    {
        msg += "\n\n" + r->error;
    }

    if (r->folderDeleted)
    {
        msg += "\n\nOllama's models folder was deleted.";
    }
    else if (!r->failed.empty())
    {
        msg += "\n\nOllama's models folder was kept, because not every model moved.";
    }

    SetStatus(r->failed.empty() && r->error.empty() ? "Ollama's models moved to ModelHub" : "Moving Ollama's models: see the message");
    AfxMessageBox(W(TrimCopy(msg)), r->failed.empty() && r->error.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
    RefreshModels();
    return 0;
}

void CChatDlg::OfferClaudeSetup(bool fromMenu)
{
    bool ready = std::any_of(settings_.providers.begin(), settings_.providers.end(),
        [](const ProviderConfig& p)
        { return p.kind == "anthropic" && p.enabled && !p.Key().empty(); });

    if (ready && !fromMenu)
    {
        return;  // Claude models are set up already
    }

    if (!fromMenu)
    {
        int id = AskWithButtons(m_hWnd, L"ModelHub Chat", L"Claude is installed on this PC",
            L"Use Claude models in ModelHub Chat too? The Claude app's Pro or Max subscription works only in "
            L"Anthropic's apps, so ModelHub Chat uses the Claude API with your Anthropic Console account (billed there). "
            L"It installs Anthropic's command-line tool (ant) and opens the sign-in.",
            { { 201, L"Set up Claude models\nInstall Anthropic's CLI and sign in" }, { 202, L"Not now" },
                { 203, L"Don't ask again" } });
        if (id == 203)
        {
            settings_.claudeSetupDeclined = true;
            settings_.Save();
            SetStatus("ModelHub Chat will not ask again (the \"+\" menu still has \"Set up Claude\").");
        }

        if (id != 201)
        {
            return;
        }
    }

    CClaudeDlg dlg(this);
    dlg.DoModal();
    if (dlg.signedIn)
    {
        EnsureClaudeProvider();
    }
}

void CChatDlg::EnsureClaudeProvider()
{
    for (auto& p : settings_.providers)
    {
        if (p.kind == "anthropic" && p.apiKeyEnv == "ant")
        {
            p.enabled = true;
            settings_.Save();
            RefreshModels();
            return;
        }
    }

    for (const auto& preset : ProviderPresets())
    {
        if (std::string(preset.id) == "claude")
        {
            ProviderConfig c;
            c.id = UniqueProviderId(settings_.providers, preset.id);
            c.name = preset.name;
            c.kind = preset.kind;
            c.baseUrl = preset.baseUrl;
            c.apiKeyEnv = "ant";
            settings_.providers.push_back(c);
            settings_.importedProviders.insert(ProviderSignature(c));
            settings_.Save();
            importNote_ = "Claude is set up";
            RefreshModels();
            return;
        }
    }
}

void CChatDlg::OnSend()
{
    if (busy_)
    {
        if (cancel_)
        {
            cancel_->Cancel();
        }

        SetStatus("Stopping...");
        return;
    }
    CString inputW;
    input_.GetWindowText(inputW);
    std::string text = TrimCopy(U8(inputW));
    if (text.empty() && attachments_.empty())
    {
        return;
    }

    if (settings_.model.empty() || models_.empty())
    {
        SetStatus("No models yet (press \u21BB to look for models).");
        return;
    }

    if (worker_.joinable())
    {
        worker_.join();
    }

    // The user's message: text files and resources inline, images as image_url parts.
    std::string full = text;
    json images = json::array(), names = json::array();
    for (const auto& a : attachments_)
    {
        names.push_back(a.kind == Attachment::Kind::Resource ? a.name : Utf8(FileName(Wide(a.name))));
        if (a.kind == Attachment::Kind::Image)
        {
            images.push_back({ { "type", "image_url" },
                { "image_url", { { "url", "data:" + a.mime + ";base64," + Base64(a.content) } } } });
        }
        else if (a.kind == Attachment::Kind::Resource)
        {
            full += "\n\n<resource uri=\"" + a.name + "\">\n" + a.content + "\n</resource>";
        }
        else
        {
            full += "\n\n<file path=\"" + a.name + "\">\n" + a.content + "\n</file>";
        }
    }

    json user = { { "role", "user" } };
    if (images.empty())
    {
        user["content"] = full;
    }
    else
    {
        json parts = json::array({ { { "type", "text" }, { "text", full } } });
        for (auto& img : images)
        {
            parts.push_back(img);
        }

        user["content"] = parts;
    }

    if (full != text || !images.empty())
    {
        user["x_text"] = text;
        user["x_attachments"] = names;
    }

    // What Auto mode needs to know about this conversation: the last answer's model and kind.
    RouteRequest route;
    route.text = text;
    route.hasImages = !images.empty();
    for (const auto& a : attachments_)
    {
        route.attachmentNames.push_back(a.name);
    }

    for (auto it = current_->messages.rbegin(); it != current_->messages.rend(); ++it)
    {
        if (it->value("role", "") == "assistant" && it->contains("x_model"))
        {
            if (route.previousModel.empty())
            {
                route.previousModel = it->value("x_model", "");
            }

            if (it->contains("x_route"))
            {
                route.previousCategory = (*it)["x_route"].value("category", "");
                break;
            }
        }
    }

    bool isNew = current_->messages.empty();
    current_->messages.push_back(user);
    if (isNew)
    {
        std::string title = TrimCopy(text.empty() ? "Attachments" : text);
        size_t nl = title.find('\n');
        if (nl != std::string::npos)
        {
            title.resize(nl);
        }

        current_->title = Truncate(title, 60, "...");
        conversations_.insert(conversations_.begin(), current_);
        SetWindowText(W(current_->title) + L" - ModelHub Chat");
    }
    else
    {
        conversations_.erase(std::remove(conversations_.begin(), conversations_.end(), current_), conversations_.end());
        conversations_.insert(conversations_.begin(), current_);
    }

    current_->updated = NowEpoch();
    current_->Save();
    FillTree();
    input_.SetWindowText(L"");
    attachments_.clear();
    UpdateAttachments();
    SetBusy(true);
    RenderConversation();

    // The job: tools the user left on, and the conversation so far.
    auto job = std::make_shared<Job>();
    job->hwnd = m_hWnd;
    job->settings = settings_;
    job->model = settings_.model;
    job->workspace = WorkspaceOf(*current_);
    for (const auto& name : BuiltinToolNames())
    {
        if (!settings_.disabledTools.count(name))
        {
            job->tools.push_back(BuiltinToolDefinition(name));
        }
    }

    for (const auto& server : mcp_.Servers())
    {
        auto info = server->Snapshot();
        if (info.state != "connected" || !info.config.enabled)
        {
            continue;
        }

        for (const auto& t : info.tools)
        {
            std::string q = McpManager::QualifiedName(info.config.name, t.name);
            if (settings_.disabledTools.count(q))
            {
                continue;
            }

            job->tools.push_back({ { "type", "function" },
                { "function", { { "name", q }, { "description", t.description }, { "parameters", t.inputSchema } } } });
            job->mcpRoutes[q] = { server, t.name };
        }
    }

    std::string system = current_->systemMessage;
    if (!job->tools.empty())
    {
        if (!system.empty())
        {
            system += "\n\n";
        }

        system += "You can call tools. The workspace folder is " + Utf8(job->workspace) +
                  "; relative file paths are resolved there. The operating system is Windows.";
    }

    if (!system.empty())
    {
        job->messages.push_back({ { "role", "system" }, { "content", system } });
    }

    for (const auto& m : current_->messages)
    {
        job->messages.push_back(ForServer(m));
    }

    job->autoMode = settings_.model == "auto";
    for (const auto& m : models_)
    {
        std::string pid, name;
        if (!SplitQualified(m, pid, name))
        {
            job->models.push_back(m);  // ModelHub's
        }
        else if (const ProviderConfig* p = settings_.FindProvider(pid); p && p->enabled && p->inAuto)
        {
            job->models.push_back(m);
            if (p->Cloud())
            {
                job->cloudProviders.insert(pid);
            }
        }
    }

    route.toolCount = job->tools.size();
    for (const auto& m : job->messages)
    {
        route.promptChars += m.dump().size();
    }

    job->routeReq = route;
    pendingRoute_ = json();
    cancel_ = std::make_shared<CancelToken>();
    job->cancel = cancel_;
    streamKind_ = 0;
    worker_ = std::thread([job]
        { RunJob(job); });
}

LRESULT CChatDlg::OnAppEvent(WPARAM, LPARAM lp)
{
    std::unique_ptr<UiEvent> ev(reinterpret_cast<UiEvent*>(lp));
    switch (ev->kind)
    {
    case UiEvent::Kind::Route:
        pendingRoute_ = ev->data;
        AppendRoute(json{ { "x_route", ev->data } });
        SetStatus("Auto: " + DisplayModel(settings_, ev->data.value("model", "")) + " for " + ev->data.value("category", "") +
                  " - loading it if needed...");
        ScrollToEnd();
        break;
    case UiEvent::Kind::AssistantBegin:
    {
        Style header;
        header.bold = true;
        Append(L"Assistant", header);
        Style model;
        model.color = kMuted;
        model.size = 9;
        Append(L"  \u00B7  " + Wide(DisplayModel(settings_, ev->text)) + L"\n", model);
        streamKind_ = 0;
        ScrollToEnd();
        break;
    }
    case UiEvent::Kind::Delta:
    {
        if (!waitStatus_.empty())
        {
            // The answer has started: the footer stops counting.
            waitStatus_.clear();
            KillTimer(7);
            footer_.SetStatus(L"Writing...");
        }

        if (ev->delta == DeltaKind::Reasoning)
        {
            if (!settings_.showReasoning)
            {
                break;
            }

            Style r;
            r.color = kMuted;
            r.italic = true;
            r.size = 10;

            if (streamKind_ != 1)
            {
                Style label = r;
                label.bold = true;
                Append(L"Thinking\n", label);
                streamKind_ = 1;
            }

            Append(Crlf(ev->text), r);
        }
        else
        {
            if (streamKind_ == 1)
            {
                Append(L"\n\n", Style{});
            }

            streamKind_ = 2;
            Append(Crlf(ev->text), Style{});
        }

        ScrollToEnd();
        break;
    }
    case UiEvent::Kind::AssistantDone:
        if (!pendingRoute_.is_null())
        {
            ev->data["x_route"] = pendingRoute_;  // on the first answer of the turn
            pendingRoute_ = json();
        }

        if (!pendingToolNotes_.empty())
        {
            ev->data["x_tool_notes"] = pendingToolNotes_;  // shown again when the conversation is drawn
            pendingToolNotes_ = json::array();
        }

        current_->messages.push_back(ev->data);
        current_->updated = NowEpoch();
        current_->Save();

        if (ev->data.contains("tool_calls"))
        {
            Append(L"\n", Style{});
            for (const auto& tc : ev->data["tool_calls"])
            {
                Style t;
                t.code = true;
                t.color = RGB(80, 80, 90);
                t.back = kCodeBack;
                t.size = 9;
                Append(Crlf("→ " + tc["function"].value("name", "") + " " +
                            Truncate(tc["function"].value("arguments", ""), 400, "...")) +
                           L"\n",
                    t);
            }
        }

        Append(L"\n", Style{});
        ScrollToEnd();
        break;
    case UiEvent::Kind::ToolStart:
        break;  // shown with the assistant's tool calls
    case UiEvent::Kind::ToolNote:
        pendingToolNotes_.push_back(ev->data);  // kept with the answer (display only)
        AppendToolNote(ev->data);
        ScrollToEnd();
        break;
    case UiEvent::Kind::ToolResult:
        current_->messages.push_back(ev->data);
        current_->Save();
        AppendMessage(ev->data);
        ScrollToEnd();
        break;
    case UiEvent::Kind::Status:
        SetStatus(ev->text);
        break;
    case UiEvent::Kind::Finished:
        pendingRoute_ = json();
        pendingToolNotes_ = json::array();
        if (worker_.joinable())
        {
            worker_.join();
        }

        SetBusy(false);
        current_->updated = NowEpoch();
        current_->Save();
        RenderConversation();  // with formatting
        if (ev->text == "stopped")
        {
            SetStatus("Stopped");
        }
        else if (!ev->text.empty())
        {
            SetStatus("Error: " + ev->text);
            Style e;
            e.color = RGB(185, 28, 28);
            Append(Crlf("Error: " + ev->text) + L"\n", e);
            ScrollToEnd();
        }

        input_.SetFocus();
        break;
    }
    return 0;
}

LRESULT CChatDlg::OnConfirm(WPARAM, LPARAM lp)
{
    std::unique_ptr<std::shared_ptr<ConfirmRequest>> holder(reinterpret_cast<std::shared_ptr<ConfirmRequest>*>(lp));
    auto& req = *holder;
    CString text = W(Truncate(req->detail, 3000, "\n...")) + L"\n\nAllow it?";
    text.Replace(L"\r\n", L"\n");
    text.Replace(L"\n", L"\r\n");
    int answer = MessageBoxW(text, W(req->title), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    req->answer.set_value(answer == IDYES);
    return 0;
}

// ---------------------------------------------------------------- the "+" menu

void CChatDlg::OnPlus()
{
    CMenu menu, reasoning, servers, resources;
    menu.CreatePopupMenu();
    reasoning.CreatePopupMenu();
    servers.CreatePopupMenu();
    resources.CreatePopupMenu();

    int current = 0;
    for (int i = 0; i < 5; ++i)
    {
        reasoning.AppendMenu(MF_STRING, ID_REASONING_FIRST + i, kReasoningLabels[i]);
        if (settings_.reasoning == kReasoningIds[i])
        {
            current = i;
        }
    }

    reasoning.CheckMenuRadioItem(ID_REASONING_FIRST, ID_REASONING_FIRST + 4, ID_REASONING_FIRST + current, MF_BYCOMMAND);

    auto list = mcp_.Servers();
    std::vector<std::pair<std::shared_ptr<McpServer>, McpResource>> resourceItems;
    for (size_t i = 0; i < list.size() && i < 100; ++i)
    {
        auto info = list[i]->Snapshot();
        std::string label = info.config.name + "    " + info.state;
        if (info.state == "connected")
        {
            label += " (" + std::to_string(info.tools.size()) + " tools)";
        }

        servers.AppendMenu(MF_STRING | (info.config.enabled ? MF_CHECKED : 0), ID_MCP_TOGGLE_FIRST + static_cast<UINT>(i),
            W(Truncate(label, 80, "...")));
        for (const auto& r : info.resources)
        {
            if (resourceItems.size() < 150)
            {
                resourceItems.push_back({ list[i], r });
            }
        }
    }

    if (!list.empty())
    {
        servers.AppendMenu(MF_SEPARATOR);
    }

    servers.AppendMenu(MF_STRING | (list.empty() ? MF_GRAYED : 0), ID_MCP_RECONNECT, L"Reconnect all");
    servers.AppendMenu(MF_STRING, ID_MCP_MANAGE, L"Manage MCP servers...");
    for (size_t i = 0; i < resourceItems.size(); ++i)
    {
        resources.AppendMenu(MF_STRING, ID_MCP_RESOURCE_FIRST + static_cast<UINT>(i),
            W(Truncate(resourceItems[i].first->Config().name + ": " + resourceItems[i].second.name, 90, "...")));
    }

    if (resourceItems.empty())
    {
        resources.AppendMenu(MF_STRING | MF_GRAYED, 0, L"(no resources)");
    }

    size_t toolsOn = 0, toolsAll = 0;
    for (const auto& n : BuiltinToolNames())
    {
        ++toolsAll;
        toolsOn += settings_.disabledTools.count(n) ? 0 : 1;
    }

    for (const auto& s : list)
    {
        auto info = s->Snapshot();
        if (info.state != "connected")
        {
            continue;
        }

        for (const auto& t : info.tools)
        {
            ++toolsAll;
            toolsOn += settings_.disabledTools.count(McpManager::QualifiedName(info.config.name, t.name)) ? 0 : 1;
        }
    }

    CString reasoningLabel;
    reasoningLabel.Format(L"Reasoning: %s", kReasoningLabels[current]);
    CString toolsLabel;
    toolsLabel.Format(L"Tools (%zu/%zu)...", toolsOn, toolsAll);
    menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(reasoning.m_hMenu), reasoningLabel);
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_ADD_FILES, L"Add files...");
    bool canPaste = IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_DIB) ||
                    IsClipboardFormatAvailable(CF_HDROP);
    menu.AppendMenu(MF_STRING | (canPaste ? 0 : MF_GRAYED), ID_PASTE, L"Paste image, files or text\tCtrl+V");
    menu.AppendMenu(MF_STRING, ID_SYSTEM_MESSAGE, L"System Message...");
    menu.AppendMenu(MF_STRING, ID_TOOLS, toolsLabel);
    menu.AppendMenu(MF_STRING | (settings_.model == "auto" ? MF_CHECKED : 0), ID_AUTO_SETTINGS, L"Auto mode...");
    menu.AppendMenu(MF_STRING, ID_PROVIDERS, L"Model providers (Claude, OpenAI, Ollama, GitHub)...");
    menu.AppendMenu(MF_STRING, ID_CLAUDE_SETUP, L"Set up Claude (install Anthropic's CLI, sign in)...");
    menu.AppendMenu(MF_STRING | (busy_ ? MF_GRAYED : 0), ID_OLLAMA_MOVE, L"Move Ollama models to ModelHub...");
    menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(servers.m_hMenu), L"MCP Servers");
    menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(resources.m_hMenu), L"MCP Resources");

    CRect btn;
    GetDlgItem(IDC_PLUS)->GetWindowRect(&btn);
    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, btn.left, btn.top - Px(4), this);

    // The submenus belong to menu now: detach them so they are not destroyed twice.
    reasoning.Detach();
    servers.Detach();
    resources.Detach();
    if (cmd == 0)
    {
        return;
    }

    if (cmd >= ID_REASONING_FIRST && cmd < ID_REASONING_FIRST + 5)
    {
        settings_.reasoning = kReasoningIds[cmd - ID_REASONING_FIRST];
        settings_.Save();
        SetStatus(std::string("Reasoning: ") + Utf8(kReasoningLabels[cmd - ID_REASONING_FIRST]));
        UpdateFooter();
    }
    else if (cmd == ID_PASTE)
    {
        if (!PasteFromClipboard())
        {
            input_.Paste();  // short text: into the message box
        }

        input_.SetFocus();
    }
    else if (cmd == ID_ADD_FILES)
    {
        CFileDialog dlg(TRUE, nullptr, nullptr, OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER,
            L"All files (*.*)|*.*|Images (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp)|*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp||",
            this);
        std::vector<wchar_t> buf(64 * 1024, L'\0');
        dlg.m_ofn.lpstrFile = buf.data();
        dlg.m_ofn.nMaxFile = static_cast<DWORD>(buf.size());
        std::wstring dir = current_ ? WorkspaceOf(*current_) : std::wstring();
        dlg.m_ofn.lpstrInitialDir = dir.c_str();
        if (dlg.DoModal() == IDOK)
        {
            for (POSITION pos = dlg.GetStartPosition(); pos;)
            {
                AddAttachment(std::wstring(dlg.GetNextPathName(pos).GetString()));
            }
        }
    }
    else if (cmd == ID_SYSTEM_MESSAGE)
    {
        CSystemMessageDlg dlg(this, current_->systemMessage);
        if (dlg.DoModal() == IDOK)
        {
            current_->systemMessage = dlg.text;
            if (dlg.makeDefault)
            {
                settings_.defaultSystemMessage = dlg.text;
                settings_.Save();
            }

            SaveCurrent();
            RenderConversation();
        }
    }
    else if (cmd == ID_TOOLS)
    {
        ShowToolsDialog();
    }
    else if (cmd == ID_AUTO_SETTINGS)
    {
        ShowAutoDialog();
    }
    else if (cmd == ID_PROVIDERS)
    {
        OnProviders();
    }
    else if (cmd == ID_CLAUDE_SETUP)
    {
        OfferClaudeSetup(true);
    }
    else if (cmd == ID_OLLAMA_MOVE)
    {
        CWaitCursor wait;
        OfferOllamaMove(FindOllamaModels(), true);
    }
    else if (cmd == ID_MCP_MANAGE)
    {
        OnMcpServers();
    }
    else if (cmd == ID_MCP_RECONNECT)
    {
        mcp_.Reconnect(m_hWnd);
    }
    else if (cmd >= ID_MCP_TOGGLE_FIRST && cmd < ID_MCP_TOGGLE_FIRST + 100)
    {
        size_t i = cmd - ID_MCP_TOGGLE_FIRST;
        if (i < list.size())
        {
            std::string name = list[i]->Config().name;
            for (auto& c : settings_.mcpServers)
            {
                if (c.name == name)
                {
                    c.enabled = !c.enabled;
                }
            }

            settings_.Save();
            ApplyMcp();
        }
    }
    else if (cmd >= ID_MCP_RESOURCE_FIRST && cmd < ID_MCP_RESOURCE_FIRST + 150)
    {
        size_t i = cmd - ID_MCP_RESOURCE_FIRST;
        if (i < resourceItems.size())
        {
            AttachResource(resourceItems[i].first, resourceItems[i].second);
        }
    }
}

void CChatDlg::AttachResource(const std::shared_ptr<McpServer>& server, const McpResource& r)
{
    CWaitCursor wait;
    std::string error;
    std::string text = server->ReadResource(r.uri, error);
    if (!error.empty())
    {
        SetStatus("Cannot read " + r.uri + ": " + error);
        return;
    }

    Attachment a;
    a.kind = Attachment::Kind::Resource;
    a.name = r.uri;
    a.content = Truncate(text, 512 * 1024, "\n... (truncated at 512 KB)");
    attachments_.push_back(std::move(a));
    UpdateAttachments();
}

void CChatDlg::ShowToolsDialog()
{
    std::vector<CToolsDlg::Group> groups;
    CToolsDlg::Group builtin;
    builtin.label = "Built-in";
    for (const auto& n : BuiltinToolNames())
    {
        builtin.tools.push_back({ n, n });
    }

    groups.push_back(builtin);
    for (const auto& s : mcp_.Servers())
    {
        auto info = s->Snapshot();
        if (info.state != "connected" || info.tools.empty())
        {
            continue;
        }

        CToolsDlg::Group g;
        g.label = info.config.name;
        for (const auto& t : info.tools)
        {
            g.tools.push_back({ McpManager::QualifiedName(info.config.name, t.name), t.name });
        }

        groups.push_back(g);
    }

    CToolsDlg dlg(this, groups, settings_.disabledTools);
    if (dlg.DoModal() == IDOK)
    {
        settings_.disabledTools = dlg.disabled;
        settings_.Save();
    }
}

// ---------------------------------------------------------------- MCP and settings

void CChatDlg::ApplyMcp()
{
    mcp_.Apply(settings_.mcpServers, m_hWnd);
}

LRESULT CChatDlg::OnMcpChanged(WPARAM, LPARAM)
{
    int connected = 0, failed = 0, connecting = 0;
    for (const auto& s : mcp_.Servers())
    {
        auto info = s->Snapshot();
        if (info.state == "connected")
        {
            ++connected;
        }
        else if (info.state == "connecting")
        {
            ++connecting;
        }
        else if (info.state.rfind("error", 0) == 0)
        {
            ++failed;
        }
    }

    CString label = L"MCP Servers";
    if (connected + failed + connecting > 0)
    {
        label.Format(L"MCP Servers  (%d on%s)", connected, failed ? L", some failed" : connecting ? L", starting"
                                                                                                  : L"");
    }

    sidebar_.SetMcpLabel(label.GetString());
    return 0;
}

void CChatDlg::OnMcpServers()
{
    CMcpServersDlg dlg(this, settings_.mcpServers, mcp_);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    settings_.mcpServers = dlg.configs;
    settings_.Save();
    if (dlg.reconnect)
    {
        mcp_.Apply(settings_.mcpServers, m_hWnd);
        mcp_.Reconnect(m_hWnd);
    }
    else
    {
        ApplyMcp();
    }
}

void CChatDlg::ShowAutoDialog()
{
    CAutoDlg dlg(this, settings_, models_);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    settings_ = dlg.s;
    settings_.model = "auto";  // the dialog is about Auto mode: turn it on
    settings_.Save();
    if (model_.GetCount() > 0)
    {
        model_.SetCurSel(0);
    }

    SetStatus("Auto mode: each message goes to the model that suits it best.");
}

void CChatDlg::StartDownload(const std::string& id, unsigned long long size)
{
    if (downloading_.count(id))
    {
        return;
    }

    CString text = L"Download " + W(id) + (size ? L" (" + W(FormatGB(size)) + L")" : CString()) +
                   L" with ModelHub?\n\nIt goes into the shared model store, so every application on this PC can use "
                   L"it. Auto mode uses it once it is there.";
    if (MessageBoxW(text, L"Download a model", MB_YESNO | MB_ICONQUESTION) != IDYES)
    {
        return;
    }

    downloading_.insert(id);
    if (!busy_)
    {
        RenderConversation();
    }

    Settings s = settings_;
    HWND hwnd = m_hWnd;
    auto work = [s, hwnd, id]()
    {
        auto post = [hwnd, id](const std::string& msg, bool finished, bool ok)
        {
            auto* e = new DownloadEvent{ id, msg, finished, ok };
            if (!::PostMessage(hwnd, WM_APP_DOWNLOAD, 0, reinterpret_cast<LPARAM>(e)))
            {
                delete e;
            }
        };

        Endpoint ep;
        std::string error;
        if (!ResolveEndpoint(s, ep, error) || !ep.modelhub)
        {
            return post("Cannot download " + id + ": " + (error.empty() ? "downloads need the ModelHub service" : error), true, false);
        }

        std::string root = ep.baseUrl.substr(0, ep.baseUrl.size() - 3);
        HttpHeaders h = ep.headers;
        h.push_back({ "Content-Type", "application/json" });
        HttpResult r = HttpSend(L"POST", root + "/hub/v1/downloads", json{ { "model", id } }.dump(), h, 30000);
        if (!r.Ok())
        {
            return post("Cannot download " + id + ": " +
                            (r.status ? "HTTP " + std::to_string(r.status) + " " + Truncate(r.body, 200) : r.error),
                true, false);
        }

        for (int missing = 0;;)
        {
            Sleep(1000);
            HttpResult d = HttpSend(L"GET", root + "/hub/v1/downloads", "", ep.headers, 15000);
            json j = json::parse(d.Ok() ? d.body : std::string("{}"), nullptr, false);
            json entry;
            if (j.is_object() && j.contains("downloads"))
            {
                for (const auto& x : j["downloads"])
                {
                    if (x.value("model", "") == id)
                    {
                        entry = x;
                    }
                }
            }

            if (entry.is_null())
            {
                if (++missing > 10)
                {
                    break;  // finished and forgotten: checked below
                }

                continue;
            }

            if (!entry.value("running", false))
            {
                std::string err = entry.value("error", "");
                if (!err.empty())
                {
                    return post("Download of " + id + " failed: " + err, true, false);
                }

                break;
            }

            double done = entry.value("done", 0.0), total = entry.value("total", 0.0);
            double bps = entry.value("bytes_per_second", 0.0);
            char buf[256];
            std::snprintf(buf, sizeof buf, "Downloading %s: %s %.0f%% (%s of %s, %.1f MB/s)", id.c_str(),
                entry.value("phase", "").c_str(), total > 0 ? done * 100 / total : 0.0,
                FormatGB(static_cast<unsigned long long>(done)).c_str(),
                FormatGB(static_cast<unsigned long long>(total)).c_str(), bps / (1024 * 1024));
            post(buf, false, false);
        }

        HttpResult m = HttpSend(L"GET", root + "/hub/v1/models/" + id, "", ep.headers, 15000);
        bool installed = m.Ok() && m.body.find("\"installed\":true") != std::string::npos;
        post(installed ? "Downloaded " + id + ": Auto mode can use it now." : "The download of " + id + " did not finish.",
            true, installed);
    };

    std::thread(work).detach();
}

LRESULT CChatDlg::OnDownloadProgress(WPARAM, LPARAM lp)
{
    std::unique_ptr<DownloadEvent> e(reinterpret_cast<DownloadEvent*>(lp));
    SetStatus(e->text);
    if (e->finished)
    {
        downloading_.erase(e->id);
        if (e->ok)
        {
            RefreshModels();
        }

        if (!busy_)
        {
            RenderConversation();
        }

        SetStatus(e->text);
    }

    return 0;
}

void CChatDlg::OnSettings()
{
    CSettingsDlg dlg(this, settings_);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    bool endpointChanged = dlg.s.backend != settings_.backend || dlg.s.customUrl != settings_.customUrl ||
                           dlg.s.customKey != settings_.customKey;
    bool reasoningShown = dlg.s.showReasoning != settings_.showReasoning;
    settings_ = dlg.s;
    settings_.Save();
    if (endpointChanged)
    {
        RefreshModels();
    }

    if (reasoningShown && !busy_)
    {
        RenderConversation();
    }

    UpdateWorkspaceLabel();
}

// ---------------------------------------------------------------- toolbar and footer

void CChatDlg::OnDrawItem(int id, LPDRAWITEMSTRUCT di)
{
    if (id == IDC_PLUS)
    {
        DrawIconButton(*di, kGlyphAdd, false, false);
    }
    else if (id == IDC_SEND)
    {
        DrawIconButton(*di, busy_ ? kGlyphStop : kGlyphSend, busy_, false);
    }
    else if (id == IDC_MIC)
    {
        DrawIconButton(*di, kGlyphMic, false, false);
    }
    else
    {
        CDialogEx::OnDrawItem(id, di);
    }
}

void CChatDlg::OnToolbar(int button, CPoint screen)
{
    switch (button)
    {
    case CTitleStrip::BtnShowSidebar:
        settings_.sidebarVisible = true;
        settings_.Save();
        Layout();
        break;
    case CTitleStrip::BtnToggleInfo:
        settings_.infoVisible = !settings_.infoVisible;
        settings_.Save();
        Layout();
        break;
    case CTitleStrip::BtnTools:
        ShowToolsDialog();
        break;
    case CTitleStrip::BtnTerminal:
        OpenTerminal();
        break;
    case CTitleStrip::BtnFolder:
        if (current_)
        {
            ShellExecuteW(m_hWnd, L"open", WorkspaceOf(*current_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        break;
    case CTitleStrip::BtnMore:
    {
        CMenu menu;
        menu.CreatePopupMenu();
        menu.AppendMenu(MF_STRING | (busy_ ? MF_GRAYED : 0), ID_NEW_CHAT_MENU, L"New chat\tCtrl+N");
        menu.AppendMenu(MF_STRING | (current_ && !current_->messages.empty() ? 0 : MF_GRAYED), ID_EXPORT,
            L"Export conversation as Markdown...");
        menu.AppendMenu(MF_STRING | (busy_ ? MF_GRAYED : 0), ID_WORKSPACE_MENU, L"Change workspace folder...");
        menu.AppendMenu(MF_SEPARATOR);
        menu.AppendMenu(MF_STRING, ID_MCP_MENU, L"MCP servers...");
        menu.AppendMenu(MF_STRING, ID_PROVIDERS, L"Model providers...");
        menu.AppendMenu(MF_STRING, ID_SETTINGS_MENU, L"Settings...");
        UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, screen.x, screen.y, this);
        if (cmd == ID_NEW_CHAT_MENU)
        {
            OnNewChat();
        }
        else if (cmd == ID_EXPORT && current_)
        {
            ExportConversation(*current_);
        }
        else if (cmd == ID_WORKSPACE_MENU)
        {
            OnWorkspaceChange();
        }
        else if (cmd == ID_MCP_MENU)
        {
            OnMcpServers();
        }
        else if (cmd == ID_PROVIDERS)
        {
            OnProviders();
        }
        else if (cmd == ID_SETTINGS_MENU)
        {
            OnSettings();
        }
        break;
    }
    }
}

void CChatDlg::OpenTerminal()
{
    std::wstring folder = current_ ? WorkspaceOf(*current_) : Wide(settings_.defaultWorkspace);
    // Windows Terminal when it is installed, else PowerShell, in the workspace folder.
    std::wstring args = L"-d \"" + folder + L"\"";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(m_hWnd, L"open", L"wt.exe", args.c_str(), nullptr, SW_SHOWNORMAL)) > 32)
    {
        return;
    }

    ShellExecuteW(m_hWnd, L"open", L"powershell.exe", L"-NoExit", folder.c_str(), SW_SHOWNORMAL);
}

void CChatDlg::UpdateFooter()
{
    std::wstring model = settings_.model == "auto" ? L"Auto" : Wide(DisplayModel(settings_, settings_.model));
    std::wstring effort;
    for (int i = 0; i < 5; ++i)
    {
        if (settings_.reasoning == kReasoningIds[i])
        {
            effort = i == 0 ? L"Default effort" : i == 1 ? L"No thinking"
                                                         : kReasoningLabels[i];
        }
    }

    footer_.SetModel(model, effort, !busy_);
}

void CChatDlg::ShowModelMenu(CPoint screen)
{
    if (busy_)
    {
        return;
    }

    CMenu menu;
    menu.CreatePopupMenu();
    std::vector<std::unique_ptr<CMenu>> subs;  // one submenu per provider
    std::map<std::string, CMenu*> byProvider;
    auto flags = [this](size_t i)
    { return MF_STRING | (comboIds_[i] == settings_.model ? MF_CHECKED : 0); };
    for (size_t i = 0; i < comboIds_.size() && i < 2000; ++i)
    {
        UINT id = ID_MODEL_FIRST + static_cast<UINT>(i);
        std::string pid, pmodel;
        if (i == 0)
        {
            menu.AppendMenu(flags(i), id, L"Auto (picks a model for each message)");
            menu.AppendMenu(MF_SEPARATOR);
        }
        else if (SplitQualified(comboIds_[i], pid, pmodel))
        {
            auto& sub = byProvider[pid];
            if (!sub)
            {
                subs.push_back(std::make_unique<CMenu>());
                subs.back()->CreatePopupMenu();
                sub = subs.back().get();
            }
            sub->AppendMenu(flags(i), id, Wide(pmodel).c_str());
        }
        else
        {
            menu.AppendMenu(flags(i), id, Wide(comboIds_[i]).c_str());
        }
    }

    if (!byProvider.empty())
    {
        menu.AppendMenu(MF_SEPARATOR);
    }

    for (auto& [pid, sub] : byProvider)
    {
        const ProviderConfig* p = settings_.FindProvider(pid);
        std::string pid2, pmodel;
        bool chosen = SplitQualified(settings_.model, pid2, pmodel) && pid2 == pid;
        menu.AppendMenu(MF_POPUP | (chosen ? MF_CHECKED : 0), reinterpret_cast<UINT_PTR>(sub->m_hMenu),
            Wide(p ? p->name : pid).c_str());
    }

    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_MODEL_REFRESH, L"Refresh the model list");
    menu.AppendMenu(MF_STRING, ID_PROVIDERS, L"Model providers...");
    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, screen.x, screen.y - Px(2), this);
    for (auto& sub : subs)
    {
        sub->Detach();  // destroyed with the menu
    }

    if (cmd >= ID_MODEL_FIRST && cmd < ID_MODEL_FIRST + comboIds_.size())
    {
        model_.SetCurSel(static_cast<int>(cmd - ID_MODEL_FIRST));
        OnModelChange();
    }
    else if (cmd == ID_MODEL_REFRESH)
    {
        RefreshModels();
    }
    else if (cmd == ID_PROVIDERS)
    {
        OnProviders();
    }
}

void CChatDlg::ShowEffortMenu(CPoint screen)
{
    if (busy_)
    {
        return;
    }

    CMenu menu;
    menu.CreatePopupMenu();
    int current = 0;
    for (int i = 0; i < 5; ++i)
    {
        menu.AppendMenu(MF_STRING, ID_REASONING_FIRST + i, kReasoningLabels[i]);
        if (settings_.reasoning == kReasoningIds[i])
        {
            current = i;
        }
    }

    menu.CheckMenuRadioItem(ID_REASONING_FIRST, ID_REASONING_FIRST + 4, ID_REASONING_FIRST + current, MF_BYCOMMAND);
    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, screen.x, screen.y - Px(2), this);
    if (cmd >= ID_REASONING_FIRST && cmd < ID_REASONING_FIRST + 5)
    {
        settings_.reasoning = kReasoningIds[cmd - ID_REASONING_FIRST];
        settings_.Save();
        UpdateFooter();
    }
}

void CChatDlg::OnInputChange()
{
    // Grow or shrink the composer when the number of lines changes.
    int lines = std::clamp(input_.GetLineCount(), 1, 8);
    if (lines != inputLines_)
    {
        inputLines_ = lines;
        Layout();
    }
}

void CChatDlg::OnMic()
{
    // Windows' voice typing (Win+H) types into the focused control: the message box.
    input_.SetFocus();
    INPUT keys[4] = {};
    for (auto& k : keys)
    {
        k.type = INPUT_KEYBOARD;
    }

    keys[0].ki.wVk = VK_LWIN;
    keys[1].ki.wVk = 'H';
    keys[2].ki.wVk = 'H';
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_LWIN;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(4, keys, sizeof(INPUT)) != 4)
    {
        SetStatus("Voice typing: press Windows+H in the message box");
    }
    else
    {
        SetStatus("Voice typing: speak now; Windows types into the message box (Windows+H or the mic stops it)");
    }
}

}  // namespace chat
