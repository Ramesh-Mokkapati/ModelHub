#pragma once
#include "stdafx.h"

#include "resource.h"

namespace chat
{

// Installs Anthropic's CLI (ant) for this user: the latest Windows release from
// github.com/anthropics/anthropic-cli for this PC's processor, checked against the release's SHA-256 checksums,
// into <ModelHub home>\tools\ant. progress hears each step. Returns the installed ant.exe, or "" with an error.
std::wstring InstallAnt(const std::function<void(const std::string&)>& progress, std::string& error);

// Claude models through the Claude API: the Claude app's subscription stays in Anthropic's apps, so this window
// installs Anthropic's CLI when needed and signs in with an Anthropic Console account (ant auth login).
class CClaudeDlg : public CDialogEx
{
public:
    explicit CClaudeDlg(CWnd* parent);
    bool signedIn = false;

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnInstall();
    afx_msg void OnSignIn();
    afx_msg void OnKeyPage();
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnDestroy();
    afx_msg LRESULT OnProgress(WPARAM finished, LPARAM text);
    DECLARE_MESSAGE_MAP()

private:
    void UpdateState();
    bool installing_ = false;
};

}  // namespace chat
