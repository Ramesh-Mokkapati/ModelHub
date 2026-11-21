// Model Providers: the list (add, edit, remove, detect) and one provider's settings (preset, API, URL, key, test).
#include "Dialogs.h"

#include "ClaudeSetup.h"
#include "Codex.h"
#include "Copilot.h"
#include "Providers.h"

#include <cstring>
#include "Util.h"

#include <cctype>

namespace chat
{

namespace
{

// The "API" list's entry for a provider kind.
int KindIndex(const std::string& kind)
{
    return kind == "anthropic" ? 0 : kind == "copilot" ? 2
                                 : kind == "codex"     ? 3
                                                       : 1;
}

std::string GetText(CWnd* dlg, int id)
{
    CString s;
    dlg->GetDlgItemText(id, s);
    return U8(s);
}

void SetText(CWnd* dlg, int id, const std::string& text)
{
    dlg->SetDlgItemText(id, W(text));
}

}  // namespace

// ---------------------------------------------------------------- one provider

BEGIN_MESSAGE_MAP(CProviderEditDlg, CDialogEx)
    ON_CBN_SELCHANGE(IDC_PE_PRESET, &CProviderEditDlg::OnPreset)
    ON_CBN_SELCHANGE(IDC_PE_KIND, &CProviderEditDlg::OnKind)
    ON_EN_CHANGE(IDC_PE_URL, &CProviderEditDlg::OnKind)
    ON_BN_CLICKED(IDC_PE_TEST, &CProviderEditDlg::OnTest)
    ON_BN_CLICKED(IDC_PE_KEYPAGE, &CProviderEditDlg::OnKeyPage)
    ON_BN_CLICKED(IDC_PE_SIGNIN, &CProviderEditDlg::OnSignIn)
    ON_EN_CHANGE(IDC_PE_KEYENV, &CProviderEditDlg::OnKind)
END_MESSAGE_MAP()

CProviderEditDlg::CProviderEditDlg(CWnd* parent, ProviderConfig cfg, bool isNew)
    : CDialogEx(IDD_PROVIDER_EDIT, parent)
    , p(std::move(cfg))
    , isNew_(isNew)
{
}

BOOL CProviderEditDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    auto* preset = static_cast<CComboBox*>(GetDlgItem(IDC_PE_PRESET));
    preset->AddString(L"(keep these settings)");
    for (const auto& pr : ProviderPresets())
    {
        preset->AddString(W(pr.label));
    }

    preset->SetCurSel(0);
    auto* kind = static_cast<CComboBox*>(GetDlgItem(IDC_PE_KIND));
    kind->AddString(L"Anthropic Messages API (Claude)");
    kind->AddString(L"OpenAI-compatible (/v1/chat/completions)");
    kind->AddString(L"GitHub Copilot (Copilot runtime; URL = GitHub host)");
    kind->AddString(L"ChatGPT sign-in (Codex CLI)");
    kind->SetCurSel(KindIndex(p.kind));
    SetText(this, IDC_PE_NAME, p.name);
    SetText(this, IDC_PE_URL, p.baseUrl);
    SetText(this, IDC_PE_KEY, p.apiKey);
    SetText(this, IDC_PE_KEYENV, p.apiKeyEnv);
    CheckDlgButton(IDC_PE_ENABLED, p.enabled);
    CheckDlgButton(IDC_PE_AUTO, p.inAuto);
    for (const auto& pr : ProviderPresets())
    {
        if (p.baseUrl == pr.baseUrl)
        {
            keyPage_ = pr.keyPage;
        }
    }

    if (isNew_)
    {
        SetWindowText(L"Add a Model Provider");
    }

    UpdateNotes();
    return TRUE;
}

void CProviderEditDlg::OnPreset()
{
    int sel = static_cast<CComboBox*>(GetDlgItem(IDC_PE_PRESET))->GetCurSel();
    if (sel <= 0 || sel > static_cast<int>(ProviderPresets().size()))
    {
        return;
    }

    const ProviderPreset& pr = ProviderPresets()[static_cast<size_t>(sel - 1)];
    if (isNew_)
    {
        p.id = pr.id;
    }

    SetDlgItemText(IDC_PE_NAME, W(pr.name));
    SetDlgItemText(IDC_PE_URL, W(pr.baseUrl));
    SetDlgItemText(IDC_PE_KEYENV, W(pr.keyEnv));
    std::string presetKind = pr.kind;
    static_cast<CComboBox*>(GetDlgItem(IDC_PE_KIND))->SetCurSel(KindIndex(presetKind));
    keyPage_ = pr.keyPage;
    ProviderConfig probe;
    probe.baseUrl = pr.baseUrl;
    CheckDlgButton(IDC_PE_AUTO, !probe.Cloud());  // on this PC: allowed in Auto mode; cloud: the user decides
    UpdateNotes();
}

void CProviderEditDlg::OnKind()
{
    UpdateNotes();
}

void CProviderEditDlg::UpdateNotes()
{
    ProviderConfig c;
    Read(c);
    GetDlgItem(IDC_PE_KEYPAGE)->EnableWindow(!keyPage_.empty());

    // Sign in through a command-line tool: the GitHub CLI (gh, gh:<host>) or Anthropic's CLI (ant).
    GetDlgItem(IDC_PE_SIGNIN)->EnableWindow(c.kind == "copilot" || c.kind == "codex" || c.apiKeyEnv == "gh" || c.apiKeyEnv.rfind("gh:", 0) == 0 || c.apiKeyEnv == "ant");

    // Copilot signs in through its own runtime: no key here.
    GetDlgItem(IDC_PE_KEY)->EnableWindow(c.kind != "copilot" && c.kind != "codex");
    GetDlgItem(IDC_PE_KEYENV)->EnableWindow(c.kind != "copilot" && c.kind != "codex");
    SetDlgItemText(IDC_PE_PRIVACY,
        c.Cloud() ? L"A cloud service: what you send to its models (your messages, attached files and tool results) "
                    L"goes to that company's servers and is billed to your account there. A key typed here is "
                    L"stored encrypted for your Windows account."
                  : L"A program on this PC: nothing leaves your computer. Start it before you use its models.");
}

bool CProviderEditDlg::Read(ProviderConfig& out)
{
    out = p;
    out.name = TrimCopy(GetText(this, IDC_PE_NAME));
    int kindSel = static_cast<CComboBox*>(GetDlgItem(IDC_PE_KIND))->GetCurSel();
    out.kind = kindSel == 0 ? "anthropic" : kindSel == 2 ? "copilot"
                                        : kindSel == 3   ? "codex"
                                                         : "openai";
    out.baseUrl = TrimCopy(GetText(this, IDC_PE_URL));
    out.apiKey = TrimCopy(GetText(this, IDC_PE_KEY));
    out.apiKeyEnv = TrimCopy(GetText(this, IDC_PE_KEYENV));
    out.enabled = IsDlgButtonChecked(IDC_PE_ENABLED) == BST_CHECKED;
    out.inAuto = IsDlgButtonChecked(IDC_PE_AUTO) == BST_CHECKED;
    return !out.name.empty() && out.baseUrl.rfind("http", 0) == 0 && out.baseUrl.find('<') == std::string::npos &&
           out.apiKeyEnv.find('<') == std::string::npos;
}

void CProviderEditDlg::OnTest()
{
    ProviderConfig c;
    if (!Read(c))
    {
        SetDlgItemText(IDC_PE_RESULT, L"Enter a name and a URL (http:// or https://), and replace any <org> or <host>.");
        return;
    }

    CWaitCursor wait;
    SetDlgItemText(IDC_PE_RESULT, L"Asking for its models...");
    UpdateWindow();
    std::vector<std::string> models;
    std::string error;
    if (ListProviderModels(c, models, error))
    {
        std::string names;
        for (size_t i = 0; i < models.size() && i < 4; ++i)
        {
            names += (i ? ", " : "") + models[i];
        }

        SetDlgItemText(IDC_PE_RESULT, W("Works: " + std::to_string(models.size()) + " models (" + names +
                                          (models.size() > 4 ? ", ..." : "") + ")"));
        p.models = models;
    }
    else
    {
        SetDlgItemText(IDC_PE_RESULT, W("Not reachable: " + error));
    }
}

void CProviderEditDlg::OnKeyPage()
{
    if (!keyPage_.empty())
    {
        ShellExecuteW(m_hWnd, L"open", Wide(keyPage_).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void CProviderEditDlg::OnSignIn()
{
    std::string source = TrimCopy(GetText(this, IDC_PE_KEYENV));
    std::wstring command;
    ProviderConfig current;
    Read(current);
    if (current.kind == "codex")
    {
        // ChatGPT: "codex login" opens the browser to sign in with the ChatGPT account.
        std::wstring cli = CodexExe();
        if (cli.empty())
        {
            SetDlgItemText(IDC_PE_RESULT, L"The Codex CLI was not found: install the Codex app (or: npm install -g @openai/codex).");
            return;
        }

        std::wstring params = L"/k echo Signing in to ChatGPT for ModelHub Chat... && \"" + cli + L"\" login";
        ShellExecuteW(m_hWnd, L"open", L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        SetDlgItemText(IDC_PE_RESULT, L"Finish the sign-in in your browser, then press Test.");
        return;
    }

    if (current.kind == "copilot")
    {
        // GitHub Copilot: "copilot login" for this host, with the CLI that comes with the Copilot runtime.
        std::wstring cli = CopilotCliExe();
        std::string host = current.baseUrl;
        bool safe = host.rfind("https://", 0) == 0 && host.find('<') == std::string::npos;
        for (char ch : host)
        {
            safe = safe && (std::isalnum(static_cast<unsigned char>(ch)) || std::strchr(".-:/", ch));
        }

        if (cli.empty() || !safe)
        {
            SetDlgItemText(IDC_PE_RESULT, cli.empty() ? L"The Copilot CLI was not found: install the GitHub Copilot app."
                                                      : L"Enter the GitHub host as the URL first, e.g. https://company.ghe.com.");
            return;
        }

        std::wstring login = L"\"" + cli + L"\" login" + (host == "https://github.com" ? L"" : L" --host " + Wide(host));
        std::wstring params = L"/k echo Signing in to GitHub Copilot for ModelHub Chat... && " + login;
        ShellExecuteW(m_hWnd, L"open", L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
        SetDlgItemText(IDC_PE_RESULT, L"Finish the sign-in in your browser, then press Test.");
        return;
    }

    if (source == "ant")
    {
        // Claude: the setup window installs Anthropic's CLI when it is missing, then signs in.
        CClaudeDlg dlg(this);
        dlg.DoModal();
        SetDlgItemText(IDC_PE_RESULT, dlg.signedIn ? L"Signed in to Claude: press Test." : L"Not signed in yet.");
        return;
    }

    if (source == "gh" || source.rfind("gh:", 0) == 0)
    {
        std::string host = source == "gh" ? "github.com" : source.substr(3);
        bool safe = !host.empty() && host.find('<') == std::string::npos;
        for (char ch : host)
        {
            safe = safe && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == ':');
        }

        if (!safe)
        {
            SetDlgItemText(IDC_PE_RESULT, L"Enter the GitHub host after \"gh:\" first, for example gh:company.ghe.com.");
            return;
        }

        command = L"gh auth login --hostname " + Wide(host) + L" --web";
    }
    else
    {
        return;
    }

    // A console window, so that the CLI can show its code and open the browser; it stays open to read the result.
    std::wstring params = L"/k echo Signing in for ModelHub Chat: " + command + L" && " + command;
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(m_hWnd, L"open", L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
    {
        SetDlgItemText(IDC_PE_RESULT, L"Could not open a command window. Run this yourself: " + CString(command.c_str()));
        return;
    }

    SetDlgItemText(IDC_PE_RESULT, L"Finish the sign-in in the window that opened (it may need the CLI installed), then press Test.");
}

void CProviderEditDlg::OnOK()
{
    ProviderConfig c;
    if (!Read(c))
    {
        AfxMessageBox(L"Enter a name and the URL of the API (http:// or https://), and replace any <org> or <host> "
                      L"with your organization and your GitHub Enterprise host (for example company.ghe.com).",
            MB_ICONINFORMATION);
        return;
    }

    p = c;
    CDialogEx::OnOK();
}

// ---------------------------------------------------------------- the list

BEGIN_MESSAGE_MAP(CProvidersDlg, CDialogEx)
    ON_BN_CLICKED(IDC_PROV_ADD, &CProvidersDlg::OnAdd)
    ON_BN_CLICKED(IDC_PROV_EDIT, &CProvidersDlg::OnEdit)
    ON_BN_CLICKED(IDC_PROV_REMOVE, &CProvidersDlg::OnRemove)
    ON_BN_CLICKED(IDC_PROV_DETECT, &CProvidersDlg::OnDetect)
    ON_NOTIFY(NM_DBLCLK, IDC_PROV_LIST, &CProvidersDlg::OnDblClk)
END_MESSAGE_MAP()

CProvidersDlg::CProvidersDlg(CWnd* parent, std::vector<ProviderConfig> list)
    : CDialogEx(IDD_PROVIDERS, parent)
    , providers(std::move(list))
{
}

BOOL CProvidersDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    list_.SubclassDlgItem(IDC_PROV_LIST, this);
    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    CRect rc;
    list_.GetClientRect(&rc);
    int w = rc.Width();
    list_.InsertColumn(0, L"Name", LVCFMT_LEFT, w * 22 / 100);
    list_.InsertColumn(1, L"Where", LVCFMT_LEFT, w * 40 / 100);
    list_.InsertColumn(2, L"Key", LVCFMT_LEFT, w * 14 / 100);
    list_.InsertColumn(3, L"Models", LVCFMT_RIGHT, w * 10 / 100);
    list_.InsertColumn(4, L"On / Auto", LVCFMT_LEFT, w * 14 / 100 - 4);
    CheckDlgButton(IDC_PROV_AUTOIMPORT, autoImport);
    Fill();
    return TRUE;
}

void CProvidersDlg::OnOK()
{
    autoImport = IsDlgButtonChecked(IDC_PROV_AUTOIMPORT) == BST_CHECKED;
    CDialogEx::OnOK();
}

void CProvidersDlg::Fill()
{
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    list_.DeleteAllItems();
    for (size_t i = 0; i < providers.size(); ++i)
    {
        const auto& p = providers[i];
        int row = list_.InsertItem(static_cast<int>(i), W(p.name));
        list_.SetItemText(row, 1, W(p.baseUrl));
        std::string key = p.kind == "copilot"                ? "Copilot sign-in"
                          : !p.apiKey.empty()                ? "saved"
                          : p.apiKeyEnv == "gh"              ? "gh sign-in"
                          : p.apiKeyEnv.rfind("gh:", 0) == 0 ? "gh: " + p.apiKeyEnv.substr(3)
                          : !p.apiKeyEnv.empty()             ? p.apiKeyEnv
                          : p.Cloud()                        ? "missing"
                                                             : "not needed";
        list_.SetItemText(row, 2, W(key));
        list_.SetItemText(row, 3, W(p.models.empty() ? std::string() : std::to_string(p.models.size())));
        list_.SetItemText(row, 4, W(std::string(p.enabled ? "on" : "off") + (p.inAuto ? ", Auto" : "")));
    }

    if (sel >= 0 && sel < list_.GetItemCount())
    {
        list_.SetItemState(sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

std::string CProvidersDlg::UniqueId(std::string base, int skip) const
{
    for (char& c : base)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
        {
            c = '-';
        }
    }

    base = Lower(base.empty() ? std::string("provider") : base);
    std::string id = base;
    for (int n = 2;; ++n)
    {
        bool taken = false;
        for (size_t i = 0; i < providers.size(); ++i)
        {
            taken = taken || (static_cast<int>(i) != skip && providers[i].id == id);
        }

        if (!taken)
        {
            return id;
        }

        id = base + std::to_string(n);
    }
}

void CProvidersDlg::OnAdd()
{
    const ProviderPreset& first = ProviderPresets().front();
    ProviderConfig c;
    c.id = first.id;
    c.name = first.name;
    c.kind = first.kind;
    c.baseUrl = first.baseUrl;
    c.apiKeyEnv = first.keyEnv;
    CProviderEditDlg dlg(this, c, true);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    dlg.p.id = UniqueId(dlg.p.id.empty() ? dlg.p.name : dlg.p.id);
    providers.push_back(dlg.p);
    Fill();
}

void CProvidersDlg::OnEdit()
{
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(providers.size()))
    {
        return;
    }

    CProviderEditDlg dlg(this, providers[static_cast<size_t>(sel)], false);
    if (dlg.DoModal() != IDOK)
    {
        return;
    }

    providers[static_cast<size_t>(sel)] = dlg.p;
    Fill();
}

void CProvidersDlg::OnDblClk(NMHDR*, LRESULT* result)
{
    OnEdit();
    *result = 0;
}

void CProvidersDlg::OnRemove()
{
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    if (sel < 0 || sel >= static_cast<int>(providers.size()))
    {
        return;
    }

    providers.erase(providers.begin() + sel);
    Fill();
}

void CProvidersDlg::OnDetect()
{
    CWaitCursor wait;
    SetDlgItemText(IDC_PROV_NOTE, L"Looking for API keys, the GitHub CLI and model servers on this PC...");
    UpdateWindow();
    Detection d = DetectProviders();
    std::string added, skipped;
    for (auto& f : d.found)
    {
        bool have = false;
        for (const auto& p : providers)
        {
            have = have || p.baseUrl == f.config.baseUrl;
        }

        if (have)
        {
            skipped += "\n  " + f.config.name + ": already in the list";
            continue;
        }

        f.config.id = UniqueId(f.config.id);
        providers.push_back(f.config);
        added += "\n  " + f.config.name + ": " + f.note;
    }

    Fill();
    std::string msg = added.empty() ? "Nothing new found." : "Added:" + added;
    if (!skipped.empty())
    {
        msg += "\n\nAlready set up:" + skipped;
    }

    for (const auto& n : d.notes)
    {
        msg += "\n\n" + n;
    }

    msg += "\n\nAuto mode uses a cloud provider's models only after you allow it (Edit > \"Auto mode may use its "
           "models\").";
    SetDlgItemText(IDC_PROV_NOTE, added.empty() ? L"Detect found nothing new." : L"Detect added providers: check them, then press OK.");
    AfxMessageBox(W(msg), MB_ICONINFORMATION);
}

}  // namespace chat
