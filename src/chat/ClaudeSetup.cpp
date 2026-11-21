#include "ClaudeSetup.h"

#include "AppMessages.h"
#include "Http.h"
#include "Util.h"

#include <bcrypt.h>

#include <filesystem>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace chat
{

namespace
{

constexpr UINT_PTR kSignInTimer = 1;

std::string Sha256Hex(const std::string& data)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    unsigned char hash[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    {
        return {};
    }

    BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()),
        hash, sizeof hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    static const char* digits = "0123456789abcdef";
    std::string hex;

    for (unsigned char b : hash)
    {
        hex += digits[b >> 4];
        hex += digits[b & 15];
    }

    return hex;
}

const char* Arch()
{
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_ARM64:
        return "arm64";
    case PROCESSOR_ARCHITECTURE_INTEL:
        return "386";
    default:
        return "amd64";
    }
}

}  // namespace

std::wstring InstallAnt(const std::function<void(const std::string&)>& progress, std::string& error)
{
    HttpHeaders github = { { "Accept", "application/vnd.github+json" } };
    progress("Looking up the latest release of Anthropic's CLI on GitHub...");
    HttpResult rel = HttpSend(L"GET", "https://api.github.com/repos/anthropics/anthropic-cli/releases/latest", "", github, 30000);

    json release = json::parse(rel.Ok() ? rel.body : std::string("null"), nullptr, false);
    if (!release.is_object() || !release.contains("assets"))
    {
        error = "cannot read the release list from GitHub (" + (rel.status ? "HTTP " + std::to_string(rel.status) : rel.error) + ")";
        return {};
    }

    std::string version = release.value("tag_name", "");
    std::string suffix = std::string("_windows_") + Arch() + ".zip";
    std::string zipName, zipUrl, sumsUrl;
    for (const auto& a : release["assets"])
    {
        std::string name = a.value("name", "");
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            zipName = name;
            zipUrl = a.value("browser_download_url", "");
        }
        else if (name.find("checksums") != std::string::npos)
        {
            sumsUrl = a.value("browser_download_url", "");
        }
    }

    if (zipUrl.empty() || sumsUrl.empty())
    {
        error = "release " + version + " has no Windows " + Arch() + " download with checksums";
        return {};
    }

    progress("Downloading " + zipName + "...");
    HttpResult sums = HttpSend(L"GET", sumsUrl, "", {}, 60000);
    HttpResult zip = HttpSend(L"GET", zipUrl, "", {}, 300000);
    if (!sums.Ok() || !zip.Ok())
    {
        error = "the download failed (" + (zip.Ok() ? sums.error : zip.error) + ")";
        return {};
    }

    // checksums.txt: "<sha256>  <file name>" per line.
    std::string expected;
    std::istringstream lines(sums.body);
    for (std::string line; std::getline(lines, line);)
    {
        std::istringstream words(line);
        std::string hash, name;
        words >> hash >> name;
        if (name == zipName)
        {
            expected = Lower(hash);
        }
    }

    std::string actual = Sha256Hex(zip.body);
    if (expected.size() != 64 || actual != expected)
    {
        error = "the download does not match the release's SHA-256 checksum: not installed";
        return {};
    }

    progress("Checksum verified. Installing...");
    std::wstring dir = AntToolsDir();
    std::error_code ec;
    fs::create_directories(dir, ec);

    std::wstring zipPath = dir + L"\\" + Wide(zipName);
    if (!WriteFileAtomic(zipPath, zip.body))
    {
        error = "cannot write " + Utf8(zipPath);
        return {};
    }

    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    RunCapture(L"\"" + std::wstring(system) + L"\\tar.exe\" -xf \"" + zipPath + L"\" -C \"" + dir + L"\"", 60000);
    DeleteFileW(zipPath.c_str());

    std::wstring exe = dir + L"\\ant.exe";
    if (!fs::exists(exe, ec))
    {
        for (const auto& e : fs::recursive_directory_iterator(dir, ec))
        {
            if (e.path().filename() == L"ant.exe")
            {
                fs::rename(e.path(), exe, ec);
                break;
            }
        }
    }

    std::string ver = TrimCopy(RunCapture(L"\"" + exe + L"\" --version", 15000));
    if (!fs::exists(exe, ec) || ver.empty())
    {
        error = "ant.exe did not run after unpacking " + zipName;
        return {};
    }

    progress("Installed Anthropic's CLI " + version + " in " + Utf8(dir));
    return exe;
}

// ---------------------------------------------------------------- the window

BEGIN_MESSAGE_MAP(CClaudeDlg, CDialogEx)
    ON_BN_CLICKED(IDC_CL_INSTALL, &CClaudeDlg::OnInstall)
    ON_BN_CLICKED(IDC_CL_SIGNIN, &CClaudeDlg::OnSignIn)
    ON_BN_CLICKED(IDC_CL_KEYPAGE, &CClaudeDlg::OnKeyPage)
    ON_WM_TIMER()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_APP_CLAUDE_PROGRESS, &CClaudeDlg::OnProgress)
END_MESSAGE_MAP()

CClaudeDlg::CClaudeDlg(CWnd* parent)
    : CDialogEx(IDD_CLAUDE, parent)
{
}

BOOL CClaudeDlg::OnInitDialog()
{
    CDialogEx::OnInitDialog();
    SetDlgItemText(IDC_CL_INTRO,
        L"Claude's Pro and Max subscriptions work only in Anthropic's own apps. ModelHub Chat reaches Claude models "
        L"through the Claude API instead: sign in with your Anthropic Console account (usage is billed to it), using "
        L"Anthropic's command-line tool, ant. Messages you send to Claude leave this PC.");
    UpdateState();
    return TRUE;
}

void CClaudeDlg::UpdateState()
{
    std::wstring exe = AntExe();
    signedIn = !exe.empty() && !AntToken().empty();
    CString state;
    if (installing_)
    {
        return;
    }

    if (exe.empty())
    {
        state = L"1. Anthropic's CLI is not installed. Install it (from github.com/anthropics/anthropic-cli, checked by "
                L"its SHA-256), then sign in.";
    }
    else if (!signedIn)
    {
        state = L"Anthropic's CLI is ready (" + CString(exe.c_str()) + L").\r\n2. Sign in: a browser opens to your "
                                                                       L"Anthropic Console account.";
    }
    else
    {
        state = L"Signed in. Claude models appear in the model list (Model Providers: \"Claude\").";
    }

    SetDlgItemText(IDC_CL_STATE, state);
    GetDlgItem(IDC_CL_INSTALL)->EnableWindow(exe.empty() && !installing_);
    GetDlgItem(IDC_CL_SIGNIN)->EnableWindow(!exe.empty() && !signedIn && !installing_);
    SetDlgItemText(IDOK, signedIn ? L"Done" : L"Close");
}

void CClaudeDlg::OnInstall()
{
    installing_ = true;
    GetDlgItem(IDC_CL_INSTALL)->EnableWindow(FALSE);
    HWND hwnd = m_hWnd;
    auto post = [hwnd](const std::string& text, bool finished)
    {
        auto* s = new std::string(text);
        if (!::PostMessage(hwnd, WM_APP_CLAUDE_PROGRESS, finished ? 1 : 0, reinterpret_cast<LPARAM>(s)))
        {
            delete s;
        }
    };

    auto work = [post]()
    {
        std::string error;
        std::wstring exe = InstallAnt([post](const std::string& s)
            { post(s, false); }, error);
        post(exe.empty() ? "Not installed: " + error : "Installed. Now sign in.", true);
    };

    std::thread(work).detach();
}

LRESULT CClaudeDlg::OnProgress(WPARAM finished, LPARAM text)
{
    std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(text));
    if (finished)
    {
        installing_ = false;
        UpdateState();
    }

    SetDlgItemText(IDC_CL_STATE, W(*s) + (finished && !AntExe().empty() ? L"\r\n\r\n2. Sign in: a browser opens to your Anthropic Console account." : L""));
    return 0;
}

void CClaudeDlg::OnSignIn()
{
    std::wstring exe = AntExe();
    if (exe.empty())
    {
        return;
    }

    // A console window: ant opens the browser and waits for the sign-in there.
    std::wstring params = L"/k echo Signing in to Claude for ModelHub Chat... && \"" + exe + L"\" auth login";
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(m_hWnd, L"open", L"cmd.exe", params.c_str(), nullptr, SW_SHOWNORMAL)) <= 32)
    {
        SetDlgItemText(IDC_CL_STATE, L"Could not open a command window. Run this yourself: \"" + CString(exe.c_str()) + L"\" auth login");
        return;
    }

    SetDlgItemText(IDC_CL_STATE, L"Finish the sign-in in your browser. This window notices when it is done.");
    SetTimer(kSignInTimer, 3000, nullptr);
}

void CClaudeDlg::OnTimer(UINT_PTR id)
{
    if (id != kSignInTimer)
    {
        return CDialogEx::OnTimer(id);
    }

    if (!AntToken().empty())
    {
        KillTimer(kSignInTimer);
        UpdateState();
    }
}

void CClaudeDlg::OnKeyPage()
{
    ShellExecuteW(m_hWnd, L"open", L"https://console.anthropic.com/settings/keys", nullptr, nullptr, SW_SHOWNORMAL);
}

void CClaudeDlg::OnDestroy()
{
    KillTimer(kSignInTimer);
    CDialogEx::OnDestroy();
}

}  // namespace chat
