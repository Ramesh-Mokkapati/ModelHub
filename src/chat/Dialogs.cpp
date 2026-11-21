#include "Dialogs.h"

#include "AppMessages.h"
#include "Providers.h"
#include "Router.h"
#include "Util.h"

#include "modelhub/modelhub_client.h"

namespace chat
{

static std::string GetText(CWnd* dlg, int id)
{
    CString s;
    dlg->GetDlgItemText(id, s);
    return U8(s);
}

static void SetText(CWnd* dlg, int id, const std::string& text)
{
    dlg->SetDlgItemText(id, W(text));
}

// Edit controls want \r\n.
static CString ToCrLf(const std::string& text)
{
    CString s = W(text);
    s.Replace(L"\r\n", L"\n");
    s.Replace(L"\n", L"\r\n");
    return s;
}

static std::string FromCrLf(CString s)
{
    s.Replace(L"\r\n", L"\n");
    return U8(s);
}

std::wstring FindManagerExe()
{
    std::wstring local = ExeDir() + L"\\ModelHubManager.exe";
    if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        return local;
    }

    std::string text, exe;
    if (modelhub::client::ReadFileUtf8(modelhub::client::HubHome() + L"\\install.json", text) &&
        modelhub::client::JsonString(text, "exe", exe))
    {
        std::wstring hub = Wide(exe);
        size_t slash = hub.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
        {
            std::wstring mgr = hub.substr(0, slash) + L"\\ModelHubManager.exe";
            if (GetFileAttributesW(mgr.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                return mgr;
            }
        }
    }

    return {};
}

// ---------------------------------------------------------------- system message

CSystemMessageDlg::CSystemMessageDlg(CWnd* parent, const std::string& t)
    : CDialogEx(IDD_SYSTEM_MESSAGE, parent)
    , text(t)
{
}

BOOL CSystemMessageDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetDlgItemText(IDC_SYSTEM_TEXT, ToCrLf(text));
    return TRUE;
}

void CSystemMessageDlg::OnOK()
{
    CString s;
    GetDlgItemText(IDC_SYSTEM_TEXT, s);
    text = TrimCopy(FromCrLf(s));
    makeDefault = IsDlgButtonChecked(IDC_SYSTEM_DEFAULT) == BST_CHECKED;
    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- tools

BEGIN_MESSAGE_MAP(CToolsDlg, CDialogEx)
    ON_NOTIFY(NM_CLICK, IDC_TOOLS_TREE, &CToolsDlg::OnTreeClick)
    ON_NOTIFY(TVN_KEYDOWN, IDC_TOOLS_TREE, &CToolsDlg::OnTreeKey)
    ON_MESSAGE(WM_APP_TREE_CHECK, &CToolsDlg::OnTreeCheck)
    ON_BN_CLICKED(IDC_TOOLS_ALL, &CToolsDlg::OnAll)
    ON_BN_CLICKED(IDC_TOOLS_NONE, &CToolsDlg::OnNone)
END_MESSAGE_MAP()

static constexpr DWORD_PTR kGroupItem = static_cast<DWORD_PTR>(-1);

CToolsDlg::CToolsDlg(CWnd* parent, std::vector<Group> groups, std::set<std::string> d)
    : CDialogEx(IDD_TOOLS, parent)
    , disabled(std::move(d))
    , groups_(std::move(groups))
{
}

BOOL CToolsDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    tree_.SubclassDlgItem(IDC_TOOLS_TREE, this);

    // Check boxes work only when the style is set after the control exists.
    tree_.ModifyStyle(TVS_CHECKBOXES, 0);
    tree_.ModifyStyle(0, TVS_CHECKBOXES);
    for (const auto& g : groups_)
    {
        HTREEITEM group = tree_.InsertItem(W(g.label));
        tree_.SetItemData(group, kGroupItem);
        for (const auto& [name, label] : g.tools)
        {
            HTREEITEM item = tree_.InsertItem(W(label), group);
            names_.push_back(name);
            tree_.SetItemData(item, names_.size() - 1);
            tree_.SetCheck(item, disabled.count(name) == 0);
        }

        UpdateGroupLabel(group);
        tree_.Expand(group, TVE_EXPAND);
    }

    if (groups_.empty())
    {
        tree_.InsertItem(L"(no tools)");
    }

    return TRUE;
}

void CToolsDlg::UpdateGroupLabel(HTREEITEM group)
{
    int total = 0, on = 0;
    for (HTREEITEM c = tree_.GetChildItem(group); c; c = tree_.GetNextSiblingItem(c))
    {
        ++total;
        on += tree_.GetCheck(c) ? 1 : 0;
    }

    size_t index = 0;
    for (HTREEITEM g = tree_.GetRootItem(); g && g != group; g = tree_.GetNextSiblingItem(g))
    {
        ++index;
    }

    if (index < groups_.size())
    {
        CString label;
        label.Format(L"%s    %d/%d", W(groups_[index].label).GetString(), on, total);
        tree_.SetItemText(group, label);
    }

    tree_.SetCheck(group, total > 0 && on == total);
}

void CToolsDlg::OnTreeClick(NMHDR*, LRESULT* result)
{
    DWORD pos = GetMessagePos();
    CPoint pt(static_cast<short>(LOWORD(pos)), static_cast<short>(HIWORD(pos)));
    tree_.ScreenToClient(&pt);
    UINT flags = 0;
    HTREEITEM item = tree_.HitTest(pt, &flags);
    if (item && (flags & TVHT_ONITEMSTATEICON))
    {
        PostMessage(WM_APP_TREE_CHECK, 0, reinterpret_cast<LPARAM>(item));  // after the tree toggled it
    }

    *result = 0;
}

void CToolsDlg::OnTreeKey(NMHDR* nm, LRESULT* result)
{
    auto* key = reinterpret_cast<NMTVKEYDOWN*>(nm);
    if (key->wVKey == VK_SPACE)
    {
        if (HTREEITEM item = tree_.GetSelectedItem())
        {
            PostMessage(WM_APP_TREE_CHECK, 0, reinterpret_cast<LPARAM>(item));
        }
    }

    *result = 0;
}

LRESULT CToolsDlg::OnTreeCheck(WPARAM, LPARAM lp)
{
    auto item = reinterpret_cast<HTREEITEM>(lp);
    if (tree_.GetItemData(item) == kGroupItem)
    {
        BOOL on = tree_.GetCheck(item);
        for (HTREEITEM c = tree_.GetChildItem(item); c; c = tree_.GetNextSiblingItem(c))
        {
            tree_.SetCheck(c, on);
        }

        UpdateGroupLabel(item);
    }
    else if (HTREEITEM parent = tree_.GetParentItem(item))
    {
        UpdateGroupLabel(parent);
    }

    return 0;
}

void CToolsDlg::SetAll(BOOL check)
{
    for (HTREEITEM g = tree_.GetRootItem(); g; g = tree_.GetNextSiblingItem(g))
    {
        for (HTREEITEM c = tree_.GetChildItem(g); c; c = tree_.GetNextSiblingItem(c))
        {
            tree_.SetCheck(c, check);
        }

        UpdateGroupLabel(g);
    }
}

void CToolsDlg::OnAll()
{
    SetAll(TRUE);
}

void CToolsDlg::OnNone()
{
    SetAll(FALSE);
}

void CToolsDlg::OnOK()
{
    for (HTREEITEM g = tree_.GetRootItem(); g; g = tree_.GetNextSiblingItem(g))
    {
        for (HTREEITEM c = tree_.GetChildItem(g); c; c = tree_.GetNextSiblingItem(c))
        {
            DWORD_PTR index = tree_.GetItemData(c);
            if (index >= names_.size())
            {
                continue;
            }

            if (tree_.GetCheck(c))
            {
                disabled.erase(names_[index]);
            }
            else
            {
                disabled.insert(names_[index]);
            }
        }
    }

    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- MCP server (one)

BEGIN_MESSAGE_MAP(CMcpEditDlg, CDialogEx)
    ON_CBN_SELCHANGE(IDC_MCP_TRANSPORT, &CMcpEditDlg::OnTransport)
END_MESSAGE_MAP()

CMcpEditDlg::CMcpEditDlg(CWnd* parent, McpServerConfig c)
    : CDialogEx(IDD_MCP_EDIT, parent)
    , cfg(std::move(c))
{
}

BOOL CMcpEditDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    auto* transport = static_cast<CComboBox*>(GetDlgItem(IDC_MCP_TRANSPORT));
    transport->AddString(L"stdio (run a command)");
    transport->AddString(L"HTTP (streamable, by URL)");
    transport->SetCurSel(cfg.transport == "http" ? 1 : 0);
    SetText(this, IDC_MCP_NAME, cfg.name);
    SetText(this, IDC_MCP_COMMAND, cfg.command);
    SetText(this, IDC_MCP_URL, cfg.url);
    SetText(this, IDC_MCP_TOKEN, cfg.token);
    CheckDlgButton(IDC_MCP_ENABLED, cfg.enabled ? BST_CHECKED : BST_UNCHECKED);
    OnTransport();
    return TRUE;
}

void CMcpEditDlg::OnTransport()
{
    bool http = static_cast<CComboBox*>(GetDlgItem(IDC_MCP_TRANSPORT))->GetCurSel() == 1;
    GetDlgItem(IDC_MCP_COMMAND)->EnableWindow(!http);
    GetDlgItem(IDC_MCP_URL)->EnableWindow(http);
    GetDlgItem(IDC_MCP_TOKEN)->EnableWindow(http);
}

void CMcpEditDlg::OnOK()
{
    McpServerConfig c;
    c.name = TrimCopy(GetText(this, IDC_MCP_NAME));
    c.transport = static_cast<CComboBox*>(GetDlgItem(IDC_MCP_TRANSPORT))->GetCurSel() == 1 ? "http" : "stdio";
    c.command = TrimCopy(GetText(this, IDC_MCP_COMMAND));
    c.url = TrimCopy(GetText(this, IDC_MCP_URL));
    c.token = TrimCopy(GetText(this, IDC_MCP_TOKEN));
    c.enabled = IsDlgButtonChecked(IDC_MCP_ENABLED) == BST_CHECKED;

    if (c.name.empty())
    {
        AfxMessageBox(L"Give the server a name.", MB_ICONINFORMATION);
        return;
    }

    if (c.transport == "stdio" && c.command.empty())
    {
        AfxMessageBox(L"Enter the command that starts the server.", MB_ICONINFORMATION);
        return;
    }

    if (c.transport == "http" && c.url.rfind("http", 0) != 0)
    {
        AfxMessageBox(L"Enter the server's URL (http:// or https://).", MB_ICONINFORMATION);
        return;
    }

    cfg = c;
    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- MCP servers (list)

BEGIN_MESSAGE_MAP(CMcpServersDlg, CDialogEx)
    ON_BN_CLICKED(IDC_MCP_ADD, &CMcpServersDlg::OnAdd)
    ON_BN_CLICKED(IDC_MCP_EDIT, &CMcpServersDlg::OnEdit)
    ON_BN_CLICKED(IDC_MCP_REMOVE, &CMcpServersDlg::OnRemove)
    ON_BN_CLICKED(IDC_MCP_RECONNECT, &CMcpServersDlg::OnReconnect)
    ON_NOTIFY(NM_DBLCLK, IDC_MCP_LIST, &CMcpServersDlg::OnDblClk)
    ON_MESSAGE(WM_APP_MCP_CHANGED, &CMcpServersDlg::OnMcpChanged)
END_MESSAGE_MAP()

CMcpServersDlg::CMcpServersDlg(CWnd* parent, std::vector<McpServerConfig> c, McpManager& manager)
    : CDialogEx(IDD_MCP_SERVERS, parent)
    , configs(std::move(c))
    , manager_(manager)
{
}

BOOL CMcpServersDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    list_.SubclassDlgItem(IDC_MCP_LIST, this);
    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    CRect rc;
    list_.GetClientRect(&rc);
    int w = rc.Width();
    list_.InsertColumn(0, L"Name", LVCFMT_LEFT, w * 22 / 100);
    list_.InsertColumn(1, L"Command or URL", LVCFMT_LEFT, w * 40 / 100);
    list_.InsertColumn(2, L"Status", LVCFMT_LEFT, w * 26 / 100);
    list_.InsertColumn(3, L"Tools", LVCFMT_RIGHT, w * 12 / 100 - 4);
    Fill();
    return TRUE;
}

LRESULT CMcpServersDlg::OnMcpChanged(WPARAM, LPARAM)
{
    Fill();
    return 0;
}

void CMcpServersDlg::Fill()
{
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    list_.DeleteAllItems();
    auto servers = manager_.Servers();
    for (size_t i = 0; i < configs.size(); ++i)
    {
        const auto& c = configs[i];
        std::string state = c.enabled ? "not applied yet" : "off";
        std::string tools;
        for (const auto& s : servers)
        {
            if (s->Config().SameConnection(c))
            {
                auto info = s->Snapshot();
                state = info.state;
                if (info.state == "connected")
                {
                    tools = std::to_string(info.tools.size());
                }
            }
        }

        int row = list_.InsertItem(static_cast<int>(i), W(c.name));
        list_.SetItemText(row, 1, W(c.transport == "http" ? c.url : c.command));
        list_.SetItemText(row, 2, W(state));
        list_.SetItemText(row, 3, W(tools));
    }

    if (sel >= 0 && sel < list_.GetItemCount())
    {
        list_.SetItemState(sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

void CMcpServersDlg::OnAdd()
{
    McpServerConfig c;
    c.name = "server" + std::to_string(configs.size() + 1);
    CMcpEditDlg dlg(this, c);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    for (const auto& existing : configs)
    {
        if (Lower(existing.name) == Lower(dlg.cfg.name))
        {
            AfxMessageBox(L"A server with this name exists already.", MB_ICONINFORMATION);
            return;
        }
    }

    configs.push_back(dlg.cfg);
    Fill();
}

void CMcpServersDlg::OnEdit()
{
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(configs.size()))
    {
        return;
    }

    CMcpEditDlg dlg(this, configs[sel]);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    configs[sel] = dlg.cfg;
    Fill();
}

void CMcpServersDlg::OnDblClk(NMHDR*, LRESULT* result)
{
    OnEdit();
    *result = 0;
}

void CMcpServersDlg::OnRemove()
{
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(configs.size()))
    {
        return;
    }

    configs.erase(configs.begin() + sel);
    Fill();
}

void CMcpServersDlg::OnReconnect()
{
    reconnect = true;
    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- settings

BEGIN_MESSAGE_MAP(CSettingsDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BACKEND_HUB, &CSettingsDlg::OnBackend)
    ON_BN_CLICKED(IDC_BACKEND_CUSTOM, &CSettingsDlg::OnBackend)
    ON_BN_CLICKED(IDC_BROWSE_WORKSPACE, &CSettingsDlg::OnBrowse)
    ON_BN_CLICKED(IDC_OPEN_MANAGER, &CSettingsDlg::OnOpenManager)
    ON_BN_CLICKED(IDC_DATA_FOLDER, &CSettingsDlg::OnDataFolder)
END_MESSAGE_MAP()

CSettingsDlg::CSettingsDlg(CWnd* parent, const Settings& settings)
    : CDialogEx(IDD_SETTINGS, parent)
    , s(settings)
{
}

BOOL CSettingsDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    CheckRadioButton(IDC_BACKEND_HUB, IDC_BACKEND_CUSTOM, s.backend == "custom" ? IDC_BACKEND_CUSTOM : IDC_BACKEND_HUB);
    SetText(this, IDC_CUSTOM_URL, s.customUrl);
    SetText(this, IDC_CUSTOM_KEY, s.customKey);
    SetText(this, IDC_DEFAULT_WORKSPACE, s.defaultWorkspace);
    auto* shell = static_cast<CComboBox*>(GetDlgItem(IDC_SHELL));
    shell->AddString(L"cmd.exe");
    shell->AddString(L"PowerShell");
    shell->SetCurSel(s.shell == "powershell" ? 1 : 0);
    CheckDlgButton(IDC_CONFIRM_SHELL, s.confirmShell);
    CheckDlgButton(IDC_CONFIRM_WRITES, s.confirmWrites);
    CheckDlgButton(IDC_SHOW_REASONING, s.showReasoning);
    CheckDlgButton(IDC_AUTO_START_HUB, s.autoStartHub);
    SetDlgItemInt(IDC_MAX_ROUNDS, static_cast<UINT>(s.maxToolRounds), FALSE);
    SetText(this, IDC_TEMPERATURE, s.temperature);
    GetDlgItem(IDC_OPEN_MANAGER)->EnableWindow(!FindManagerExe().empty());
    OnBackend();
    return TRUE;
}

void CSettingsDlg::OnBackend()
{
    bool custom = IsDlgButtonChecked(IDC_BACKEND_CUSTOM) == BST_CHECKED;
    GetDlgItem(IDC_CUSTOM_URL)->EnableWindow(custom);
    GetDlgItem(IDC_CUSTOM_KEY)->EnableWindow(custom);
    GetDlgItem(IDC_AUTO_START_HUB)->EnableWindow(!custom);
}

void CSettingsDlg::OnBrowse()
{
    CFolderPickerDialog dlg(W(GetText(this, IDC_DEFAULT_WORKSPACE)), 0, this);
    if (dlg.DoModal() == IDOK)
    {
        SetDlgItemText(IDC_DEFAULT_WORKSPACE, dlg.GetPathName());
    }
}

void CSettingsDlg::OnOpenManager()
{
    std::wstring exe = FindManagerExe();
    if (!exe.empty())
    {
        ShellExecuteW(m_hWnd, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void CSettingsDlg::OnDataFolder()
{
    ShellExecuteW(m_hWnd, L"open", ChatHome().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CSettingsDlg::OnOK()
{
    std::string temperature = TrimCopy(GetText(this, IDC_TEMPERATURE));
    if (!temperature.empty())
    {
        char* end = nullptr;
        double t = std::strtod(temperature.c_str(), &end);
        if (*end != '\0' || t < 0 || t > 2)
        {
            AfxMessageBox(L"Temperature is a number from 0 to 2, or empty.", MB_ICONINFORMATION);
            return;
        }
    }

    s.backend = IsDlgButtonChecked(IDC_BACKEND_CUSTOM) == BST_CHECKED ? "custom" : "modelhub";
    s.customUrl = TrimCopy(GetText(this, IDC_CUSTOM_URL));
    s.customKey = TrimCopy(GetText(this, IDC_CUSTOM_KEY));
    s.defaultWorkspace = TrimCopy(GetText(this, IDC_DEFAULT_WORKSPACE));
    s.shell = static_cast<CComboBox*>(GetDlgItem(IDC_SHELL))->GetCurSel() == 1 ? "powershell" : "cmd";
    s.confirmShell = IsDlgButtonChecked(IDC_CONFIRM_SHELL) == BST_CHECKED;
    s.confirmWrites = IsDlgButtonChecked(IDC_CONFIRM_WRITES) == BST_CHECKED;
    s.showReasoning = IsDlgButtonChecked(IDC_SHOW_REASONING) == BST_CHECKED;
    s.autoStartHub = IsDlgButtonChecked(IDC_AUTO_START_HUB) == BST_CHECKED;
    s.maxToolRounds = std::max(1, static_cast<int>(GetDlgItemInt(IDC_MAX_ROUNDS, nullptr, FALSE)));
    s.temperature = temperature;
    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- Auto mode

CAutoDlg::CAutoDlg(CWnd* parent, const Settings& settings, std::vector<std::string> models)
    : CDialogEx(IDD_AUTO, parent)
    , s(settings)
    , models_(std::move(models))
{
}

BOOL CAutoDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    for (int i = 0; i < kCategoryCount; ++i)
    {
        auto* combo = static_cast<CComboBox*>(GetDlgItem(IDC_AUTO_CAT_FIRST + i));
        combo->AddString(L"Automatic (best for this PC)");
        int sel = 0;
        auto it = s.autoOverrides.find(CategoryId(static_cast<Category>(i)));
        for (const auto& m : models_)
        {
            int index = combo->AddString(W(DisplayModel(s, m)));
            if (it != s.autoOverrides.end() && it->second == m)
            {
                sel = index;
            }
        }

        combo->SetCurSel(sel);
    }

    CheckDlgButton(IDC_AUTO_ROUTER, s.autoRouterModel);
    CheckDlgButton(IDC_AUTO_STICKY, s.autoSticky);
    CheckDlgButton(IDC_AUTO_SUGGEST, s.autoSuggestDownloads);
    GetDlgItem(IDC_AUTO_SUGGEST)->EnableWindow(s.backend != "custom");
    return TRUE;
}

void CAutoDlg::OnOK()
{
    s.autoOverrides.clear();
    for (int i = 0; i < kCategoryCount; ++i)
    {
        auto* combo = static_cast<CComboBox*>(GetDlgItem(IDC_AUTO_CAT_FIRST + i));
        int sel = combo->GetCurSel();
        if (sel > 0 && sel <= static_cast<int>(models_.size()))
        {
            s.autoOverrides[CategoryId(static_cast<Category>(i))] = models_[static_cast<size_t>(sel - 1)];
        }
    }

    s.autoRouterModel = IsDlgButtonChecked(IDC_AUTO_ROUTER) == BST_CHECKED;
    s.autoSticky = IsDlgButtonChecked(IDC_AUTO_STICKY) == BST_CHECKED;
    s.autoSuggestDownloads = IsDlgButtonChecked(IDC_AUTO_SUGGEST) == BST_CHECKED;
    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- rename

CRenameDlg::CRenameDlg(CWnd* parent, const std::string& t)
    : CDialogEx(IDD_RENAME, parent)
    , text(t)
{
}

BOOL CRenameDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetText(this, IDC_RENAME_TEXT, text);
    auto* edit = static_cast<CEdit*>(GetDlgItem(IDC_RENAME_TEXT));
    edit->SetFocus();
    edit->SetSel(0, -1);
    return FALSE;
}

void CRenameDlg::OnOK()
{
    text = TrimCopy(GetText(this, IDC_RENAME_TEXT));
    if (!text.empty())
    {
        CDialogEx::OnOK();
    }
}

}  // namespace chat
